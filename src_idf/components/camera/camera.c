/**
 * Camera Component Implementation
 *
 * OV2640 camera driver for XIAO ESP32-S3 Sense.
 * Uses esp_camera with PSRAM frame buffers.
 */

#include "camera.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "camera_pins.h"

static const char *TAG = "camera";
static bool camera_ready = false;

#include <string.h>
#include <stdio.h>

#define NVS_CAMERA_NAMESPACE "camera"
#define NVS_KEY_ENABLED      "http_cam_en"
#define NVS_KEY_FRAMESIZE    "framesize"
#define NVS_KEY_QUALITY      "quality"
#define NVS_KEY_BRIGHTNESS   "brightness"
#define NVS_KEY_CONTRAST     "contrast"
#define NVS_KEY_MIC_ENABLED  "mic_en"
#define NVS_KEY_MIC_MUTED    "mic_mute"
#define NVS_KEY_MIC_SENS     "mic_sens"
#define NVS_KEY_AAC_RATE     "aac_rate"
#define NVS_KEY_AAC_BITRATE  "aac_bitr"

esp_err_t camera_init(void) {
    if (camera_ready) {
        ESP_LOGW(TAG, "Camera already initialized");
        return ESP_OK;
    }

    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,

        .xclk_freq_hz = 10000000,  // 10MHz for stability
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,     // 640x480
        .jpeg_quality = 10,              // 0-63, lower = better quality
        .fb_count = 2,                   // Double buffer
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST, // Always get newest frame
    };

    ESP_LOGI(TAG, "Initializing camera (VGA JPEG, 2 buffers in PSRAM)...");

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return err;
    }

    // Apply OV2640 sensor defaults
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_saturation(s, 2);
        s->set_aec2(s, 1);
        s->set_gainceiling(s, GAINCEILING_128X);
        s->set_lenc(s, 1);
        ESP_LOGI(TAG, "Sensor PID: 0x%04x", s->id.PID);
    }

    camera_ready = true;

    // Load saved settings from NVS and apply
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t val8;
        int8_t val8s;

        if (s) {
            if (nvs_get_u8(handle, NVS_KEY_FRAMESIZE, &val8) == ESP_OK) {
                s->set_framesize(s, (framesize_t)val8);
                ESP_LOGI(TAG, "Restored framesize: %d", val8);
            }
            if (nvs_get_u8(handle, NVS_KEY_QUALITY, &val8) == ESP_OK) {
                s->set_quality(s, val8);
                ESP_LOGI(TAG, "Restored quality: %d", val8);
            }
            if (nvs_get_i8(handle, NVS_KEY_BRIGHTNESS, &val8s) == ESP_OK) {
                s->set_brightness(s, val8s);
                ESP_LOGI(TAG, "Restored brightness: %d", val8s);
            }
            if (nvs_get_i8(handle, NVS_KEY_CONTRAST, &val8s) == ESP_OK) {
                s->set_contrast(s, val8s);
                ESP_LOGI(TAG, "Restored contrast: %d", val8s);
            }
        }
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Camera initialized successfully");
    return ESP_OK;
}

camera_fb_t *camera_capture(void) {
    if (!camera_ready) {
        return NULL;
    }
    return esp_camera_fb_get();
}

void camera_return_fb(camera_fb_t *fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

bool camera_is_ready(void) {
    return camera_ready;
}

bool camera_is_enabled(void) {
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;  // Default to disabled
    }

    uint8_t enabled = 0;
    nvs_get_u8(handle, NVS_KEY_ENABLED, &enabled);
    nvs_close(handle);

    return enabled != 0;
}

esp_err_t camera_set_enabled(bool enabled) {
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "HTTP camera streaming %s", enabled ? "enabled" : "disabled");
    return err;
}

