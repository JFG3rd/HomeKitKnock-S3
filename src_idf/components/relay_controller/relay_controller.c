/**
 * Relay Controller Component
 *
 * Drives GONG_RELAY_PIN (GPIO3) and DOOR_OPENER_PIN (GPIO1) via async
 * FreeRTOS tasks.  Each pulse task waits a brief startup delay before
 * asserting the output, allowing audio I2S to initialize first.
 */

#include "relay_controller.h"
#include "config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "relay_controller";

// Cached pulse durations (updated from NVS at startup / on features save)
static uint32_t s_gong_ms = GONG_RELAY_PULSE_MS;
static uint32_t s_door_ms = DOOR_OPENER_PULSE_MS;

// ---------------------------------------------------------------------------
// Gong relay (GPIO3)
// ---------------------------------------------------------------------------

typedef struct { int pin; uint32_t ms; uint32_t delay_ms; } pulse_args_t;
static pulse_args_t s_gong_args;
static pulse_args_t s_door_args;

static void pulse_task(void *arg) {
    pulse_args_t *a = (pulse_args_t *)arg;
    if (a->delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(a->delay_ms));
    }
    ESP_LOGI(TAG, "GPIO%d HIGH for %lums", a->pin, (unsigned long)a->ms);
    gpio_set_level(a->pin, 1);
    vTaskDelay(pdMS_TO_TICKS(a->ms));
    gpio_set_level(a->pin, 0);
    ESP_LOGI(TAG, "GPIO%d LOW", a->pin);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t relay_controller_init(void) {
    // Configure both relay outputs LOW
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GONG_RELAY_PIN) | (1ULL << DOOR_OPENER_PIN),
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
    gpio_set_level(DOOR_OPENER_PIN, 0);
    ESP_LOGI(TAG, "GPIO%d (gong) and GPIO%d (door) configured LOW",
             GONG_RELAY_PIN, DOOR_OPENER_PIN);
    return ESP_OK;
}

void relay_controller_set_gong_ms(uint32_t ms) {
    if (ms > 0) s_gong_ms = ms;
}

uint32_t relay_controller_get_gong_ms(void) {
    return s_gong_ms;
}

void relay_controller_set_door_ms(uint32_t ms) {
    if (ms > 0) s_door_ms = ms;
}

uint32_t relay_controller_get_door_ms(void) {
    return s_door_ms;
}

void relay_controller_pulse_gong(void) {
    // 150ms startup delay lets gong I2S initialize before relay fires.
    // Task pinned to core 0 so it cannot compete with the gong audio task on core 1.
    // Stack is 4096 bytes — ESP_LOGI needs ~2-3 KB for vsnprintf formatting.
    s_gong_args = (pulse_args_t){ .pin = GONG_RELAY_PIN, .ms = s_gong_ms, .delay_ms = 150 };
    xTaskCreatePinnedToCore(pulse_task, "relay_gong", 4096, &s_gong_args, 5, NULL, 0);
}

void relay_controller_pulse_door(void) {
    s_door_args = (pulse_args_t){ .pin = DOOR_OPENER_PIN, .ms = s_door_ms, .delay_ms = 0 };
    xTaskCreatePinnedToCore(pulse_task, "relay_door", 4096, &s_door_args, 5, NULL, 0);
}
