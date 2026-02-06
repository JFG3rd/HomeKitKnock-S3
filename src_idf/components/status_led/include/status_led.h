/**
 * Status LED Component
 *
 * PWM-controlled status LED with various patterns.
 * GPIO2, active-high, 330 ohm to LED.
 *
 * Patterns (priority order):
 * 1. Ringing: breathing (dim in/out)
 * 2. AP mode: fast double blink
 * 3. WiFi connecting: 2 Hz blink
 * 4. SIP error: slow pulse
 * 5. SIP ok: steady low glow
 * 6. RTSP active: short tick every 2 seconds
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// LED configuration
#define STATUS_LED_GPIO         2
#define STATUS_LED_ACTIVE_LOW   0

// LED states (priority order)
typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_RTSP_ACTIVE,
    LED_STATE_SIP_OK,
    LED_STATE_SIP_ERROR,
    LED_STATE_WIFI_CONNECTING,
    LED_STATE_AP_MODE,
    LED_STATE_RINGING,
} led_state_t;

/**
 * Initialize the status LED
 *
 * @return ESP_OK on success
 */
esp_err_t status_led_init(void);

/**
 * Update the LED pattern (call from main loop)
 *
 * This updates the PWM based on current state and timing.
 */
void status_led_update(void);

/**
 * Set the LED state
 *
 * The highest priority state will be displayed.
 *
 * @param state The state to set
 * @param active Whether this state is active
 */
void status_led_set_state(led_state_t state, bool active);

/**
 * Mark the LED as ringing (starts breathing animation)
 *
 * The ring animation lasts for a fixed duration.
 */
void status_led_mark_ring(void);

/**
 * Check if ring animation is active
 *
 * @return true if ringing animation is active
 */
bool status_led_is_ringing(void);

#ifdef __cplusplus
}
#endif
