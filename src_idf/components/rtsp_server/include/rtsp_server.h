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

/**
 * Set RTSP Basic Auth credentials.
 * Pass empty string or NULL for user to disable authentication.
 *
 * @param user  Username (max 31 chars). NULL or "" = open access.
 * @param pass  Password (max 63 chars).
 */
void rtsp_server_set_credentials(const char *user, const char *pass);

/**
 * Get total UDP video send failures since boot (or last reset).
 */
int rtsp_server_udp_fail_count(void);

/**
 * Reset the UDP fail counter to zero.
 */
void rtsp_server_udp_fail_reset(void);

/**
 * Get the maximum UDP backoff remaining across all active sessions (ms).
 * Returns 0 if no session is in backoff.
 */
uint32_t rtsp_server_udp_backoff_max_ms(void);

/**
 * Enable or disable low-latency socket tuning for new RTSP client connections.
 *
 * When enabled, SO_SNDBUF is reduced to minimize kernel buffering and a tight
 * send timeout is applied. TCP_NODELAY is always on regardless of this setting.
 *
 * @param enabled true to enable low-latency socket tuning
 */
void rtsp_server_set_low_latency(bool enabled);

/**
 * Get client IP strings for all active (playing) RTSP sessions.
 *
 * @param out   Array of char[16] buffers to fill with dotted-decimal IPs.
 * @param max   Maximum number of entries to fill (should match array size).
 * @param count Set to the number of entries filled on return.
 */
void rtsp_server_get_client_ips(char out[][16], int max, int *count);

#ifdef __cplusplus
}
#endif
