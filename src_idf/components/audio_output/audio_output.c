/**
 * Audio Output Component
 *
 * MAX98357A I2S DAC speaker on I2S_NUM_1.
 * Gong: embedded PCM from flash, falls back to synthesized 2-tone.
 */

#include "audio_output.h"
#include "audio_capture.h"
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
#define NVS_KEY_MIC_SOURCE   "mic_source"

#define I2S_DMA_BUF_COUNT    6
#define I2S_DMA_BUF_SAMPLES  256

// Embedded gong PCM (from gong_data.c, generated from data/gong.pcm)
extern const uint8_t gong_pcm_data[];
extern const size_t gong_pcm_data_size;

static i2s_chan_handle_t tx_channel = NULL;
static SemaphoreHandle_t output_mutex = NULL;
static uint8_t volume = 70;
static bool initialized = false;
static bool tx_enabled = false;
static bool gong_task_running = false;

static mic_source_t read_mic_source_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return audio_capture_get_source();
    }

    uint8_t source = 0;
    if (nvs_get_u8(handle, NVS_KEY_MIC_SOURCE, &source) != ESP_OK) {
        nvs_close(handle);
        return audio_capture_get_source();
    }

    nvs_close(handle);
    return (source == 1) ? MIC_SOURCE_INMP441 : MIC_SOURCE_PDM;
}

static esp_err_t ensure_tx_enabled(void) {
    if (!tx_channel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx_enabled) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(tx_channel);
    if (err == ESP_OK) {
        tx_enabled = true;
    }
    return err;
}

static void disable_tx_channel(void) {
    if (tx_channel && tx_enabled) {
        i2s_channel_disable(tx_channel);
        tx_enabled = false;
    }
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
    nvs_close(handle);
}

bool audio_output_is_available(void) {
    return read_mic_source_from_nvs() != MIC_SOURCE_INMP441;
}

