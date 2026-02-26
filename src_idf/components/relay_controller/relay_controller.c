/**
 * Relay Controller Component
 *
 * Drives GONG_RELAY_PIN (GPIO3) HIGH for a configurable pulse duration,
 * then LOW, via an async FreeRTOS task.
 */

#include "relay_controller.h"
#include "config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "relay_controller";

static void pulse_task(void *arg) {
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "pulsing GPIO%d for %ldms", GONG_RELAY_PIN, (long)ms);
    gpio_set_level(GONG_RELAY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(GONG_RELAY_PIN, 0);
    ESP_LOGI(TAG, "GPIO%d LOW", GONG_RELAY_PIN);
    vTaskDelete(NULL);
}

esp_err_t relay_controller_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GONG_RELAY_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(GONG_RELAY_PIN, 0);
    ESP_LOGI(TAG, "GPIO%d configured, starting LOW", GONG_RELAY_PIN);
    return ESP_OK;
}

void relay_controller_pulse_async(uint32_t ms) {
    xTaskCreate(pulse_task, "relay_pulse", 1024, (void *)(uintptr_t)ms, 5, NULL);
}
