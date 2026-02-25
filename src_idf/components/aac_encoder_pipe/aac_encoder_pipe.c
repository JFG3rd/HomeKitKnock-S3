/**
 * AAC Encoder Pipeline
 *
 * Port of src_arduino/aac_stream.cpp to pure C ESP-IDF.
 * ESP-ADF pipeline: raw_stream_writer → aac_encoder → raw_stream_reader
 */

#include "aac_encoder_pipe.h"
#include "audio_capture.h"
#include "config.h"
#include "nvs_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "audio_pipeline.h"
#include "raw_stream.h"
#include "aac_encoder.h"
#include "audio_element.h"

static const char *TAG = "aac_pipe";

#define NVS_CAMERA_NAMESPACE "camera"
#define NVS_KEY_AAC_RATE     "aac_rate"
#define NVS_KEY_AAC_BITRATE  "aac_bitr"

#define MAX_MIC_SAMPLES  2048
#define STASH_SIZE       4096
#define MAX_FRAME_BYTES  2048

static SemaphoreHandle_t aac_mutex = NULL;

static uint32_t sample_rate_hz = 16000;
static uint32_t bitrate_bps = 32000;

static bool aac_ready = false;
static bool aac_init_failed = false;  // Latch: don't retry after failure
static audio_pipeline_handle_t pipeline = NULL;
static audio_element_handle_t raw_writer = NULL;
static audio_element_handle_t aac_enc = NULL;
static audio_element_handle_t raw_reader = NULL;

static uint8_t stash[STASH_SIZE];
static size_t stash_len = 0;

static int16_t mic_buf[MAX_MIC_SAMPLES];
static int16_t pcm_frame[AAC_FRAME_SAMPLES];

// --- Helpers ---

static uint8_t freq_index_from_rate(uint32_t rate) {
    switch (rate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000:  return 11;
        case 7350:  return 12;
        default:    return 8;
    }
}

static bool parse_adts_header(const uint8_t *data, size_t len,
                               size_t *frame_len, size_t *header_len) {
    if (!data || len < 7) return false;
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) return false;

    bool protection_absent = data[1] & 0x01;
    *frame_len = ((data[3] & 0x03) << 11) |
                 ((size_t)data[4] << 3) |
                 ((data[5] & 0xE0) >> 5);
    *header_len = protection_absent ? 7 : 9;

    return *frame_len >= *header_len;
}

static void downsample(const int16_t *in, size_t in_samples,
                        int16_t *out, size_t out_samples,
                        uint32_t in_rate, uint32_t out_rate) {
    if (in_rate == out_rate) {
        size_t copy = in_samples < out_samples ? in_samples : out_samples;
        memcpy(out, in, copy * sizeof(int16_t));
        if (copy < out_samples) {
            memset(out + copy, 0, (out_samples - copy) * sizeof(int16_t));
        }
        return;
    }

    size_t step = in_rate / out_rate;
    if (step < 1) step = 1;

    for (size_t i = 0; i < out_samples; i++) {
        size_t idx = i * step;
        out[i] = idx < in_samples ? in[idx] : 0;
    }
}

// --- Pipeline management ---

static void cleanup_partial_pipeline(void) {
    // Pipeline may be partially running (some element tasks created, others not).
    // Must stop/terminate before unregistering and deiniting to avoid spinlock asserts.
    if (pipeline) {
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
        audio_pipeline_terminate(pipeline);
        if (raw_writer) audio_pipeline_unregister(pipeline, raw_writer);
        if (aac_enc)    audio_pipeline_unregister(pipeline, aac_enc);
        if (raw_reader) audio_pipeline_unregister(pipeline, raw_reader);
    }
    if (raw_writer) { audio_element_deinit(raw_writer); raw_writer = NULL; }
    if (aac_enc)    { audio_element_deinit(aac_enc);    aac_enc = NULL; }
    if (raw_reader) { audio_element_deinit(raw_reader); raw_reader = NULL; }
    if (pipeline)   { audio_pipeline_deinit(pipeline);  pipeline = NULL; }
}

