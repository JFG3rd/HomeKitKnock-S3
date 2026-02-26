/**
 * Relay Controller Component
 *
 * Drives a GPIO output high for a configurable pulse duration, then
 * returns it low.  The pulse runs in a dedicated FreeRTOS task so the
 * caller is not blocked during the delay.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the gong relay GPIO (GONG_RELAY_PIN) as output, default LOW.
 *
 * @return ESP_OK on success
 */
esp_err_t relay_controller_init(void);

/**
 * Pulse the gong relay HIGH for the specified duration, then LOW.
 * Spawns a minimal FreeRTOS task; returns immediately.
 *
 * @param ms Pulse duration in milliseconds
 */
void relay_controller_pulse_async(uint32_t ms);

#ifdef __cplusplus
}
#endif
