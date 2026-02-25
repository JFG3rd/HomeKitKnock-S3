/**
 * Audio Output Component
 *
 * MAX98357A I2S DAC speaker on I2S_NUM_1.
 * Gong: embedded PCM from flash, falls back to synthesized 2-tone.
 */

#include "audio_output.h"
#include "audio_capture.h"
#include "i2s_shared_bus.h"
#include "config.h"
#include "nvs_manager.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio_output";

#define NVS_CAMERA_NAMESPACE "camera"
#define NVS_KEY_AUD_VOLUME   "aud_volume"
#define NVS_KEY_HW_DIAG      "hw_diag"

#define I2S_DMA_BUF_COUNT    6
#define I2S_DMA_BUF_SAMPLES  256
#define AUDIO_TASK_PRIORITY   5
#define AUDIO_TASK_STACK_SIZE 8192
#define TX_WRITE_TIMEOUT_MS   400

// Gong PCM peaks at ~78% full scale. This headroom factor scales it so that
// volume=100% outputs ~15.7% of full scale, staying below the speaker
// distortion threshold. Raise this if the speaker is larger/quieter.
#define GONG_PCM_HEADROOM_PCT 20

// Embedded gong PCM (from gong_data.c, generated from data/gong.pcm)
extern const uint8_t gong_pcm_data[];
extern const size_t gong_pcm_data_size;

static i2s_chan_handle_t tx_channel = NULL;
static SemaphoreHandle_t output_mutex = NULL;
static uint8_t volume = 70;
static bool initialized = false;
static bool tx_enabled = false;
static bool tx_from_shared_bus = false;
static bool gong_task_running = false;
static i2s_port_t tx_port = I2S_NUM_1;
static bool hardware_diagnostic_mode = false;
static uint32_t diag_writes_ok = 0;
static uint32_t diag_writes_timeout = 0;
static uint32_t diag_writes_other_err = 0;
static uint32_t diag_zero_bytes = 0;
static uint64_t diag_bytes_written = 0;