esp_err_t camera_set_control(const char *var, int val) {
    const char *nvs_key = NULL;
    bool is_signed = false;

    // --- Mic/audio settings (NVS-only, no hardware yet) ---
    if (strcmp(var, "mic_enabled") == 0) {
        nvs_key = NVS_KEY_MIC_ENABLED;
    } else if (strcmp(var, "mic_muted") == 0) {
        nvs_key = NVS_KEY_MIC_MUTED;
    } else if (strcmp(var, "mic_sensitivity") == 0) {
        if (val >= 0 && val <= 100) {
            nvs_key = NVS_KEY_MIC_SENS;
        }
    } else if (strcmp(var, "aac_sample_rate") == 0) {
        if (val == 8 || val == 16) {
            nvs_key = NVS_KEY_AAC_RATE;
        }
    } else if (strcmp(var, "aac_bitrate") == 0) {
        if (val >= 16 && val <= 48) {
            nvs_key = NVS_KEY_AAC_BITRATE;
        }
    } else {
        // --- Camera sensor settings (require camera hardware) ---
        if (!camera_ready) return ESP_ERR_INVALID_STATE;

        sensor_t *s = esp_camera_sensor_get();
        if (!s) return ESP_ERR_INVALID_STATE;

        // OV2640 set_brightness/set_contrast use WRITE_REG_OR_RETURN which
        // may return non-zero even when the setting was applied.
        // We always persist to NVS if in range.
        int res = -1;

        if (strcmp(var, "framesize") == 0) {
            if (val >= 0 && val <= 13) {
                res = s->set_framesize(s, (framesize_t)val);
                nvs_key = NVS_KEY_FRAMESIZE;
            }
        } else if (strcmp(var, "quality") == 0) {
            if (val >= 4 && val <= 63) {
                res = s->set_quality(s, val);
                nvs_key = NVS_KEY_QUALITY;
            }
        } else if (strcmp(var, "brightness") == 0) {
            if (val >= -2 && val <= 2) {
                s->set_brightness(s, val);
                nvs_key = NVS_KEY_BRIGHTNESS;
                is_signed = true;
                res = 0;
            }
        } else if (strcmp(var, "contrast") == 0) {
            if (val >= -2 && val <= 2) {
                s->set_contrast(s, val);
                nvs_key = NVS_KEY_CONTRAST;
                is_signed = true;
                res = 0;
            }
        } else {
            ESP_LOGW(TAG, "Unknown control var: %s", var);
            return ESP_ERR_NOT_FOUND;
        }

        if (res != 0 && nvs_key) {
            ESP_LOGW(TAG, "Sensor returned error for %s=%d (res=%d)", var, val, res);
        }
    }

    if (!nvs_key) {
        ESP_LOGW(TAG, "Value out of range: %s=%d", var, val);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Set %s=%d", var, val);

    // Persist to NVS
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (is_signed) {
            nvs_set_i8(handle, nvs_key, (int8_t)val);
        } else {
            nvs_set_u8(handle, nvs_key, (uint8_t)val);
        }
        nvs_commit(handle);
        nvs_close(handle);
    }

    return ESP_OK;
}

void camera_get_status_json(char *buf, size_t buf_size) {
    // Load mic/audio settings from NVS (always available, even without camera)
    uint8_t mic_en = 0, mic_mute = 0, mic_sens = 70, aac_rate = 16, aac_bitr = 32;
    nvs_handle_t nvs;
    if (nvs_manager_open(NVS_CAMERA_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_MIC_ENABLED, &mic_en);
        nvs_get_u8(nvs, NVS_KEY_MIC_MUTED, &mic_mute);
        nvs_get_u8(nvs, NVS_KEY_MIC_SENS, &mic_sens);
        nvs_get_u8(nvs, NVS_KEY_AAC_RATE, &aac_rate);
        nvs_get_u8(nvs, NVS_KEY_AAC_BITRATE, &aac_bitr);
        nvs_close(nvs);
    }

    if (!camera_ready) {
        snprintf(buf, buf_size,
                 "{\"camera_ready\":false,"
                 "\"mic_enabled\":%s,\"mic_muted\":%s,"
                 "\"mic_sensitivity\":%d,\"aac_sample_rate\":%d,\"aac_bitrate\":%d}",
                 mic_en ? "true" : "false", mic_mute ? "true" : "false",
                 mic_sens, aac_rate, aac_bitr);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        snprintf(buf, buf_size, "{\"camera_ready\":true}");
        return;
    }

    snprintf(buf, buf_size,
             "{\"camera_ready\":true,\"PID\":\"0x%04x\","
             "\"framesize\":%d,\"quality\":%d,"
             "\"brightness\":%d,\"contrast\":%d,"
             "\"mic_enabled\":%s,\"mic_muted\":%s,"
             "\"mic_sensitivity\":%d,\"aac_sample_rate\":%d,\"aac_bitrate\":%d}",
             s->id.PID,
             s->status.framesize,
             s->status.quality,
             s->status.brightness,
             s->status.contrast,
             mic_en ? "true" : "false", mic_mute ? "true" : "false",
             mic_sens, aac_rate, aac_bitr);
}
