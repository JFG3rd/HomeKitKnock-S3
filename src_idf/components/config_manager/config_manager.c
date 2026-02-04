/**
 * Configuration Manager Component Implementation
 */

#include "config_manager.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_mgr";

// NVS namespace for configuration
#define CONFIG_NAMESPACE "config"

// Default configurations
static const sip_config_t DEFAULT_SIP = {
    .server = "192.168.1.1",
    .port = 5060,
    .username = "",
    .password = "",
    .extension = "",
    .enabled = false
};

static const camera_config_t DEFAULT_CAMERA = {
    .width = 640,
    .height = 480,
    .fps = 15,
    .quality = 80,
    .enabled = false
};

static const audio_config_t DEFAULT_AUDIO = {
    .volume = 50,
    .enabled = false
};

static const system_config_t DEFAULT_SYSTEM = {
    .device_name = "ESP32-Doorbell",
    .timezone = 0,
    .uptime_offset = 0
};

esp_err_t config_manager_init(void) {
    ESP_LOGI(TAG, "Initializing configuration manager");

    // Configuration is loaded on-demand, no need to pre-load
    // Just verify we can access the NVS namespace
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet, that's OK
        ESP_LOGI(TAG, "No configuration found, will use defaults");
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to access config namespace: %s", esp_err_to_name(err));
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "✓ Configuration manager ready");
    return ESP_OK;
}

esp_err_t config_get_sip(sip_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Start with defaults
    memcpy(config, &DEFAULT_SIP, sizeof(sip_config_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No SIP config found, using defaults");
        return ESP_OK; // Return defaults
    }

    // Read each field
    size_t len;

    len = sizeof(config->server);
    nvs_get_str(handle, "sip_server", config->server, &len);

    nvs_get_u16(handle, "sip_port", &config->port);

    len = sizeof(config->username);
    nvs_get_str(handle, "sip_user", config->username, &len);

    len = sizeof(config->password);
    nvs_get_str(handle, "sip_pass", config->password, &len);

    len = sizeof(config->extension);
    nvs_get_str(handle, "sip_ext", config->extension, &len);

    uint8_t enabled_u8;
    if (nvs_get_u8(handle, "sip_enabled", &enabled_u8) == ESP_OK) {
        config->enabled = (enabled_u8 != 0);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_set_sip(const sip_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // Write each field
    nvs_set_str(handle, "sip_server", config->server);
    nvs_set_u16(handle, "sip_port", config->port);
    nvs_set_str(handle, "sip_user", config->username);
    nvs_set_str(handle, "sip_pass", config->password);
    nvs_set_str(handle, "sip_ext", config->extension);
    nvs_set_u8(handle, "sip_enabled", config->enabled ? 1 : 0);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ SIP configuration saved");
    }
    return err;
}

esp_err_t config_get_camera(camera_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &DEFAULT_CAMERA, sizeof(camera_config_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_OK; // Return defaults
    }

    nvs_get_u16(handle, "cam_width", &config->width);
    nvs_get_u16(handle, "cam_height", &config->height);
    nvs_get_u8(handle, "cam_fps", &config->fps);
    nvs_get_u8(handle, "cam_quality", &config->quality);

    uint8_t enabled_u8;
    if (nvs_get_u8(handle, "cam_enabled", &enabled_u8) == ESP_OK) {
        config->enabled = (enabled_u8 != 0);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_set_camera(const camera_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_u16(handle, "cam_width", config->width);
    nvs_set_u16(handle, "cam_height", config->height);
    nvs_set_u8(handle, "cam_fps", config->fps);
    nvs_set_u8(handle, "cam_quality", config->quality);
    nvs_set_u8(handle, "cam_enabled", config->enabled ? 1 : 0);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Camera configuration saved");
    }
    return err;
}

esp_err_t config_get_audio(audio_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &DEFAULT_AUDIO, sizeof(audio_config_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_OK; // Return defaults
    }

    nvs_get_u8(handle, "aud_volume", &config->volume);

    uint8_t enabled_u8;
    if (nvs_get_u8(handle, "aud_enabled", &enabled_u8) == ESP_OK) {
        config->enabled = (enabled_u8 != 0);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_set_audio(const audio_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_u8(handle, "aud_volume", config->volume);
    nvs_set_u8(handle, "aud_enabled", config->enabled ? 1 : 0);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Audio configuration saved");
    }
    return err;
}

esp_err_t config_get_system(system_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &DEFAULT_SYSTEM, sizeof(system_config_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_OK; // Return defaults
    }

    size_t len = sizeof(config->device_name);
    nvs_get_str(handle, "sys_name", config->device_name, &len);

    int8_t tz_s8;
    if (nvs_get_i8(handle, "sys_tz", &tz_s8) == ESP_OK) {
        config->timezone = tz_s8;
    }

    nvs_get_u32(handle, "sys_uptime", &config->uptime_offset);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_set_system(const system_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(handle, "sys_name", config->device_name);
    nvs_set_i8(handle, "sys_tz", config->timezone);
    nvs_set_u32(handle, "sys_uptime", config->uptime_offset);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ System configuration saved");
    }
    return err;
}

esp_err_t config_reset_all(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // Erase entire namespace
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ All configuration reset to defaults");
    }
    return err;
}