static esp_err_t create_tx_channel(void) {
    // Use shared bus: GPIO7 (BCLK) and GPIO8 (WS) are physically shared between
    // MAX98357A (TX/DOUT=GPIO9) and INMP441 (RX/DIN=GPIO12). The shared bus
    // component creates both channels simultaneously on I2S_NUM_1.
    esp_err_t err = i2s_shared_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    tx_channel = i2s_shared_bus_get_tx_channel();
    if (!tx_channel) {
        ESP_LOGE(TAG, "Shared bus TX channel not available");
        return ESP_FAIL;
    }

    // Channel may already be enabled if audio_capture initialized shared bus first.
    err = i2s_channel_enable(tx_channel);
    if (err == ESP_ERR_INVALID_STATE) {
        i2s_channel_disable(tx_channel);
        err = i2s_channel_enable(tx_channel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker TX enable failed: %s", esp_err_to_name(err));
        tx_channel = NULL;
        return err;
    }

    tx_enabled = true;
    tx_from_shared_bus = true;
    ESP_LOGI(TAG, "Speaker TX via shared bus (I2S1 BCLK=%d WS=%d DOUT=%d, vol=%d%%)",
             I2S_DAC_BCLK, I2S_DAC_LRCLK, I2S_DAC_DOUT, volume);
    return ESP_OK;
}

static esp_err_t rebuild_tx_channel(void) {
    if (tx_channel) {
        if (tx_enabled) {
            i2s_channel_disable(tx_channel);
            tx_enabled = false;
        }
        if (!tx_from_shared_bus) {
            i2s_del_channel(tx_channel);
        }
        tx_channel = NULL;
        tx_from_shared_bus = false;
    }
    return create_tx_channel();
}

static esp_err_t ensure_tx_enabled(void) {
    if (!tx_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx_enabled) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(tx_channel);
    if (err == ESP_ERR_INVALID_STATE) {
        i2s_channel_disable(tx_channel);
        err = i2s_channel_enable(tx_channel);
    }

    if (err == ESP_OK) {
        tx_enabled = true;
    } else {
        ESP_LOGW(TAG, "TX enable failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void disable_tx_channel(void) {
    if (tx_channel && tx_enabled) {
        // INMP441 uses TX as its BCLK source. Disabling TX would silence GPIO7
        // and cause every subsequent capture read to return zeros until TX is
        // re-enabled. Keep TX running (outputting DMA silence) while INMP441 is active.
        if (audio_capture_is_running() && audio_capture_get_source() == MIC_SOURCE_INMP441) {
            ESP_LOGD(TAG, "TX kept active: INMP441 capture needs BCLK");
            return;
        }
        i2s_channel_disable(tx_channel);
        tx_enabled = false;
    }
}

void audio_output_flush_and_stop(void) {
    if (!initialized || !tx_channel || !tx_enabled) return;
    // Flush DMA circular buffers with silence — same teardown as gong_task.
    int16_t stereo_silence[512];  // 256 stereo pairs
    memset(stereo_silence, 0, sizeof(stereo_silence));
    size_t bw = 0;
    for (int i = 0; i < I2S_DMA_BUF_COUNT + 2; i++) {
        i2s_channel_write(tx_channel, stereo_silence, sizeof(stereo_silence),
                          &bw, pdMS_TO_TICKS(TX_WRITE_TIMEOUT_MS));
    }
    disable_tx_channel();
    ESP_LOGI(TAG, "TX flushed and stopped");
}

static void load_nvs_config(void) {
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t val = 70;
    if (nvs_get_u8(handle, NVS_KEY_AUD_VOLUME, &val) == ESP_OK) {
        volume = val;
    }

    val = 0;
    if (nvs_get_u8(handle, NVS_KEY_HW_DIAG, &val) == ESP_OK) {
        hardware_diagnostic_mode = (val != 0);
    }
    nvs_close(handle);
}

void audio_output_set_hardware_diagnostic_mode(bool enabled) {
    hardware_diagnostic_mode = enabled;
    // Non-diag: INFO (ERROR+WARN+INFO visible). Diag: DEBUG (all levels visible).
    esp_log_level_t lvl = enabled ? ESP_LOG_DEBUG : ESP_LOG_INFO;
    esp_log_level_set("audio_output",     lvl);
    esp_log_level_set("audio_capture",    lvl);
    esp_log_level_set("aac_encoder_pipe", lvl);
    esp_log_level_set("i2s_shared_bus",   lvl);
    esp_log_level_set("sip",  enabled ? ESP_LOG_DEBUG : ESP_LOG_WARN);
    esp_log_level_set("rtsp", enabled ? ESP_LOG_DEBUG : ESP_LOG_WARN);
    ESP_LOGI(TAG, "Hardware diagnostic mode %s — audio log level → %s",
             enabled ? "enabled" : "disabled",
             enabled ? "DEBUG" : "INFO");
}

bool audio_output_get_hardware_diagnostic_mode(void) {
    return hardware_diagnostic_mode;
}

bool audio_output_is_available(void) {
    return true;
}

bool audio_output_is_initialized(void) {
    return initialized;
}

esp_err_t audio_output_init(void) {
    if (initialized) return ESP_OK;

    output_mutex = xSemaphoreCreateMutex();
    if (!output_mutex) return ESP_ERR_NO_MEM;

    load_nvs_config();

    // TX channel is deferred to the first create_tx_channel() call (lazy init via
    // audio_output_write() or prepare_exclusive_playback()). This ensures the shared
    // bus is initialized only when needed, after audio_capture may also init it.
    initialized = true;
    ESP_LOGI(TAG, "Speaker driver ready (I2S%d, BCK=%d WS=%d DOUT=%d, vol=%d%%) — TX channel deferred",
             (int)tx_port, I2S_DAC_BCLK, I2S_DAC_LRCLK, I2S_DAC_DOUT, volume);

    // Apply NVS-loaded log level immediately so serial is quiet or verbose from boot.
    audio_output_set_hardware_diagnostic_mode(hardware_diagnostic_mode);
    return ESP_OK;
}

void audio_output_deinit(void) {
    if (!initialized) return;

    if (tx_channel) {
        disable_tx_channel();
        if (!tx_from_shared_bus) {
            i2s_del_channel(tx_channel);
        }
        tx_channel = NULL;
    }
    initialized = false;
    ESP_LOGI(TAG, "Speaker deinitialized");
}

bool audio_output_write(const int16_t *samples, size_t count, uint32_t timeout_ms) {
    if (!initialized || !samples || count == 0) return false;

    // Don't write while gong/test-tone task owns the channel.
    if (gong_task_running) return false;

    // If channel doesn't exist yet, or was disabled by a completed gong/test-tone,
    // do a full rebuild — same sequence as prepare_exclusive_playback() — so the
    // I2S hardware starts from a clean state and MAX98357A re-locks onto LRCLK.
    // When the channel is already enabled (SIP streaming), skip the rebuild.
    if (!tx_channel || !tx_enabled) {
        ESP_LOGI(TAG, "audio_output_write: rebuild TX (%s)",
                 !tx_channel ? "no channel" : "was disabled");
        if (rebuild_tx_channel() != ESP_OK) return false;

        // Feed silence preamble to let MAX98357A lock onto LRCLK.
        int16_t stereo_silence[512];
        memset(stereo_silence, 0, sizeof(stereo_silence));
        size_t bw = 0;
        for (int p = 0; p < 3; p++) {
            i2s_channel_write(tx_channel, stereo_silence, sizeof(stereo_silence),
                              &bw, pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Apply volume scaling and duplicate mono to stereo for MAX98357A.
    int16_t stereo[512];
    size_t offset = 0;

    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > 256) chunk = 256;

        for (size_t i = 0; i < chunk; i++) {
            int32_t s = (int32_t)samples[offset + i] * volume / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            stereo[2 * i] = (int16_t)s;
            stereo[2 * i + 1] = (int16_t)s;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_channel, stereo,
                                           chunk * sizeof(int16_t) * 2,
                                           &bytes_written,
                                           pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) return false;
        offset += chunk;
    }

    return true;
}

void audio_output_set_volume(uint8_t percent) {
    if (percent > 100) percent = 100;
    volume = percent;
}

uint8_t audio_output_get_volume(void) {
    return volume;
}

// ---------- Gong playback ----------

static bool diag_samples_logged = false;

static void write_samples(const int16_t *buf, size_t count) {
    if (ensure_tx_enabled() != ESP_OK) {
        return;
    }

    // Log the first few non-zero samples once per playback for diagnostics
    if (hardware_diagnostic_mode && !diag_samples_logged) {
        for (size_t i = 0; i < count && i < 8; i++) {
            if (buf[i] != 0) {
                ESP_LOGI(TAG, "DIAG samples[0..7]: %d %d %d %d %d %d %d %d",
                         buf[0], buf[1], buf[2], buf[3],
                         buf[4], buf[5], buf[6], buf[7]);
                diag_samples_logged = true;
                break;
            }
        }
    }

    // Stereo duplication: duplicate mono samples to both L and R channels.
    int16_t stereo[512];
    for (size_t i = 0; i < count; i++) {
        stereo[2 * i] = buf[i];
        stereo[2 * i + 1] = buf[i];
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_channel, stereo, count * sizeof(int16_t) * 2,
                                      &bytes_written, pdMS_TO_TICKS(TX_WRITE_TIMEOUT_MS));
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            diag_writes_timeout++;
            ESP_LOGW(TAG, "TX write timeout (timeouts=%lu, bytes=%llu)",
                     (unsigned long)diag_writes_timeout,
                     (unsigned long long)diag_bytes_written);
        } else {
            diag_writes_other_err++;
            ESP_LOGW(TAG, "TX write failed: %s", esp_err_to_name(err));
        }
    } else if (bytes_written == 0) {
        diag_zero_bytes++;
        ESP_LOGW(TAG, "TX write returned 0 bytes");
    } else {
        diag_writes_ok++;
        diag_bytes_written += bytes_written;
        if (hardware_diagnostic_mode && (diag_writes_ok % 64 == 0)) {
            ESP_LOGI(TAG,
                     "DIAG TX ok=%lu timeout=%lu err=%lu zero=%lu bytes=%llu",
                     (unsigned long)diag_writes_ok,
                     (unsigned long)diag_writes_timeout,
                     (unsigned long)diag_writes_other_err,
                     (unsigned long)diag_zero_bytes,
                     (unsigned long long)diag_bytes_written);
        }
    }
}

static bool prepare_exclusive_playback(bool *resume_capture) {
    // Shared bus: TX and RX are independent channels — no need to stop capture.
    // INMP441 RX continues reading while MAX98357A TX plays audio (true full-duplex).
    *resume_capture = false;

    if (rebuild_tx_channel() != ESP_OK) {
        ESP_LOGE(TAG, "TX rebuild failed before playback");
        return false;
    }

    // Let MAX98357A lock onto LRCLK and settle before sending audio.
    // Feed a few DMA buffers of silence first to avoid pop/buzz on start.
    int16_t silence[256];
    memset(silence, 0, sizeof(silence));
    for (int i = 0; i < 3; i++) {
        write_samples(silence, 256);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    return true;
}

static void play_embedded_pcm(void) {
    const int16_t *pcm = (const int16_t *)gong_pcm_data;
    size_t total_samples = gong_pcm_data_size / sizeof(int16_t);
    int16_t buf[256];
    size_t offset = 0;

    while (offset < total_samples) {
        size_t chunk = total_samples - offset;
        if (chunk > 256) chunk = 256;

        for (size_t i = 0; i < chunk; i++) {
            int32_t s = (int32_t)pcm[offset + i] * (int32_t)volume * GONG_PCM_HEADROOM_PCT / 10000;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[i] = (int16_t)s;
        }

        write_samples(buf, chunk);
        offset += chunk;
    }
}

static void play_synthesized_gong(void) {
    const float tone_a = 880.0f;
    const float tone_b = 660.0f;
    const uint32_t samples_per_tone = AUDIO_SAMPLE_RATE / 3;
    int16_t buf[256];

    // Tone A (880 Hz) with decay envelope. Peak 5000 (~15% full scale) keeps
    // output below the speaker distortion threshold at volume=100%.
    float phase = 0.0f;
    float phase_step = 2.0f * (float)M_PI * tone_a / AUDIO_SAMPLE_RATE;
    for (uint32_t i = 0; i < samples_per_tone; i += 256) {
        size_t chunk = samples_per_tone - i;
        if (chunk > 256) chunk = 256;
        float envelope = 1.0f - (float)i / samples_per_tone;
        for (size_t j = 0; j < chunk; j++) {
            float sample = sinf(phase) * envelope;
            int32_t s = (int32_t)(sample * 5000.0f) * volume / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[j] = (int16_t)s;
            phase += phase_step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        write_samples(buf, chunk);
    }

    // Tone B (660 Hz) with decay envelope.
    phase = 0.0f;
    phase_step = 2.0f * (float)M_PI * tone_b / AUDIO_SAMPLE_RATE;
    for (uint32_t i = 0; i < samples_per_tone; i += 256) {
        size_t chunk = samples_per_tone - i;
        if (chunk > 256) chunk = 256;
        float envelope = 1.0f - (float)i / samples_per_tone;
        for (size_t j = 0; j < chunk; j++) {
            float sample = sinf(phase) * envelope;
            int32_t s = (int32_t)(sample * 4500.0f) * volume / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[j] = (int16_t)s;
            phase += phase_step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        write_samples(buf, chunk);
    }
}

static void gong_task(void *param) {
    gong_task_running = true;
    diag_samples_logged = false;
    bool resume_capture = false;

    if (!initialized) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!xSemaphoreTake(output_mutex, pdMS_TO_TICKS(1000))) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!prepare_exclusive_playback(&resume_capture)) {
        xSemaphoreGive(output_mutex);
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Play embedded PCM if available, else synthesize
    if (gong_pcm_data_size > 0) {
        ESP_LOGI(TAG, "Playing embedded gong PCM (%u bytes)", (unsigned)gong_pcm_data_size);
        play_embedded_pcm();
    } else {
        ESP_LOGI(TAG, "Playing synthesized gong (880/660 Hz)");
        play_synthesized_gong();
    }

    // Flush DMA circular buffers with silence so audio doesn't loop
    int16_t silence[256];
    memset(silence, 0, sizeof(silence));
    for (int i = 0; i < I2S_DMA_BUF_COUNT + 2; i++) {
        write_samples(silence, 256);
    }

    // Fully disable TX so residual DMA content cannot keep looping quietly.
    disable_tx_channel();

    ESP_LOGI(TAG, "Gong playback finished");

    if (resume_capture) {
        audio_capture_start();
    }

    xSemaphoreGive(output_mutex);
    gong_task_running = false;
    vTaskDelete(NULL);
}

void audio_output_play_gong(void) {
    if (!initialized) return;
    if (gong_task_running) return;
    if (volume == 0) return;

    xTaskCreatePinnedToCore(gong_task, "gong", AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIORITY, NULL, STREAM_TASK_CORE);
}

// ---------- Debug test tone ----------

static void test_tone_task(void *param) {
    gong_task_running = true;
    diag_samples_logged = false;
    bool resume_capture = false;

    if (!initialized) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!xSemaphoreTake(output_mutex, pdMS_TO_TICKS(1000))) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!prepare_exclusive_playback(&resume_capture)) {
        xSemaphoreGive(output_mutex);
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    // 440 Hz for 2s. Amplitude 5000 (~15% full scale) with volume scaling
    // so even at volume=100% the output stays below the speaker distortion threshold.
    const float freq = 440.0f;
    const uint32_t duration_samples = AUDIO_SAMPLE_RATE * 2; // 2 seconds
    float phase = 0.0f;
    float phase_step = 2.0f * (float)M_PI * freq / AUDIO_SAMPLE_RATE;
    int16_t buf[256];

    const float amplitude = 5000.0f;

    ESP_LOGI(TAG, "Test tone: %d Hz, 2s, amp=%.0f vol=%d%%, I2S%d, BCLK=%d WS=%d DOUT=%d (diag=%d)",
             (int)freq, amplitude, volume, (int)tx_port,
             I2S_DAC_BCLK, I2S_DAC_LRCLK, I2S_DAC_DOUT,
             hardware_diagnostic_mode ? 1 : 0);

    for (uint32_t i = 0; i < duration_samples; i += 256) {
        size_t chunk = duration_samples - i;
        if (chunk > 256) chunk = 256;
        for (size_t j = 0; j < chunk; j++) {
            int32_t s = (int32_t)(sinf(phase) * amplitude * volume / 100);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[j] = (int16_t)s;
            phase += phase_step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        write_samples(buf, chunk);
    }

    // Flush and then fully disable TX to avoid residual output.
    int16_t silence[256];
    memset(silence, 0, sizeof(silence));
    for (int i = 0; i < I2S_DMA_BUF_COUNT + 2; i++) {
        write_samples(silence, 256);
    }
    disable_tx_channel();

    ESP_LOGI(TAG, "Test tone finished");

    if (resume_capture) {
        audio_capture_start();
    }

    xSemaphoreGive(output_mutex);
    gong_task_running = false;
    vTaskDelete(NULL);
}

void audio_output_play_test_tone(void) {
    if (!initialized) {
        ESP_LOGW(TAG, "Speaker not initialized, cannot play test tone");
        return;
    }
    if (gong_task_running) {
        ESP_LOGW(TAG, "Audio already playing");
        return;
    }

    xTaskCreatePinnedToCore(test_tone_task, "test_tone", AUDIO_TASK_STACK_SIZE, NULL, AUDIO_TASK_PRIORITY, NULL, STREAM_TASK_CORE);
}
