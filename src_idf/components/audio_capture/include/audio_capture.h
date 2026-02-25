/**
 * Audio Capture Component
 *
 * I2S microphone input with source selection (onboard PDM or external INMP441).
 * Mic source is a boot-time config (NVS, requires reboot to change).
 * Provides PCM samples to both RTSP (AAC encoder) and SIP (G.711).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIC_SOURCE_PDM = 0,       // Onboard PDM mic (GPIO41/42), I2S_NUM_0
    MIC_SOURCE_INMP441 = 1,   // External INMP441 I2S mic (GPIO43/44/12), shared I2S_NUM_1 full-duplex bus
} mic_source_t;

/**
 * Initialize audio capture. Reads config from NVS. Does NOT start I2S.
 */
esp_err_t audio_capture_init(void);

/**
 * Start I2S capture on the configured mic source.
 * Safe to call multiple times (no-op if already running).
 */
esp_err_t audio_capture_start(void);

/**
 * Stop I2S capture and uninstall driver.
 */
void audio_capture_stop(void);

/**
 * Read PCM samples from the mic. Applies software sensitivity scaling.
 * Returns silence if muted. Blocks up to timeout_ms.
 *
 * @param buffer      Output: 16-bit signed PCM samples
 * @param sample_count Number of samples to read
 * @param timeout_ms  Maximum wait time
 * @return true if samples were read, false on error/timeout
 */
bool audio_capture_read(int16_t *buffer, size_t sample_count, uint32_t timeout_ms);

/**
 * True when the capture channel is currently active.
 */
bool audio_capture_is_running(void);

/**
 * Get current mic source (read from NVS at init, boot-time only).
 */
mic_source_t audio_capture_get_source(void);

/**
 * Runtime setters (also persist to NVS via camera namespace).
 */
void audio_capture_set_sensitivity(uint8_t percent);

bool audio_capture_is_enabled(void);
bool audio_capture_is_muted(void);
uint8_t audio_capture_get_sensitivity(void);

#ifdef __cplusplus
}
#endif
