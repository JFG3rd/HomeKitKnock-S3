/**
 * WiFi Manager Component Implementation
 */

#include "wifi_manager.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

// State tracking
static bool initialized = false;
static bool sta_connected = false;
static bool sta_got_ip = false;
static bool scan_in_progress = false;
static wifi_mgr_event_cb_t event_callback = NULL;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

// Scan results cache
#define MAX_SCAN_RESULTS 20
static wifi_ap_record_t cached_scan_results[MAX_SCAN_RESULTS];
static uint16_t cached_scan_count = 0;

// Default AP configuration
#define DEFAULT_AP_SSID     "doorbell-setup"
#define DEFAULT_AP_PASSWORD "doorbell123"
#define DEFAULT_AP_CHANNEL  6
#define DEFAULT_AP_MAX_CONN 4

/**
 * WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Station started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "✓ Connected to AP");
                sta_connected = true;
                if (event_callback) {
                    event_callback(WIFI_MGR_EVENT_STA_CONNECTED, event_data);
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn = event_data;
                ESP_LOGW(TAG, "Disconnected from AP (reason: %d)", disconn->reason);
                sta_connected = false;
                sta_got_ip = false;

                if (event_callback) {
                    event_callback(WIFI_MGR_EVENT_STA_DISCONNECTED, event_data);
                }

                // Attempt reconnection
                ESP_LOGI(TAG, "Attempting reconnection...");
                esp_wifi_connect();
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "✓ AP mode started");
                if (event_callback) {
                    event_callback(WIFI_MGR_EVENT_AP_STARTED, NULL);
                }
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP mode stopped");
                if (event_callback) {
                    event_callback(WIFI_MGR_EVENT_AP_STOPPED, NULL);
                }
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Client connected to AP");
                break;

            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Client disconnected from AP");
                break;

            case WIFI_EVENT_SCAN_DONE: {
                ESP_LOGI(TAG, "WiFi scan completed");
                scan_in_progress = false;

                // Cache the scan results immediately
                uint16_t num_aps = MAX_SCAN_RESULTS;
                esp_err_t err = esp_wifi_scan_get_ap_records(&num_aps, cached_scan_results);
                if (err == ESP_OK) {
                    cached_scan_count = num_aps;
                    ESP_LOGI(TAG, "Cached %d scan results", cached_scan_count);
                } else {
                    cached_scan_count = 0;
                    ESP_LOGW(TAG, "Failed to cache scan results: %s", esp_err_to_name(err));
                }
                break;
            }

            default:
                break;
        }
    }
}

/**
 * IP event handler
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = event_data;
                ESP_LOGI(TAG, "✓ Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
                sta_got_ip = true;

                if (event_callback) {
                    event_callback(WIFI_MGR_EVENT_STA_GOT_IP, event_data);
                }
                break;
            }

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "Lost IP address");
                sta_got_ip = false;
                break;

            default:
                break;
        }
    }
}

esp_err_t wifi_manager_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi manager");

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop if not exists
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
        return err;
    }

    // Create network interfaces
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                &ip_event_handler, NULL));

    // Set WiFi storage to flash (persist mode across reboots)
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    initialized = true;
    ESP_LOGI(TAG, "✓ WiFi manager ready");

    return ESP_OK;
}

esp_err_t wifi_manager_start_sta(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    // Load credentials from NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open("wifi", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No WiFi credentials found");
        return ESP_ERR_NOT_FOUND;
    }

    char ssid[32] = {0};
    char password[64] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Failed to read SSID: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_str(handle, "password", password, &pass_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Failed to read password: %s", esp_err_to_name(err));
        return err;
    }

    nvs_close(handle);

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    // Configure AP
    wifi_config_t ap_config = {
        .ap = {
            .ssid = DEFAULT_AP_SSID,
            .ssid_len = strlen(DEFAULT_AP_SSID),
            .channel = DEFAULT_AP_CHANNEL,
            .password = DEFAULT_AP_PASSWORD,
            .max_connection = DEFAULT_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    // Use open auth if no password
    if (strlen(DEFAULT_AP_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Use APSTA mode to allow WiFi scanning while in AP mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started (APSTA mode for scanning) - SSID: %s, Channel: %d",
             DEFAULT_AP_SSID, DEFAULT_AP_CHANNEL);

    // Start initial WiFi scan so results are available when user visits setup page
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } }
    };
    scan_in_progress = true;
    esp_err_t scan_err = esp_wifi_scan_start(&scan_config, false);
    if (scan_err != ESP_OK) {
        scan_in_progress = false;
        ESP_LOGW(TAG, "Failed to start initial scan: %s", esp_err_to_name(scan_err));
    } else {
        ESP_LOGI(TAG, "Started initial WiFi scan");
    }

    return ESP_OK;
}

esp_err_t wifi_manager_stop(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping WiFi");
    sta_connected = false;
    sta_got_ip = false;

    return esp_wifi_stop();
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(ssid) > 31 || strlen(password) > 63) {
        ESP_LOGE(TAG, "SSID or password too long");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "ssid", ssid);
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, "password", password);
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ WiFi credentials saved");
    } else {
        ESP_LOGE(TAG, "Failed to commit credentials: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t wifi_manager_clear_credentials(void) {
    esp_err_t err;

    // Clear credentials from our NVS namespace
    nvs_handle_t handle;
    err = nvs_manager_open("wifi", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, "ssid");
        nvs_erase_key(handle, "password");
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Cleared credentials from NVS 'wifi' namespace");
    }

    // Also clear the ESP-IDF WiFi stack's internal config
    wifi_config_t wifi_config = {0};
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Cleared WiFi stack STA config");
    } else {
        ESP_LOGW(TAG, "Failed to clear WiFi stack config: %s", esp_err_to_name(err));
    }

    // Erase WiFi config from ESP-IDF's internal NVS (nvs.net80211)
    nvs_handle_t nvs_net_handle;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &nvs_net_handle) == ESP_OK) {
        nvs_erase_all(nvs_net_handle);
        nvs_commit(nvs_net_handle);
        nvs_close(nvs_net_handle);
        ESP_LOGI(TAG, "Cleared ESP-IDF WiFi NVS (nvs.net80211)");
    }

    ESP_LOGI(TAG, "✓ All WiFi credentials cleared");
    return ESP_OK;
}

bool wifi_manager_has_credentials(void) {
    nvs_handle_t handle;
    if (nvs_manager_open("wifi", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    char ssid[32];
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &len);
    nvs_close(handle);

    return (err == ESP_OK && len > 1);
}

void wifi_manager_set_event_callback(wifi_mgr_event_cb_t callback) {
    event_callback = callback;
}

bool wifi_manager_is_connected(void) {
    return sta_connected && sta_got_ip;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t len) {
    if (!ip_str || len < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!sta_got_ip || !sta_netif) {
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t wifi_manager_start_scan(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (scan_in_progress) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            }
        }
    };

    scan_in_progress = true;
    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        scan_in_progress = false;
        ESP_LOGE(TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi scan started");
    return ESP_OK;
}

bool wifi_manager_is_scan_in_progress(void) {
    return scan_in_progress;
}

esp_err_t wifi_manager_get_scan_results(wifi_ap_record_t *ap_records,
                                         uint16_t max_records,
                                         uint16_t *num_found) {
    if (!ap_records || !num_found) {
        return ESP_ERR_INVALID_ARG;
    }

    if (scan_in_progress) {
        *num_found = 0;
        return ESP_OK;  // Return empty results while scanning
    }

    // Return cached results
    uint16_t count = (cached_scan_count < max_records) ? cached_scan_count : max_records;
    memcpy(ap_records, cached_scan_results, count * sizeof(wifi_ap_record_t));
    *num_found = count;

    ESP_LOGI(TAG, "Returned %d cached scan results", *num_found);
    return ESP_OK;
}
