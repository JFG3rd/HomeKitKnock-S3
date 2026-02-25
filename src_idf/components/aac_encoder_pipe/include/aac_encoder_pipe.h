/**
 * AAC Encoder Pipeline
 *
 * ESP-ADF audio pipeline: raw_stream → aac_encoder → raw_stream
 * Captures PCM from audio_capture, encodes to AAC-LC,
 * provides raw AAC frames (no ADTS) for RTP packetization.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AAC_FRAME_SAMPLES 1024

/**
 * Initialize the AAC encoder pipeline.
 * Reads aac_rate and aac_bitr from NVS. Creates ESP-ADF pipeline.
 * audio_capture must be initialized first.
 */
esp_err_t aac_encoder_pipe_init(void);

/**
 * Deinitialize and free all pipeline resources.
 */
void aac_encoder_pipe_deinit(void);

/**
 * Capture one AAC frame: reads PCM from mic, encodes, returns raw AAC.
 * Output is raw AAC (no ADTS header), suitable for RTP packetization.
 *
 * @param out     Output buffer for raw AAC frame
 * @param out_max Size of output buffer (recommend 2048)
 * @param out_len Actual bytes written
 * @return true if a frame was encoded successfully
 */
bool aac_encoder_pipe_get_frame(uint8_t *out, size_t out_max, size_t *out_len);

/**
 * Get current encoder sample rate in Hz (8000 or 16000).
 */
uint32_t aac_encoder_pipe_get_sample_rate(void);

/**
 * Get SDP rtpmap string, e.g. "MPEG4-GENERIC/16000/1"
 */
void aac_encoder_pipe_get_sdp_rtpmap(char *buf, size_t sz);

/**
 * Get SDP fmtp string (without "96 " prefix).
 */
void aac_encoder_pipe_get_sdp_fmtp(char *buf, size_t sz);

#ifdef __cplusplus
}
#endif
