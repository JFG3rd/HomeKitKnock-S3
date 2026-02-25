#include "i2s_shared_bus.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "i2s_shared_bus";

#define I2S_DMA_BUF_COUNT    6
#define I2S_DMA_BUF_SAMPLES  256

static SemaphoreHandle_t s_mutex = NULL;
static i2s_chan_handle_t s_tx_channel = NULL;
static i2s_chan_handle_t s_rx_channel = NULL;
static bool s_initialized = false;

esp_err_t i2s_shared_bus_init(void) {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_channel, &s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared channel create failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_INMP441_SCK,
            .ws = I2S_INMP441_WS,
            .dout = I2S_GPIO_UNUSED,   // RX channel has no data output
            .din = I2S_INMP441_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // INMP441 requires 64 BCLK per LRCLK (32-bit I2S frames).
    // The Philips macro defaults slot_bit_width to data_bit_width (16), giving
    // only 32 BCLK per LRCLK at which INMP441 outputs no valid data.
    // Force 32-bit slots: BCLK = 16000 × 2 × 32 = 1.024 MHz. ✓
    rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    err = i2s_channel_init_std_mode(s_rx_channel, &rx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared RX init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_channel);
        i2s_del_channel(s_tx_channel);
        s_rx_channel = NULL;
        s_tx_channel = NULL;
        xSemaphoreGive(s_mutex);
        return err;
    }

    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
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
    // TX must match RX slot_bit_width — TX is the BCLK master for the full-duplex pair.
    // 32-bit slots → BCLK = 1.024 MHz. MAX98357A supports 32-bit I2S frames. ✓
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(s_tx_channel, &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shared TX init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_channel);
        i2s_del_channel(s_tx_channel);
        s_rx_channel = NULL;
        s_tx_channel = NULL;
        xSemaphoreGive(s_mutex);
        return err;
    }

    s_initialized = true;

    ESP_LOGI(TAG,
             "Shared I2S1 full-duplex ready (BCLK=%d WS=%d, DIN=%d, DOUT=%d)",
             I2S_DAC_BCLK, I2S_DAC_LRCLK, I2S_INMP441_SD, I2S_DAC_DOUT);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void i2s_shared_bus_deinit(void) {
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_initialized) {
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_tx_channel) {
        i2s_channel_disable(s_tx_channel);
    }
    if (s_rx_channel) {
        i2s_channel_disable(s_rx_channel);
    }

    if (s_rx_channel) {
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    if (s_tx_channel) {
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Shared I2S1 full-duplex deinitialized");

    xSemaphoreGive(s_mutex);
}

bool i2s_shared_bus_is_initialized(void) {
    return s_initialized;
}

i2s_chan_handle_t i2s_shared_bus_get_tx_channel(void) {
    return s_tx_channel;
}

i2s_chan_handle_t i2s_shared_bus_get_rx_channel(void) {
    return s_rx_channel;
}
