/**
 * Audio Capture Component
 *
 * I2S microphone input. Supports onboard PDM mic (I2S_NUM_0) or
 * external INMP441 (I2S_NUM_1). Mic source is boot-time config from NVS.
 */

#include "audio_capture.h"
#include "i2s_shared_bus.h"
#include "camera.h"
#include "config.h"
#include "nvs_manager.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio_capture";

#define NVS_CAMERA_NAMESPACE "camera"
#define NVS_KEY_MIC_ENABLED  "mic_en"
#define NVS_KEY_MIC_MUTED    "mic_mute"
#define NVS_KEY_MIC_SENS     "mic_sens"
#define NVS_KEY_MIC_SOURCE   "mic_source"

#define I2S_DMA_BUF_COUNT    4
#define I2S_DMA_BUF_SAMPLES  512

static i2s_chan_handle_t rx_channel = NULL;
static SemaphoreHandle_t capture_mutex = NULL;
static bool rx_from_shared_bus = false;

static mic_source_t current_source = MIC_SOURCE_PDM;
static bool mic_enabled = false;
static bool mic_muted = false;
static uint8_t mic_sensitivity = 70;
static bool initialized = false;
static bool running = false;

static uint32_t diag_reads_ok      = 0;
static uint32_t diag_reads_timeout = 0;
static uint32_t diag_reads_muted   = 0;
static uint64_t diag_samples_read  = 0;

static void load_nvs_config(void) {
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t val = 0;
    if (nvs_get_u8(handle, NVS_KEY_MIC_ENABLED, &val) == ESP_OK) {
        mic_enabled = (val != 0);
    }
    if (nvs_get_u8(handle, NVS_KEY_MIC_MUTED, &val) == ESP_OK) {
        mic_muted = (val != 0);
    }
    if (nvs_get_u8(handle, NVS_KEY_MIC_SENS, &val) == ESP_OK) {
        mic_sensitivity = val;
    }
    if (nvs_get_u8(handle, NVS_KEY_MIC_SOURCE, &val) == ESP_OK) {
        current_source = (val == 1) ? MIC_SOURCE_INMP441 : MIC_SOURCE_PDM;
    }

    nvs_close(handle);
}

