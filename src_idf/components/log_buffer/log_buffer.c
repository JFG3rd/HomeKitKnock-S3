/**
 * Log Buffer Component Implementation
 *
 * Uses a ring buffer to store log entries and hooks into ESP-IDF's
 * logging system via esp_log_set_vprintf().
 */

#include "log_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>  // for strcasecmp
#include <time.h>
#include <sys/time.h>

// Ring buffer configuration
#define LOG_BUFFER_SIZE 100  // Number of log entries to store

// Ring buffer
static log_entry_t log_entries[LOG_BUFFER_SIZE];
static int log_head = 0;        // Next write position
static int log_count = 0;       // Current number of entries
static SemaphoreHandle_t log_mutex = NULL;

// Static buffer for log formatting (avoids stack overflow in event tasks)
static char log_format_buffer[256];
static SemaphoreHandle_t format_mutex = NULL;

// Original vprintf function (for passing through to serial)
static vprintf_like_t original_vprintf = NULL;

// Tags for each filter category
static const char *core_tags[] = {
    "main", "wifi", "wifi_mgr", "nvs", "nvs_mgr", "web_server", "httpd",
    "dns", "dns_server", "esp_netif", "system_api", "heap_init", "cpu_start",
    "esp_image", "boot", "spi_flash", NULL
};

static const char *camera_tags[] = {
    "camera", "cam", "rtsp", "mjpeg", "stream", "ov2640", "s3_eye",
    "jpeg", "fb_alloc", "video", NULL
};

static const char *doorbell_tags[] = {
    "doorbell", "sip", "sip_client", "button", "ring", "tr064", "tr-064",
    "gpio", "relay", "audio", "i2s", "mic", NULL
};

/**
 * Check if tag matches any in a list
 */
