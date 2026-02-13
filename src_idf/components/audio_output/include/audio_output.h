/**
 * Audio Output Component
 *
 * MAX98357A I2S DAC speaker output on I2S_NUM_1.
 * Only available when mic source is PDM (not INMP441, which uses I2S_NUM_1).
 * Used for SIP call audio playback and gong doorbell sound.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize audio output (MAX98357A on I2S_NUM_1).
 * Returns ESP_ERR_NOT_SUPPORTED if INMP441 mic owns I2S_NUM_1.
 */
esp_err_t audio_output_init(void);

/**
 * Deinitialize and free I2S resources.
 */
void audio_output_deinit(void);

/**
 * Write PCM samples to speaker. Applies volume scaling.
 * Blocks up to timeout_ms.
 *
 * @return true if samples were written
 */
bool audio_output_write(const int16_t *samples, size_t count, uint32_t timeout_ms);

/**
 * Check if speaker is available (false if INMP441 owns I2S_NUM_1).
 */
bool audio_output_is_available(void);

/**
 * Set speaker volume (0-100, applied as PCM scaling).
 */
void audio_output_set_volume(uint8_t percent);
uint8_t audio_output_get_volume(void);

/**
 * Play doorbell gong sound asynchronously (fire-and-forget task).
 * Uses embedded PCM from flash, falls back to synthesized 2-tone.
 */
void audio_output_play_gong(void);

/**
 * Play a 1 kHz test tone for 1 second (debug/diagnostic).
 */
void audio_output_play_test_tone(void);

#ifdef __cplusplus
}
#endif