static esp_err_t start_pdm_mic(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM channel create failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_PDM_MIC_CLK,
            .din = I2S_PDM_MIC_DATA,
            .invert_flags = { .clk_inv = false },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(rx_channel, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM init failed: %s", esp_err_to_name(err));
        i2s_del_channel(rx_channel);
        rx_channel = NULL;
        return err;
    }

    err = i2s_channel_enable(rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(rx_channel);
        rx_channel = NULL;
        return err;
    }

    ESP_LOGI(TAG, "PDM mic started (I2S0, GPIO%d/%d, %d Hz)",
             I2S_PDM_MIC_DATA, I2S_PDM_MIC_CLK, AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t start_inmp441_mic(void) {
    // GPIO7/8 (BCLK/WS) are physically shared with MAX98357A speaker.
    // Use shared bus so both channels coexist on I2S_NUM_1 (true full-duplex).
    esp_err_t err = i2s_shared_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    rx_channel = i2s_shared_bus_get_rx_channel();
    if (!rx_channel) {
        ESP_LOGE(TAG, "Shared bus RX channel not available");
        return ESP_FAIL;
    }

    // Channel may already be enabled if audio_output initialized shared bus first.
    err = i2s_channel_enable(rx_channel);
    if (err == ESP_ERR_INVALID_STATE) {
        i2s_channel_disable(rx_channel);
        err = i2s_channel_enable(rx_channel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "INMP441 RX enable failed: %s", esp_err_to_name(err));
        rx_channel = NULL;
        return err;
    }

    rx_from_shared_bus = true;

    // TX channel is the BCLK master for the shared I2S bus. Without TX enabled,
    // no clock signal appears on GPIO7 and INMP441 outputs zeros regardless of
    // wiring. Enable TX now so BCLK flows as soon as RX capture starts.
    i2s_chan_handle_t tx_ch = i2s_shared_bus_get_tx_channel();
    if (tx_ch) {
        esp_err_t tx_err = i2s_channel_enable(tx_ch);
        if (tx_err == ESP_OK) {
            ESP_LOGI(TAG, "INMP441: TX enabled to generate BCLK (GPIO%d)", I2S_INMP441_SCK);
        } else if (tx_err == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "INMP441: TX already enabled — BCLK flowing");
        } else {
            ESP_LOGW(TAG, "INMP441: TX enable for BCLK failed: %s", esp_err_to_name(tx_err));
        }
    }

    ESP_LOGI(TAG, "INMP441 mic started via shared bus (I2S1, SCK=%d WS=%d SD=%d, %d Hz)",
             I2S_INMP441_SCK, I2S_INMP441_WS, I2S_INMP441_SD, AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_capture_init(void) {
    if (initialized) {
        return ESP_OK;
    }

    capture_mutex = xSemaphoreCreateMutex();
    if (!capture_mutex) {
        return ESP_ERR_NO_MEM;
    }

    load_nvs_config();

    ESP_LOGI(TAG, "Audio capture initialized (source=%s, enabled=%d, sensitivity=%d)",
             current_source == MIC_SOURCE_PDM ? "PDM" : "INMP441",
             mic_enabled, mic_sensitivity);

    initialized = true;
    return ESP_OK;
}

esp_err_t audio_capture_start(void) {
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (running) return ESP_OK;

    xSemaphoreTake(capture_mutex, portMAX_DELAY);

    esp_err_t err;
    if (current_source == MIC_SOURCE_INMP441) {
        err = start_inmp441_mic();
    } else {
        err = start_pdm_mic();
    }

    if (err == ESP_OK) {
        running = true;
    }

    xSemaphoreGive(capture_mutex);
    return err;
}

void audio_capture_stop(void) {
    if (!running || !rx_channel) return;

    xSemaphoreTake(capture_mutex, portMAX_DELAY);

    i2s_channel_disable(rx_channel);
    if (!rx_from_shared_bus) {
        i2s_del_channel(rx_channel);
    }
    rx_channel = NULL;
    running = false;
    rx_from_shared_bus = false;

    ESP_LOGI(TAG, "Audio capture stopped");
    xSemaphoreGive(capture_mutex);
}

bool audio_capture_read(int16_t *buffer, size_t sample_count, uint32_t timeout_ms) {
    if (!running || !rx_channel || !buffer || sample_count == 0) {
        return false;
    }

    // If muted, return silence
    if (mic_muted) {
        memset(buffer, 0, sample_count * sizeof(int16_t));
        diag_reads_muted++;
        return true;
    }

    size_t samples_read = 0;
    esp_err_t err = ESP_OK;

    if (rx_from_shared_bus) {
        // INMP441 via shared I2S1 bus: ESP-IDF STD driver returns stereo-interleaved
        // DMA data [L, R, L, R, ...] even when configured as I2S_SLOT_MODE_MONO.
        // INMP441 with L/R=GND outputs only on the left channel (WS=LOW); the right
        // channel slot is always zero. Read in 256-frame chunks and extract L only.
        int16_t chunk[256 * 2];  // 256 stereo frames, 1 KB stack
        size_t filled = 0;
        TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);

        while (filled < sample_count) {
            size_t frames = sample_count - filled;
            if (frames > 256) frames = 256;
            size_t got = 0;
            err = i2s_channel_read(rx_channel, chunk,
                                   frames * 2 * sizeof(int16_t), &got, wait_ticks);
            if (err != ESP_OK || got == 0) {
                memset(buffer + filled, 0,
                       (sample_count - filled) * sizeof(int16_t));
                diag_reads_timeout++;
                return false;
            }
            size_t f = got / (2 * sizeof(int16_t));
            for (size_t i = 0; i < f; i++) {
                buffer[filled + i] = chunk[2 * i];  // L channel (even index)
            }
            filled += f;
            wait_ticks = pdMS_TO_TICKS(200);  // relax after first fill
        }
        samples_read = filled;
    } else {
        // PDM mic (I2S_NUM_0): true mono DMA, direct read into output buffer.
        size_t bytes_to_read = sample_count * sizeof(int16_t);
        size_t bytes_read = 0;
        err = i2s_channel_read(rx_channel, buffer, bytes_to_read,
                               &bytes_read, pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK || bytes_read == 0) {
            memset(buffer, 0, sample_count * sizeof(int16_t));
            diag_reads_timeout++;
            return false;
        }
        samples_read = bytes_read / sizeof(int16_t);
        if (samples_read < sample_count) {
            memset(buffer + samples_read, 0,
                   (sample_count - samples_read) * sizeof(int16_t));
        }
    }

    // Apply software sensitivity scaling
    if (mic_sensitivity < 100) {
        for (size_t i = 0; i < samples_read; i++) {
            int32_t scaled = (int32_t)buffer[i] * mic_sensitivity / 100;
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;
            buffer[i] = (int16_t)scaled;
        }
    }

    diag_reads_ok++;
    diag_samples_read += samples_read;
    if (diag_reads_ok % 200 == 0 && camera_is_hardware_diag_enabled()) {
        ESP_LOGI(TAG, "DIAG ok=%lu timeout=%lu muted=%lu samples=%llu",
                 (unsigned long)diag_reads_ok, (unsigned long)diag_reads_timeout,
                 (unsigned long)diag_reads_muted, (unsigned long long)diag_samples_read);
    }

    return true;
}

bool audio_capture_is_running(void) {
    return running;
}

mic_source_t audio_capture_get_source(void) {
    return current_source;
}

void audio_capture_set_sensitivity(uint8_t percent) {
    if (percent > 100) percent = 100;
    mic_sensitivity = percent;
}

bool audio_capture_is_enabled(void) {
    if (!initialized) {
        // Read directly from NVS if not yet initialized
        nvs_handle_t handle;
        if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            return false;
        }
        uint8_t val = 0;
        nvs_get_u8(handle, NVS_KEY_MIC_ENABLED, &val);
        nvs_close(handle);
        return val != 0;
    }
    return mic_enabled;
}

bool audio_capture_is_muted(void) {
    return mic_muted;
}

uint8_t audio_capture_get_sensitivity(void) {
    return mic_sensitivity;
}