static bool init_pipeline(void) {
    if (aac_ready) return true;
    if (aac_init_failed) return false;  // Don't retry after permanent failure

    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipe_cfg);
    if (!pipeline) {
        ESP_LOGE(TAG, "Pipeline init failed");
        goto fail;
    }

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 4 * 1024;
    raw_writer = raw_stream_init(&raw_cfg);
    if (!raw_writer) {
        ESP_LOGE(TAG, "Raw writer init failed");
        goto fail;
    }

    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = 4 * 1024;
    raw_reader = raw_stream_init(&raw_cfg);
    if (!raw_reader) {
        ESP_LOGE(TAG, "Raw reader init failed");
        goto fail;
    }

    aac_encoder_cfg_t aac_cfg = DEFAULT_AAC_ENCODER_CONFIG();
    aac_cfg.sample_rate = (int)sample_rate_hz;
    aac_cfg.channel = 1;
    aac_cfg.bitrate = (int)bitrate_bps;
    aac_cfg.task_core = STREAM_TASK_CORE;
    aac_cfg.out_rb_size = 4 * 1024;
    aac_enc = aac_encoder_init(&aac_cfg);
    if (!aac_enc) {
        ESP_LOGE(TAG, "AAC encoder init failed");
        goto fail;
    }

    audio_pipeline_register(pipeline, raw_writer, "raw_in");
    audio_pipeline_register(pipeline, aac_enc, "aac");
    audio_pipeline_register(pipeline, raw_reader, "raw_out");

    const char *link_tag[3] = {"raw_in", "aac", "raw_out"};
    audio_pipeline_link(pipeline, link_tag, 3);

    audio_element_set_input_timeout(raw_writer, pdMS_TO_TICKS(50));
    audio_element_set_output_timeout(raw_reader, pdMS_TO_TICKS(50));

    if (audio_pipeline_run(pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Pipeline run failed");
        goto fail;
    }

    stash_len = 0;
    aac_ready = true;
    ESP_LOGI(TAG, "AAC pipeline started (%lu Hz, %lu bps)", sample_rate_hz, bitrate_bps);
    return true;

fail:
    ESP_LOGE(TAG, "AAC pipeline init permanently failed — will not retry");
    cleanup_partial_pipeline();
    aac_init_failed = true;
    return false;
}

static void deinit_pipeline(void) {
    if (!aac_ready) return;

    if (pipeline) {
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
        audio_pipeline_terminate(pipeline);
    }
    if (pipeline && raw_writer) audio_pipeline_unregister(pipeline, raw_writer);
    if (pipeline && aac_enc)    audio_pipeline_unregister(pipeline, aac_enc);
    if (pipeline && raw_reader) audio_pipeline_unregister(pipeline, raw_reader);

    if (raw_writer) { audio_element_deinit(raw_writer); raw_writer = NULL; }
    if (aac_enc)    { audio_element_deinit(aac_enc);    aac_enc = NULL; }
    if (raw_reader) { audio_element_deinit(raw_reader); raw_reader = NULL; }
    if (pipeline)   { audio_pipeline_deinit(pipeline);  pipeline = NULL; }

    aac_ready = false;
    stash_len = 0;
}

// --- Read encoded frame from pipeline output ---

static bool read_encoded_frame(uint8_t *out, size_t out_max, size_t *out_len) {
    *out_len = 0;
    if (!raw_reader || !out || out_max == 0) return false;

    uint32_t start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint8_t temp[512];

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS) - start_ms < 80) {
        int rd = raw_stream_read(raw_reader, (char *)temp, sizeof(temp));
        if (rd > 0) {
            size_t copy = (size_t)rd;
            if (copy > STASH_SIZE - stash_len) copy = STASH_SIZE - stash_len;
            memcpy(stash + stash_len, temp, copy);
            stash_len += copy;
        }

        if (stash_len < 7) {
            vTaskDelay(1);
            continue;
        }

        size_t frame_len, header_len;
        if (parse_adts_header(stash, stash_len, &frame_len, &header_len)) {
            if (stash_len < frame_len) {
                vTaskDelay(1);
                continue;
            }
            size_t raw_len = frame_len - header_len;
            if (raw_len > out_max) raw_len = out_max;
            memcpy(out, stash + header_len, raw_len);
            *out_len = raw_len;

            size_t remaining = stash_len - frame_len;
            if (remaining > 0) {
                memmove(stash, stash + frame_len, remaining);
            }
            stash_len = remaining;
            return *out_len > 0;
        }

        // No valid ADTS sync — flush stash as raw
        if (stash_len > 0) {
            size_t raw_len = stash_len < out_max ? stash_len : out_max;
            memcpy(out, stash, raw_len);
            *out_len = raw_len;
            stash_len = 0;
            return *out_len > 0;
        }

        vTaskDelay(1);
    }

    return false;
}

