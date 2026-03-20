/**
 * Camera Component
 *
 * Wraps esp_camera for OV2640 on XIAO ESP32-S3 Sense.
 * VGA JPEG output with PSRAM frame buffers.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "timestamp_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Unified frame type returned by camera_capture_frame().
 * Consumers use .buf and .len; call camera_release_frame() when done.
 */
typedef struct {
    const uint8_t *buf;
    size_t len;
    uint16_t width;
    uint16_t height;
    // Private — do not access directly:
    camera_fb_t *_fb;           // non-NULL = raw path (no overlay)
    overlay_frame_t _overlay;   // valid when _fb == NULL (overlay path)
} captured_frame_t;

/**
 * Initialize the camera
 *
 * Configures OV2640 with XIAO ESP32-S3 pin map,
 * VGA resolution, JPEG format, 2 frame buffers in PSRAM.
 *
 * @return ESP_OK on success
 */
esp_err_t camera_init(void);

/**
 * Capture a JPEG frame
 *
 * Returns a pointer to the frame buffer. Caller MUST return it
 * via camera_return_fb() when done.
 *
 * @return Frame buffer pointer, or NULL on failure
 */
camera_fb_t *camera_capture(void);

/**
 * Return a frame buffer to the camera driver
 *
 * @param fb Frame buffer to return
 */
void camera_return_fb(camera_fb_t *fb);

/**
 * Capture a frame with optional timestamp overlay.
 * If overlay is enabled, decodes/draws/re-encodes the JPEG.
 * If overlay is disabled or fails, returns the raw camera frame.
 * Caller MUST call camera_release_frame() when done.
 *
 * @param out Pointer to captured_frame_t to populate
 * @return true on success, false if capture failed
 */
bool camera_capture_frame(captured_frame_t *out);

/**
 * Release a frame obtained via camera_capture_frame().
 * Handles both raw and overlay paths.
 */
void camera_release_frame(captured_frame_t *frame);

/**
 * Check if camera is initialized and ready
 *
 * @return true if camera is ready for capture
 */
bool camera_is_ready(void);

/**
 * Check if HTTP camera streaming feature is enabled in settings
 *
 * @return true if enabled (default: false)
 */
bool camera_is_enabled(void);

/**
 * Set HTTP camera streaming feature enabled state (persisted to NVS)
 *
 * @param enabled true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t camera_set_enabled(bool enabled);

/**
 * Check if RTSP camera streaming feature is enabled in settings
 *
 * @return true if enabled (default: false)
 */
bool camera_is_rtsp_enabled(void);

/**
 * Set RTSP camera streaming feature enabled state (persisted to NVS)
 *
 * @param enabled true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t camera_set_rtsp_enabled(bool enabled);

/**
 * Get/set RTSP UDP transport preference (persisted to NVS).
 * When false (default), only TCP interleaved transport is offered.
 */
bool camera_is_rtsp_udp_enabled(void);
esp_err_t camera_set_rtsp_udp_enabled(bool enabled);

/**
 * Set a camera control variable at runtime (also persists to NVS)
 *
 * Supported vars: "framesize", "quality", "brightness", "contrast"
 *
 * @param var Variable name
 * @param val Integer value
 * @return ESP_OK on success
 */
esp_err_t camera_set_control(const char *var, int val);

/**
 * Audio output (gong/speaker) enable state (persisted to NVS)
 */
bool camera_is_audio_out_enabled(void);
esp_err_t camera_set_audio_out_enabled(bool enabled);

bool camera_is_audio_out_muted(void);
esp_err_t camera_set_audio_out_muted(bool muted);

bool camera_is_hardware_diag_enabled(void);
esp_err_t camera_set_hardware_diag_enabled(bool enabled);

/**
 * Set camera name for overlay (max 24 chars, persisted to NVS)
 */
esp_err_t camera_set_name(const char *name);

/**
 * Get current camera settings as JSON
 *
 * Writes JSON like: {"framesize":8,"quality":10,"brightness":0,"contrast":0,"PID":"0x0026"}
 *
 * @param buf Output buffer
 * @param buf_size Buffer size
 */
void camera_get_status_json(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
