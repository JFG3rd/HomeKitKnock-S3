#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint8_t *buf;        // JPEG data (free via timestamp_overlay_release)
    size_t   len;
    uint16_t width;
    uint16_t height;
} overlay_frame_t;

/**
 * Allocate PSRAM RGB buffer for max resolution.
 * Call once at boot after camera init.
 */
esp_err_t timestamp_overlay_init(uint16_t max_width, uint16_t max_height);

/** Check if overlay is currently enabled. */
bool timestamp_overlay_is_enabled(void);

/** Enable/disable overlay (in-memory only; caller persists to NVS). */
void timestamp_overlay_set_enabled(bool enabled);

/**
 * Decode JPEG, draw timestamp, re-encode.
 * On success, populates *out. Caller MUST call timestamp_overlay_release(out).
 * Returns false on failure (OOM, decode error, mutex contention).
 * quality: 0-100 (fmt2jpg scale, higher = better).
 */
bool timestamp_overlay_apply(const uint8_t *jpeg_in, size_t jpeg_len,
                             uint16_t width, uint16_t height,
                             uint8_t quality, overlay_frame_t *out);

/** Free the JPEG buffer inside an overlay frame. */
void timestamp_overlay_release(overlay_frame_t *frame);

/** Check if camera name overlay is currently enabled. */
bool camera_name_overlay_is_enabled(void);

/** Enable/disable camera name overlay (in-memory only; caller persists to NVS). */
void camera_name_overlay_set_enabled(bool enabled);

/** Set camera name string (max 24 chars). Thread-safe. */
void camera_name_overlay_set_name(const char *name);

/** Get current camera name string. */
const char *camera_name_overlay_get_name(void);
