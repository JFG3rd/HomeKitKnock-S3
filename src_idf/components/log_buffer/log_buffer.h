/**
 * Log Buffer Component
 *
 * Captures ESP-IDF log output into a ring buffer for web-based viewing.
 * Hooks into esp_log system to intercept all log messages.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Log entry structure
typedef struct {
    uint32_t timestamp_ms;      // Unix timestamp (seconds) if SNTP synced, else uptime (ms)
    esp_log_level_t level;      // Log level (E/W/I/D/V)
    char tag[24];               // Component tag
    char message[200];          // Log message (truncated if needed)
} log_entry_t;

// Log filter categories
typedef enum {
    LOG_FILTER_ALL = 0,
    LOG_FILTER_CORE,        // main, wifi, nvs, web_server, dns, httpd
    LOG_FILTER_CAMERA,      // camera, rtsp, mjpeg, stream, ov2640
    LOG_FILTER_DOORBELL,    // doorbell, sip, button, ring, tr064
} log_filter_t;

/**
 * Initialize the log buffer and hook into ESP-IDF logging
 *
 * @return ESP_OK on success
 */
esp_err_t log_buffer_init(void);

/**
 * Get log entries as JSON string
 *
 * @param buffer Output buffer for JSON
 * @param buffer_size Size of output buffer
 * @param filter Log filter category
 * @param max_entries Maximum entries to return (0 = all)
 * @return Number of bytes written to buffer
 */
int log_buffer_get_json(char *buffer, size_t buffer_size, log_filter_t filter, int max_entries);

/**
 * Clear all log entries
 */
void log_buffer_clear(void);

/**
 * Get number of log entries in buffer
 *
 * @return Number of entries
 */
int log_buffer_count(void);

/**
 * Check if a tag matches a filter category
 *
 * @param tag Log tag to check
 * @param filter Filter category
 * @return true if tag matches filter
 */
bool log_buffer_tag_matches_filter(const char *tag, log_filter_t filter);

#ifdef __cplusplus
}
#endif