// --- Public API ---

esp_err_t aac_encoder_pipe_init(void) {
    if (aac_mutex) return ESP_OK; // Already initialized

    aac_mutex = xSemaphoreCreateMutex();
    if (!aac_mutex) return ESP_ERR_NO_MEM;

    // Read config from NVS
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t val;
        if (nvs_get_u8(handle, NVS_KEY_AAC_RATE, &val) == ESP_OK) {
            sample_rate_hz = (val == 8) ? 8000 : 16000;
        }
        if (nvs_get_u8(handle, NVS_KEY_AAC_BITRATE, &val) == ESP_OK) {
            bitrate_bps = (uint32_t)val * 1000;
        }
        nvs_close(handle);
    }

    if (bitrate_bps < 16000) bitrate_bps = 32000;
    if (bitrate_bps > 48000) bitrate_bps = 32000;

    ESP_LOGI(TAG, "AAC encoder pipe initialized (%lu Hz, %lu bps)",
             sample_rate_hz, bitrate_bps);
    return ESP_OK;
}

void aac_encoder_pipe_deinit(void) {
    if (aac_mutex) {
        xSemaphoreTake(aac_mutex, portMAX_DELAY);
        deinit_pipeline();
        xSemaphoreGive(aac_mutex);
    }
}

bool aac_encoder_pipe_get_frame(uint8_t *out, size_t out_max, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!out || out_max == 0 || !out_len) return false;

    if (!xSemaphoreTake(aac_mutex, pdMS_TO_TICKS(200))) return false;

    // Lazy-init pipeline on first frame request
    if (!aac_ready) {
        if (!init_pipeline()) {
            xSemaphoreGive(aac_mutex);
            return false;
        }
    }

    // Calculate how many mic samples we need
    size_t input_samples = (AUDIO_SAMPLE_RATE / sample_rate_hz) * AAC_FRAME_SAMPLES;
    if (input_samples > MAX_MIC_SAMPLES) input_samples = MAX_MIC_SAMPLES;

    // Capture mic samples
    bool captured = audio_capture_read(mic_buf, input_samples, 80);
    if (!captured) {
        memset(mic_buf, 0, input_samples * sizeof(int16_t));
    }

    // Downsample if needed (e.g., 16kHz mic → 8kHz AAC)
    downsample(mic_buf, input_samples, pcm_frame, AAC_FRAME_SAMPLES,
               AUDIO_SAMPLE_RATE, sample_rate_hz);

    // Feed PCM to pipeline
    int written = raw_stream_write(raw_writer, (char *)pcm_frame,
                                    AAC_FRAME_SAMPLES * sizeof(int16_t));
    if (written <= 0) {
        xSemaphoreGive(aac_mutex);
        return false;
    }

    // Read encoded output
    bool ok = read_encoded_frame(out, out_max, out_len);
    xSemaphoreGive(aac_mutex);
    return ok;
}

uint32_t aac_encoder_pipe_get_sample_rate(void) {
    return sample_rate_hz;
}

void aac_encoder_pipe_get_sdp_rtpmap(char *buf, size_t sz) {
    snprintf(buf, sz, "MPEG4-GENERIC/%lu/1", sample_rate_hz);
}

void aac_encoder_pipe_get_sdp_fmtp(char *buf, size_t sz) {
    uint8_t freq_idx = freq_index_from_rate(sample_rate_hz);
    // AudioSpecificConfig: AAC-LC profile=2, freq_idx, 1 channel
    uint16_t asc = (uint16_t)((2 << 11) | (freq_idx << 7) | (1 << 3));
    snprintf(buf, sz,
             "profile-level-id=1;mode=AAC-hbr;config=%04X"
             ";SizeLength=13;IndexLength=3;IndexDeltaLength=3",
             asc);
}
