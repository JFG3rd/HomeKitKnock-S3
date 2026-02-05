/**
 * ESP32-S3 Doorbell - Pure ESP-IDF Implementation
 * Phase 3: SIP Client Integration
 *
 * Boot Sequence (CRITICAL ORDER):
 * 1. NVS initialization (MUST be first!)
 * 2. WiFi initialization
 * 3. Network startup (STA or AP mode)
 * 4. Web server startup (deferred to main loop)
 * 5. SIP client startup (deferred to main loop)
 *
 * NOTE: Heavy initialization is deferred to main loop to avoid
 * stack overflow in the system event task (limited stack size).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

// Component headers
#include "nvs_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "dns_server.h"
#include "log_buffer.h"
#include "sip_client.h"

static const char *TAG = "main";
static httpd_handle_t http_server = NULL;
static bool sip_initialized = false;
static bool sip_config_valid_flag = false;  // Only true if SIP config loaded successfully
static sip_config_t sip_config;

// Deferred initialization flags (to avoid stack overflow in event callback)
static volatile bool web_server_pending = false;
static volatile bool sip_init_pending = false;
static volatile bool dns_server_pending = false;
static volatile bool dns_stop_pending = false;

/**
 * WiFi event callback
 * NOTE: Keep this VERY lightweight - just set flags for main loop
 */
static void wifi_event_callback(wifi_mgr_event_t event, void *data) {
    switch (event) {
        case WIFI_MGR_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "WiFi got IP - queueing web server start");
            dns_stop_pending = true;
            web_server_pending = true;
            sip_init_pending = true;
            break;

        case WIFI_MGR_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;

        case WIFI_MGR_EVENT_AP_STARTED:
            ESP_LOGI(TAG, "AP mode active - queueing server start");
            dns_server_pending = true;
            web_server_pending = true;
            break;

        case WIFI_MGR_EVENT_AP_STOPPED:
            ESP_LOGI(TAG, "AP mode stopped");
            dns_stop_pending = true;
            break;

        default:
            break;
    }
}

/**
 * Application entry point
 */
void app_main(void) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "ESP32-S3 Doorbell - ESP-IDF v2.0");
    ESP_LOGI(TAG, "Phase 3: SIP Client Integration");
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "====================================");

    // =====================================================================
    // STEP 1: Initialize NVS (MUST BE FIRST!)
    // =====================================================================
    ESP_LOGI(TAG, "[1/5] Initializing NVS...");
    esp_err_t err = nvs_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed! Cannot continue.");
        return;
    }

    // =====================================================================
    // STEP 2: Initialize Log Buffer
    // =====================================================================
    ESP_LOGI(TAG, "[2/5] Initializing Log Buffer...");
    err = log_buffer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Log buffer init failed (non-fatal)");
    }

    // =====================================================================
    // STEP 3: Initialize WiFi Manager
    // =====================================================================
    ESP_LOGI(TAG, "[3/5] Initializing WiFi Manager...");
    err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi manager initialization failed!");
        return;
    }

    // Register WiFi event callback
    wifi_manager_set_event_callback(wifi_event_callback);

    // =====================================================================
    // STEP 4: Start WiFi (STA or AP mode)
    // =====================================================================
    ESP_LOGI(TAG, "[4/5] Starting WiFi...");

    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Found saved credentials, starting in Station mode");
        err = wifi_manager_start_sta();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Station mode failed, falling back to AP mode");
            wifi_manager_start_ap();
        }
    } else {
        ESP_LOGI(TAG, "No credentials found, starting in AP mode");
        wifi_manager_start_ap();
    }

    // =====================================================================
    // STEP 5: Services start via main loop (deferred from event callback)
    // =====================================================================
    ESP_LOGI(TAG, "[5/5] Waiting for network...");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "System Initialization Complete");
    ESP_LOGI(TAG, "====================================");

    if (!wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "SETUP: Connect to 'doorbell-setup' (pw: doorbell123)");
    } else {
        ESP_LOGI(TAG, "Connecting to saved network...");
    }

    ESP_LOGI(TAG, "Main loop starting");

    // Main loop - handles deferred initialization and SIP processing
    uint32_t status_log_counter = 0;
    while (1) {
        // Process deferred DNS server stop
        if (dns_stop_pending) {
            dns_stop_pending = false;
            dns_server_stop();
        }

        // Process deferred DNS server start
        if (dns_server_pending) {
            dns_server_pending = false;
            ESP_LOGI(TAG, "Starting DNS server...");
            dns_server_start();
        }

        // Process deferred web server start
        if (web_server_pending && !http_server) {
            web_server_pending = false;
            ESP_LOGI(TAG, "Starting web server...");
            http_server = web_server_start();
            if (http_server) {
                ESP_LOGI(TAG, "Web server started");
            }
        }

        // Process deferred SIP client initialization (only if SIP feature is enabled)
        if (sip_init_pending && !sip_initialized) {
            sip_init_pending = false;
            if (!sip_is_enabled()) {
                ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "SIP feature disabled - skipping init");
            } else {
                ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "Initializing SIP client...");
                esp_err_t sip_err = sip_client_init();
                if (sip_err == ESP_OK) {
                    sip_initialized = true;
                    ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "SIP client initialized");
                    if (sip_config_load(&sip_config) && sip_config_valid(&sip_config)) {
                        sip_config_valid_flag = true;
                        ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "SIP config loaded - use web interface to test");
                        // Skip automatic registration on boot to avoid lwIP/FreeRTOS issues
                        // User can trigger registration via /ring/sip endpoint
                    } else {
                        sip_config_valid_flag = false;
                        ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "No SIP config - configure via web interface");
                    }
                } else {
                    ESP_LOG_LEVEL(ESP_LOG_WARN, "sip", "SIP client init failed: %s", esp_err_to_name(sip_err));
                }
            }
        }

        // SIP processing (runs frequently for active calls, only if SIP enabled)
        if (sip_initialized && sip_is_enabled() && wifi_manager_is_connected()) {
            // Note: sip_handle_incoming uses MSG_DONTWAIT which should be safe
            sip_handle_incoming();

            // Check for deferred ring requests (from HTTP handlers)
            if (sip_config_valid_flag) {
                sip_check_pending_ring(&sip_config);
            }

            if (sip_ring_active()) {
                sip_ring_process();
                sip_media_process();
            }

            // Periodic registration (main loop has full stack, so this is safe)
            if (sip_config_valid_flag) {
                sip_register_if_needed(&sip_config);
            }
        }

        // Short delay for responsiveness (50ms)
        vTaskDelay(pdMS_TO_TICKS(50));

        // Periodic status log (every ~10 seconds)
        status_log_counter++;
        if (status_log_counter >= 200) {
            status_log_counter = 0;
            if (wifi_manager_is_connected()) {
                char ip[16];
                wifi_manager_get_ip(ip, sizeof(ip));
                // Use "sip" tag for SIP status so it shows in doorbell filter
                if (!sip_is_enabled()) {
                    ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "Status: IP=%s SIP=disabled", ip);
                } else {
                    bool sip_reg = sip_initialized && sip_is_registered();
                    ESP_LOG_LEVEL(ESP_LOG_INFO, "sip", "Status: IP=%s registered=%s",
                                  ip, sip_reg ? "yes" : "no");
                }
            } else {
                ESP_LOGI(TAG, "Status: %s",
                         wifi_manager_has_credentials() ? "Connecting..." : "AP Mode");
            }
        }
    }
}
