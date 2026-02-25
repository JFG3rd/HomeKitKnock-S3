/**
 * Audio Output Component
 *
 * MAX98357A I2S DAC speaker output on shared I2S_NUM_1 full-duplex bus.
 * Shares BCLK + WS with external INMP441 mic while using separate data lines.
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
 * Initialize audio output (MAX98357A on shared I2S_NUM_1).
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
 * Flush DMA silence and disable TX channel after a bulk write.
 * Call after audio_output_write() to prevent DMA circular-buffer replay.
 */
void audio_output_flush_and_stop(void);

/**
 * Check if speaker output path is available (always true on this hardware).
 */
bool audio_output_is_available(void);

/**
 * Check if audio_output_init() has completed successfully.
 */
bool audio_output_is_initialized(void);

/**
 * Set speaker volume (0-100, applied as PCM scaling).
 */
void audio_output_set_volume(uint8_t percent);
uint8_t audio_output_get_volume(void);

void audio_output_set_hardware_diagnostic_mode(bool enabled);
bool audio_output_get_hardware_diagnostic_mode(void);

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
