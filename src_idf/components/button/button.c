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

// Long-press state
static button_long_press_callback_t long_press_callback = NULL;
static uint32_t long_press_hold_ms = 0;
static uint32_t press_start_ms = 0;
static bool long_press_fired = false;

// Double long-press state machine
typedef enum { DLP_IDLE = 0, DLP_FIRST_HOLDING, DLP_WAIT_RELEASE, DLP_SECOND_HOLDING } dlp_state_t;
static button_long_press_callback_t double_lp_callback = NULL;
static uint32_t double_lp_hold_ms    = 0;
static uint32_t double_lp_window_ms  = 0;
static dlp_state_t dlp_state         = DLP_IDLE;
static uint32_t dlp_press_start_ms   = 0;
static uint32_t dlp_release_ms       = 0;
static bool     dlp_first_done       = false;

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

void button_set_long_press_callback(button_long_press_callback_t callback, uint32_t hold_ms) {
    long_press_callback = callback;
    long_press_hold_ms  = hold_ms;
}

void button_set_double_long_press_callback(button_long_press_callback_t callback,
                                           uint32_t hold_ms,
                                           uint32_t release_window_ms) {
    double_lp_callback  = callback;
    double_lp_hold_ms   = hold_ms;
    double_lp_window_ms = release_window_ms;
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
        if (pressed) {
            press_start_ms   = now;
            long_press_fired = false;
        } else {
            press_start_ms   = 0;
            long_press_fired = false;
        }
    }

    // Check for stable press (debounced) — fires once on initial latch
    if (pressed && !button_latched && (now - last_change_ms) > BUTTON_DEBOUNCE_MS) {
        ESP_LOGI(TAG, "Button pressed");
        button_latched = true;
        if (press_callback) {
            press_callback();
        }
        // DLP: rising edge
        if (double_lp_callback) {
            if (dlp_state == DLP_IDLE) {
                dlp_state = DLP_FIRST_HOLDING;
                dlp_press_start_ms = now;
                dlp_first_done = false;
            } else if (dlp_state == DLP_WAIT_RELEASE) {
                dlp_state = DLP_SECOND_HOLDING;
                dlp_press_start_ms = now;
            } else {
                dlp_state = DLP_IDLE;
            }
        }
    }
    // Check for stable release (debounced)
    else if (!pressed && button_latched && (now - last_change_ms) > BUTTON_DEBOUNCE_MS) {
        button_latched = false;
        // DLP: falling edge
        if (double_lp_callback) {
            if (dlp_state == DLP_FIRST_HOLDING) {
                if (dlp_first_done) {
                    dlp_state = DLP_WAIT_RELEASE;
                    dlp_release_ms = now;
                } else {
                    dlp_state = DLP_IDLE;  // released too early
                }
            } else if (dlp_state == DLP_SECOND_HOLDING) {
                dlp_state = DLP_IDLE;  // released too early in second phase
            }
        }
    }

    // Check for long press — fires once after hold_ms of continuous hold
    if (pressed && button_latched && !long_press_fired &&
        long_press_callback && long_press_hold_ms > 0 &&
        (now - press_start_ms) >= long_press_hold_ms) {
        ESP_LOGW(TAG, "Long press detected (%lu ms)", (unsigned long)long_press_hold_ms);
        long_press_fired = true;
        long_press_callback();
    }

    // Double long-press polling
    if (double_lp_callback && double_lp_hold_ms > 0) {
        if (button_latched) {
            // Mark first hold done when button has been held long enough
            if (dlp_state == DLP_FIRST_HOLDING && !dlp_first_done &&
                (now - dlp_press_start_ms) >= double_lp_hold_ms) {
                dlp_first_done = true;
                // Wait for release; callback fires only after second hold
            }
            // Fire callback when second hold is complete
            if (dlp_state == DLP_SECOND_HOLDING &&
                (now - dlp_press_start_ms) >= double_lp_hold_ms) {
                ESP_LOGW(TAG, "Double long press confirmed");
                dlp_state = DLP_IDLE;
                double_lp_callback();
            }
        } else {
            // Not held: check if wait-release window has expired
            if (dlp_state == DLP_WAIT_RELEASE && double_lp_window_ms > 0 &&
                (now - dlp_release_ms) >= double_lp_window_ms) {
                dlp_state = DLP_IDLE;
            }
        }
    }
}