esp_err_t audio_output_init(void) {
    if (initialized) return ESP_OK;

    if (!audio_output_is_available()) {
        ESP_LOGW(TAG, "Speaker unavailable (INMP441 owns I2S1)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    output_mutex = xSemaphoreCreateMutex();
    if (!output_mutex) return ESP_ERR_NO_MEM;

    load_nvs_config();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_channel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker channel create failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_DAC_BCLK,
            .ws = I2S_DAC_LRCLK,
            .dout = I2S_DAC_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // Duplicate mono data to both L and R slots. MAX98357A with SD pin at GND
    // outputs (L+R)/2; sending to only one slot halves the volume.
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(tx_channel, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker init failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
        return err;
    }

    err = i2s_channel_enable(tx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
        return err;
    }
    tx_enabled = true;

    initialized = true;
    ESP_LOGI(TAG, "Speaker initialized (I2S1, BCK=%d WS=%d DOUT=%d, vol=%d%%)",
             I2S_DAC_BCLK, I2S_DAC_LRCLK, I2S_DAC_DOUT, volume);
    return ESP_OK;
}

void audio_output_deinit(void) {
    if (!initialized) return;

    if (tx_channel) {
        disable_tx_channel();
        i2s_del_channel(tx_channel);
        tx_channel = NULL;
    }
    initialized = false;
    ESP_LOGI(TAG, "Speaker deinitialized");
}

bool audio_output_write(const int16_t *samples, size_t count, uint32_t timeout_ms) {
    if (!initialized || !tx_channel || !samples || count == 0) return false;

    if (ensure_tx_enabled() != ESP_OK) {
        return false;
    }

    // Apply volume scaling to a temp buffer (avoid modifying caller's data)
    int16_t scaled[256];
    size_t offset = 0;

    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > 256) chunk = 256;

        for (size_t i = 0; i < chunk; i++) {
            int32_t s = (int32_t)samples[offset + i] * volume / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            scaled[i] = (int16_t)s;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_channel, scaled,
                                           chunk * sizeof(int16_t),
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

static void write_samples(const int16_t *buf, size_t count) {
    if (ensure_tx_enabled() != ESP_OK) {
        return;
    }
    size_t bytes_written = 0;
    i2s_channel_write(tx_channel, buf, count * sizeof(int16_t),
                      &bytes_written, pdMS_TO_TICKS(100));
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
            int32_t s = (int32_t)pcm[offset + i] * volume / 100;
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

    // Tone A (880 Hz) with decay envelope
    float phase = 0.0f;
    float phase_step = 2.0f * (float)M_PI * tone_a / AUDIO_SAMPLE_RATE;
    for (uint32_t i = 0; i < samples_per_tone; i += 256) {
        size_t chunk = samples_per_tone - i;
        if (chunk > 256) chunk = 256;
        float envelope = 1.0f - (float)i / samples_per_tone;
        for (size_t j = 0; j < chunk; j++) {
            float sample = sinf(phase) * envelope;
            int32_t s = (int32_t)(sample * 16000.0f) * volume / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[j] = (int16_t)s;
            phase += phase_step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        write_samples(buf, chunk);
    }

    // Tone B (660 Hz) with decay envelope
    phase = 0.0f;
    phase_step = 2.0f * (float)M_PI * tone_b / AUDIO_SAMPLE_RATE;
    for (uint32_t i = 0; i < samples_per_tone; i += 256) {
        size_t chunk = samples_per_tone - i;
        if (chunk > 256) chunk = 256;
        float envelope = 1.0f - (float)i / samples_per_tone;
        for (size_t j = 0; j < chunk; j++) {
            float sample = sinf(phase) * envelope;
            int32_t s = (int32_t)(sample * 14000.0f) * volume / 100;
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

    if (!initialized || !tx_channel) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!xSemaphoreTake(output_mutex, pdMS_TO_TICKS(1000))) {
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

    xSemaphoreGive(output_mutex);
    gong_task_running = false;
    vTaskDelete(NULL);
}

void audio_output_play_gong(void) {
    if (!initialized || !tx_channel) return;
    if (gong_task_running) return;
    if (volume == 0) return;
    if (ensure_tx_enabled() != ESP_OK) return;

    xTaskCreatePinnedToCore(gong_task, "gong", 4096, NULL, 1, NULL, STREAM_TASK_CORE);
}

// ---------- Debug test tone ----------

static void test_tone_task(void *param) {
    gong_task_running = true;

    if (!initialized || !tx_channel) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!xSemaphoreTake(output_mutex, pdMS_TO_TICKS(1000))) {
        gong_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    // 1 kHz sine wave for 1 second at current volume
    const float freq = 1000.0f;
    const uint32_t duration_samples = AUDIO_SAMPLE_RATE; // 1 second
    float phase = 0.0f;
    float phase_step = 2.0f * (float)M_PI * freq / AUDIO_SAMPLE_RATE;
    int16_t buf[256];

    ESP_LOGI(TAG, "Test tone: 1 kHz, 1s, vol=%d%%", volume);

    for (uint32_t i = 0; i < duration_samples; i += 256) {
        size_t chunk = duration_samples - i;
        if (chunk > 256) chunk = 256;
        for (size_t j = 0; j < chunk; j++) {
            int32_t s = (int32_t)(sinf(phase) * 24000.0f) * volume / 100;
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

    xSemaphoreGive(output_mutex);
    gong_task_running = false;
    vTaskDelete(NULL);
}

void audio_output_play_test_tone(void) {
    if (!initialized || !tx_channel) {
        ESP_LOGW(TAG, "Speaker not initialized, cannot play test tone");
        return;
    }
    if (gong_task_running) {
        ESP_LOGW(TAG, "Audio already playing");
        return;
    }
    if (ensure_tx_enabled() != ESP_OK) {
        ESP_LOGW(TAG, "Speaker enable failed, cannot play test tone");
        return;
    }

    xTaskCreatePinnedToCore(test_tone_task, "test_tone", 4096, NULL, 1, NULL, STREAM_TASK_CORE);
}
