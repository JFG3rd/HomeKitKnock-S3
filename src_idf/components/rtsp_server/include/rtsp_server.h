/**
 * RTSP Server Component
 *
 * Custom RTSP 1.0 server for MJPEG streaming over RTP.
 * Port 8554, TCP interleaved + UDP transport.
 * Implements RFC 2326 (RTSP) and RFC 2435 (RTP/JPEG).
 *
 * Ported from Arduino version (src_arduino/rtsp_server.cpp).
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the RTSP server on port 8554
 *
 * Requires camera to be initialized first.
 *
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server and close all sessions
 */
void rtsp_server_stop(void);

/**
 * Check if the RTSP server is running
 *
 * @return true if running
 */
bool rtsp_server_is_running(void);

/**
 * Get number of active (playing) RTSP sessions
 *
 * @return Number of active sessions
 */
int rtsp_server_active_session_count(void);

/**
 * Enable or disable UDP transport for RTSP clients
 *
 * When disabled, only TCP interleaved transport is accepted.
 *
 * @param allow true to allow UDP, false for TCP-only
 */
void rtsp_server_set_allow_udp(bool allow);

#ifdef __cplusplus
}
#endif
