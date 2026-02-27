/**
 * Relay Controller Component
 *
 * Controls GONG_RELAY_PIN (GPIO3) and DOOR_OPENER_PIN (GPIO1).
 * Each pulse runs in a FreeRTOS task and returns immediately to the caller.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize both relay GPIOs as outputs, default LOW.
 */
esp_err_t relay_controller_init(void);

/**
 * Set the gong relay (GPIO3) pulse duration in ms.
 * Cached in RAM; call at startup after loading from NVS.
 */
void relay_controller_set_gong_ms(uint32_t ms);
uint32_t relay_controller_get_gong_ms(void);

/**
 * Set the door opener relay (GPIO1) pulse duration in ms.
 */
void relay_controller_set_door_ms(uint32_t ms);
uint32_t relay_controller_get_door_ms(void);

/**
 * Pulse GPIO3 (gong) HIGH for the configured duration.
 * A 150ms startup delay is applied before asserting HIGH so that
 * the audio I2S driver can initialize before the relay fires.
 * Returns immediately; pulse runs in a background task.
 */
void relay_controller_pulse_gong(void);

/**
 * Pulse GPIO1 (door opener) HIGH for the configured duration.
 * Returns immediately; pulse runs in a background task.
 */
void relay_controller_pulse_door(void);

#ifdef __cplusplus
}
#endif
