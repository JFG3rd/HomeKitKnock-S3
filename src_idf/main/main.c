/**
 * ESP32-S3 Doorbell - Pure ESP-IDF Implementation
 * Phase 1: Base System with Component Architecture
 *
 * This application demonstrates proper ESP-IDF component architecture:
 * - NVS Manager: Robust non-volatile storage with error recovery
 * - WiFi Manager: STA/AP modes with credential storage
 * - Web Server: HTTP server with embedded assets and API endpoints
 *
 * Boot Sequence (CRITICAL ORDER):
 * 1. NVS initialization (MUST be first!)
 * 2. WiFi initialization
 * 3. Network startup (STA or AP mode)
 * 4. Web server startup
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

static const char *TAG = "main";
static httpd_handle_t http_server = NULL;

/**
 * WiFi event callback
 * Handles WiFi state changes and starts/stops web server as needed
 */
static void wifi_event_callback(wifi_mgr_event_t event, void *data) {
    switch (event) {
        case WIFI_MGR_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "✓ WiFi connected, starting web server");
            // Stop DNS server (no longer needed when connected to real network)
            dns_server_stop();
            if (!http_server) {
                http_server = web_server_start();
            }
            break;

        case WIFI_MGR_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;

        case WIFI_MGR_EVENT_AP_STARTED:
            ESP_LOGI(TAG, "✓ AP mode active, starting web server and DNS server");
            // Start DNS server for captive portal
            dns_server_start();
            if (!http_server) {
                http_server = web_server_start();
            }
            break;

        case WIFI_MGR_EVENT_AP_STOPPED:
            ESP_LOGI(TAG, "AP mode stopped");
            dns_server_stop();
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
    ESP_LOGI(TAG, "Phase 1: Component Architecture");
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "====================================");

    // =====================================================================
    // STEP 1: Initialize NVS (MUST BE FIRST!)
    // =====================================================================
    ESP_LOGI(TAG, "[1/5] Initializing NVS...");
    esp_err_t err = nvs_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed! Cannot continue.");
        ESP_LOGE(TAG, "Error: %s (0x%x)", esp_err_to_name(err), err);
        return;
    }

    // =====================================================================
    // STEP 2: Initialize Log Buffer (capture all subsequent logs)
    // =====================================================================
    ESP_LOGI(TAG, "[2/5] Initializing Log Buffer...");
    err = log_buffer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Log buffer initialization failed (non-fatal): %s", esp_err_to_name(err));
    }

    // =====================================================================
    // STEP 3: Initialize WiFi Manager
    // =====================================================================
    ESP_LOGI(TAG, "[3/5] Initializing WiFi Manager...");
    err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi manager initialization failed!");
        ESP_LOGE(TAG, "Error: %s (0x%x)", esp_err_to_name(err), err);
        return;
    }

    // Register WiFi event callback
    wifi_manager_set_event_callback(wifi_event_callback);

    // =====================================================================
    // STEP 4: Start WiFi (STA or AP mode)
    // =====================================================================
    ESP_LOGI(TAG, "[4/5] Starting WiFi...");

    // Try Station mode first (if credentials exist)
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
    // STEP 5: Web server will start automatically via WiFi event callback
    // =====================================================================
    ESP_LOGI(TAG, "[5/5] Waiting for network...");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "System Initialization Complete");
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "");

    // Print instructions
    if (!wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "🔧 SETUP REQUIRED:");
        ESP_LOGI(TAG, "1. Connect to WiFi: 'doorbell-setup'");
        ESP_LOGI(TAG, "2. Password: 'doorbell123'");
        ESP_LOGI(TAG, "3. Captive portal should open automatically");
        ESP_LOGI(TAG, "   Or open browser: http://192.168.4.1");
        ESP_LOGI(TAG, "4. Configure your WiFi credentials");
    } else {
        ESP_LOGI(TAG, "📡 Connecting to saved network...");
        ESP_LOGI(TAG, "Web interface will be available after connection");
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Main initialization complete");
    ESP_LOGI(TAG, "System running - FreeRTOS tasks active");

    // Main loop (optional - FreeRTOS tasks handle everything)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // Sleep for 10 seconds

        // Periodic status log
        if (wifi_manager_is_connected()) {
            char ip[16];
            wifi_manager_get_ip(ip, sizeof(ip));
            ESP_LOGI(TAG, "Status: Connected (IP: %s)", ip);
        } else {
            ESP_LOGI(TAG, "Status: %s",
                     wifi_manager_has_credentials() ? "Connecting..." : "AP Mode");
        }
    }
}
