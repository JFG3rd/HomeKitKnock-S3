/**
 * MJPEG Streaming Server
 *
 * HTTP MJPEG streaming on port 81 using raw lwIP sockets.
 * Supports up to 2 concurrent clients streaming from the OV2640 camera.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the MJPEG streaming server on port 81
 *
 * Creates a listener task on core 1 that accepts connections
 * and spawns per-client streaming tasks.
 *
 * @return ESP_OK on success
 */
esp_err_t mjpeg_server_start(void);

/**
 * Stop the MJPEG server and disconnect all clients
 */
void mjpeg_server_stop(void);

/**
 * Get the number of active streaming clients
 *
 * @return Number of connected clients (0-2)
 */
uint8_t mjpeg_server_client_count(void);

/**
 * Check if the MJPEG server is running
 *
 * @return true if server is accepting connections
 */
bool mjpeg_server_is_running(void);

/**
 * Get client IP strings for all active MJPEG streaming clients.
 *
 * @param out   Array of char[16] buffers to fill with dotted-decimal IPs.
 * @param max   Maximum number of entries to fill (should match array size).
 * @param count Set to the number of entries filled on return.
 */
void mjpeg_server_get_client_ips(char out[][16], int max, int *count);

/** Set max concurrent MJPEG clients (1-4). Persists to NVS. */
void mjpeg_server_set_max_clients(int val);

/** Get current max concurrent MJPEG clients. */
int mjpeg_server_get_max_clients(void);

/** Load max clients setting from NVS (call at init before start). */
void mjpeg_server_load_max_clients(void);

#ifdef __cplusplus
}
#endif
