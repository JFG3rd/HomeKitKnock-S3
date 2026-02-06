/**
 * Button Component Implementation
 *
 * Simple debounced button handler for doorbell trigger.
 */

#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "button";

// Button state
static bool last_button_pressed = false;
static uint32_t last_change_ms = 0;
static bool button_latched = false;
static button_press_callback_t press_callback = NULL;

// Helper to get milliseconds
static uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t button_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIO: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Button initialized on GPIO%d (active-%s)",
             BUTTON_GPIO, BUTTON_ACTIVE_LOW ? "low" : "high");
    return ESP_OK;
}

void button_set_callback(button_press_callback_t callback) {
    press_callback = callback;
}

bool button_is_pressed(void) {
    int level = gpio_get_level(BUTTON_GPIO);
    if (BUTTON_ACTIVE_LOW) {
        return level == 0;
    }
    return level == 1;
}

void button_poll(void) {
    bool pressed = button_is_pressed();
    uint32_t now = millis();

    // Detect state change
    if (pressed != last_button_pressed) {
        last_change_ms = now;
        last_button_pressed = pressed;
    }

    // Check for stable press (debounced)
    if (pressed && !button_latched && (now - last_change_ms) > BUTTON_DEBOUNCE_MS) {
        ESP_LOGI(TAG, "Button pressed");
        button_latched = true;
        if (press_callback) {
            press_callback();
        }
    }
    // Check for stable release (debounced)
    else if (!pressed && button_latched && (now - last_change_ms) > BUTTON_DEBOUNCE_MS) {
        button_latched = false;
    }
}