static bool tag_in_list(const char *tag, const char **list) {
    for (int i = 0; list[i] != NULL; i++) {
        if (strcasecmp(tag, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool log_buffer_tag_matches_filter(const char *tag, log_filter_t filter) {
    if (filter == LOG_FILTER_ALL) {
        return true;
    }

    switch (filter) {
        case LOG_FILTER_CORE:
            return tag_in_list(tag, core_tags);
        case LOG_FILTER_CAMERA:
            return tag_in_list(tag, camera_tags);
        case LOG_FILTER_DOORBELL:
            return tag_in_list(tag, doorbell_tags);
        default:
            return true;
    }
}

/**
 * Parse log format and extract tag and message
 * ESP-IDF format: "X (timestamp) tag: message\n"
 */
static void parse_log_line(const char *line, esp_log_level_t *level, char *tag, size_t tag_size, char *msg, size_t msg_size) {
    // Default values
    *level = ESP_LOG_INFO;
    tag[0] = '\0';
    msg[0] = '\0';

    if (!line || strlen(line) < 3) return;

    // Parse level from first character
    switch (line[0]) {
        case 'E': *level = ESP_LOG_ERROR; break;
        case 'W': *level = ESP_LOG_WARN; break;
        case 'I': *level = ESP_LOG_INFO; break;
        case 'D': *level = ESP_LOG_DEBUG; break;
        case 'V': *level = ESP_LOG_VERBOSE; break;
        default: break;
    }

    // Find tag (after closing paren and space)
    const char *tag_start = strchr(line, ')');
    if (tag_start) {
        tag_start += 2;  // Skip ") "
        const char *tag_end = strchr(tag_start, ':');
        if (tag_end) {
            size_t len = tag_end - tag_start;
            if (len >= tag_size) len = tag_size - 1;
            strncpy(tag, tag_start, len);
            tag[len] = '\0';

            // Get message (after ": ")
            const char *msg_start = tag_end + 2;
            size_t msg_len = strlen(msg_start);
            // Remove trailing newline
            while (msg_len > 0 && (msg_start[msg_len-1] == '\n' || msg_start[msg_len-1] == '\r')) {
                msg_len--;
            }
            if (msg_len >= msg_size) msg_len = msg_size - 1;
            strncpy(msg, msg_start, msg_len);
            msg[msg_len] = '\0';
        }
    }
}

/**
 * Custom vprintf that captures logs to ring buffer
 * Uses static buffer to avoid stack overflow in event tasks
 */
static int log_vprintf(const char *fmt, va_list args) {
    // Make a copy of args for the original handler
    va_list args_copy;
    va_copy(args_copy, args);

    // Try to get format mutex (non-blocking to avoid deadlocks)
    if (format_mutex && xSemaphoreTake(format_mutex, 0) == pdTRUE) {
        // Format the log message using static buffer
        int len = vsnprintf(log_format_buffer, sizeof(log_format_buffer), fmt, args);

        // Parse and store in ring buffer (non-blocking)
        if (log_mutex && xSemaphoreTake(log_mutex, 0) == pdTRUE) {
            log_entry_t *entry = &log_entries[log_head];

            // Use actual time if SNTP synced (year > 2020), otherwise use uptime
            time_t now = time(NULL);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            if (timeinfo.tm_year > 120) {  // Year > 2020 means SNTP synced
                entry->timestamp_ms = (uint32_t)now;  // Unix timestamp in seconds
            } else {
                entry->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);  // Uptime in ms
            }
            parse_log_line(log_format_buffer, &entry->level, entry->tag, sizeof(entry->tag),
                           entry->message, sizeof(entry->message));

            // Only store if we got a valid tag
            if (entry->tag[0] != '\0') {
                log_head = (log_head + 1) % LOG_BUFFER_SIZE;
                if (log_count < LOG_BUFFER_SIZE) {
                    log_count++;
                }
            }

            xSemaphoreGive(log_mutex);
        }

        xSemaphoreGive(format_mutex);
        (void)len;  // Suppress unused warning
    }

    // Always pass through to original handler (serial output)
    int result = 0;
    if (original_vprintf) {
        result = original_vprintf(fmt, args_copy);
    }

    va_end(args_copy);
    return result;
}

esp_err_t log_buffer_init(void) {
    // Create mutexes
    log_mutex = xSemaphoreCreateMutex();
    format_mutex = xSemaphoreCreateMutex();
    if (!log_mutex || !format_mutex) {
        return ESP_ERR_NO_MEM;
    }

    // Clear buffer
    memset(log_entries, 0, sizeof(log_entries));
    log_head = 0;
    log_count = 0;

    // Hook into ESP-IDF logging
    original_vprintf = esp_log_set_vprintf(log_vprintf);

    // Log that we started (this will be captured too!)
    ESP_LOGI("log_buffer", "Log buffer initialized (%d entries)", LOG_BUFFER_SIZE);

    return ESP_OK;
}

void log_buffer_clear(void) {
    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(log_entries, 0, sizeof(log_entries));
        log_head = 0;
        log_count = 0;
        xSemaphoreGive(log_mutex);
    }
}

int log_buffer_count(void) {
    return log_count;
}

/**
 * Escape a string for JSON
 */
static int json_escape(char *dest, size_t dest_size, const char *src) {
    int written = 0;
    while (*src && written < (int)dest_size - 2) {
        switch (*src) {
            case '"':  dest[written++] = '\\'; dest[written++] = '"'; break;
            case '\\': dest[written++] = '\\'; dest[written++] = '\\'; break;
            case '\n': dest[written++] = '\\'; dest[written++] = 'n'; break;
            case '\r': dest[written++] = '\\'; dest[written++] = 'r'; break;
            case '\t': dest[written++] = '\\'; dest[written++] = 't'; break;
            default:
                if (*src >= 32) {
                    dest[written++] = *src;
                }
                break;
        }
        src++;
    }
    dest[written] = '\0';
    return written;
}

int log_buffer_get_json(char *buffer, size_t buffer_size, log_filter_t filter, int max_entries) {
    if (!buffer || buffer_size < 100) return 0;

    int offset = 0;
    offset += snprintf(buffer + offset, buffer_size - offset, "{\"logs\":[");

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int entries_written = 0;
        int start_idx;

        // Calculate start index (oldest entry)
        if (log_count < LOG_BUFFER_SIZE) {
            start_idx = 0;
        } else {
            start_idx = log_head;  // Oldest is at current head position
        }

        // Iterate through entries from oldest to newest
        for (int i = 0; i < log_count && offset < (int)buffer_size - 300; i++) {
            int idx = (start_idx + i) % LOG_BUFFER_SIZE;
            log_entry_t *entry = &log_entries[idx];

            // Skip empty entries
            if (entry->tag[0] == '\0') continue;

            // Apply filter
            if (!log_buffer_tag_matches_filter(entry->tag, filter)) {
                continue;
            }

            // Check max entries limit
            if (max_entries > 0 && entries_written >= max_entries) {
                break;
            }

            // Add comma separator
            if (entries_written > 0) {
                offset += snprintf(buffer + offset, buffer_size - offset, ",");
            }

            // Escape message for JSON
            char escaped_msg[250];
            json_escape(escaped_msg, sizeof(escaped_msg), entry->message);

            // Level to string
            const char *level_str;
            switch (entry->level) {
                case ESP_LOG_ERROR:   level_str = "E"; break;
                case ESP_LOG_WARN:    level_str = "W"; break;
                case ESP_LOG_INFO:    level_str = "I"; break;
                case ESP_LOG_DEBUG:   level_str = "D"; break;
                case ESP_LOG_VERBOSE: level_str = "V"; break;
                default:              level_str = "?"; break;
            }

            offset += snprintf(buffer + offset, buffer_size - offset,
                "{\"ts\":%lu,\"lvl\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
                (unsigned long)entry->timestamp_ms,
                level_str,
                entry->tag,
                escaped_msg);

            entries_written++;
        }

        xSemaphoreGive(log_mutex);
    }

    offset += snprintf(buffer + offset, buffer_size - offset,
        "],\"count\":%d,\"capacity\":%d}", log_count, LOG_BUFFER_SIZE);

    return offset;
}
