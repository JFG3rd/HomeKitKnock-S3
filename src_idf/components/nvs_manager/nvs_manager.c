/**
 * NVS Manager Component Implementation
 */

#include "nvs_manager.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs_mgr";
static bool initialized = false;

esp_err_t nvs_manager_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing NVS Flash");
    esp_err_t err = nvs_flash_init();

    // Handle known recovery scenarios
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        err == ESP_ERR_NVS_INVALID_STATE) {

        ESP_LOGW(TAG, "NVS needs recovery: %s (0x%x)",
                 esp_err_to_name(err), err);

        ESP_LOGW(TAG, "Erasing NVS partition...");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Re-initializing NVS after erase...");
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s (0x%x)",
                 esp_err_to_name(err), err);
        return err;
    }

    ESP_LOGI(TAG, "✓ NVS initialized successfully");
    initialized = true;

    return ESP_OK;
}

esp_err_t nvs_manager_open(const char *namespace,
                            nvs_open_mode_t mode,
                            nvs_handle_t *handle) {
    if (!initialized) {
        ESP_LOGE(TAG, "Not initialized! Call nvs_manager_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!namespace || !handle) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(namespace) > 15) {
        ESP_LOGE(TAG, "Namespace '%s' too long (max 15 chars)", namespace);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_open(namespace, mode, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open namespace '%s': %s",
                 namespace, esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Opened namespace '%s' (mode=%s)",
                 namespace, mode == NVS_READONLY ? "RO" : "RW");
    }

    return err;
}

esp_err_t nvs_manager_factory_reset(void) {
    ESP_LOGW(TAG, "!!! FACTORY RESET - Erasing all NVS data !!!");

    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
        return err;
    }

    initialized = false;
    ESP_LOGI(TAG, "Factory reset complete, re-initializing...");

    return nvs_manager_init();
}

bool nvs_manager_is_initialized(void) {
    return initialized;
}
