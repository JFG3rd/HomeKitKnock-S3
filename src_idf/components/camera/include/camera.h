/**
 * Camera Component
 *
 * Wraps esp_camera for OV2640 on XIAO ESP32-S3 Sense.
 * VGA JPEG output with PSRAM frame buffers.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

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
