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

// Callback types for button events
typedef void (*button_press_callback_t)(void);
typedef void (*button_long_press_callback_t)(void);

/**
 * Initialize the button GPIO
 *
 * @return ESP_OK on success
 */
esp_err_t button_init(void);

/**
 * Register a callback for button press events (debounced, fires on initial press)
 *
 * @param callback Function to call when button is pressed
 */
void button_set_callback(button_press_callback_t callback);

/**
 * Register a callback for long-press events
 *
 * @param callback  Function to call when button is held for hold_ms milliseconds
 * @param hold_ms   Hold duration in ms required to trigger the long press
 */
void button_set_long_press_callback(button_long_press_callback_t callback, uint32_t hold_ms);

/**
 * Register a callback for double long-press: hold hold_ms → release (within window) → hold hold_ms again.
 *
 * @param callback          Function to call when the gesture completes
 * @param hold_ms           Duration each hold phase must last (e.g. 5000)
 * @param release_window_ms Max time allowed for the release gap (e.g. 3000)
 */
void button_set_double_long_press_callback(button_long_press_callback_t callback,
                                           uint32_t hold_ms,
                                           uint32_t release_window_ms);

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
