/**
 * Button Component
 *
 * Handles doorbell button with debouncing.
 * GPIO4, active-low with internal pull-up.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Button configuration
#define BUTTON_GPIO         4
#define BUTTON_ACTIVE_LOW   1
#define BUTTON_DEBOUNCE_MS  50

// Callback type for button press events
typedef void (*button_press_callback_t)(void);

/**
 * Initialize the button GPIO
 *
 * @return ESP_OK on success
 */
esp_err_t button_init(void);

/**
 * Register a callback for button press events
 *
 * @param callback Function to call when button is pressed (debounced)
 */
void button_set_callback(button_press_callback_t callback);

/**
 * Poll the button state (call from main loop)
 *
 * This handles debouncing and triggers the callback on press.
 */
void button_poll(void);

/**
 * Check if button is currently pressed (raw state, no debounce)
 *
 * @return true if button is pressed
 */
bool button_is_pressed(void);

#ifdef __cplusplus
}
#endif
