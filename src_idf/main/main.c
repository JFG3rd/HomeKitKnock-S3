/**
 * ESP32-S3 Doorbell - Pure ESP-IDF Implementation
 * Phase 5: Complete Audio Path
 *
 * Boot Sequence (CRITICAL ORDER):
 * 1. NVS initialization (MUST be first!)
 * 2. WiFi initialization
 * 3. Network startup (STA or AP mode)
 * 4. Web server startup (deferred to main loop)
 * 5. SIP client startup (deferred to main loop)
 * 6. Camera + MJPEG server startup (deferred to main loop)
 *
 * NOTE: Heavy initialization is deferred to main loop to avoid
 * stack overflow in the system event task (limited stack size).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "nvs.h"

// Component headers
#include "nvs_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "dns_server.h"
#include "log_buffer.h"
#include "sip_client.h"
#include "button.h"
#include "status_led.h"
#include "camera.h"
#include "mjpeg_server.h"
#include "rtsp_server.h"
#include "audio_capture.h"
#include "audio_output.h"
#include "aac_encoder_pipe.h"

static const char *TAG = "main";
static httpd_handle_t http_server = NULL;
static bool sip_initialized = false;
static bool sip_config_valid_flag = false;  // Only true if SIP config loaded successfully
static sip_config_t sip_config;

// Deferred initialization flags (to avoid stack overflow in event callback)
static volatile bool web_server_pending = false;
static volatile bool sip_init_pending = false;
static volatile bool camera_init_pending = false;
static volatile bool dns_server_pending = false;
static volatile bool dns_stop_pending = false;
static volatile bool sntp_init_pending = false;
static bool sntp_initialized = false;
static bool camera_initialized = false;

#define NVS_SYSTEM_NAMESPACE "system"
#define NVS_KEY_TIMEZONE     "timezone"
#define DEFAULT_TIMEZONE     "CET-1CEST,M3.5.0,M10.5.0/3"
#define MAX_TIMEZONE_LEN     64

static void load_timezone(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    strncpy(out, DEFAULT_TIMEZONE, out_size - 1);
    out[out_size - 1] = '\0';

    nvs_handle_t handle;
    if (nvs_manager_open(NVS_SYSTEM_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t len = out_size;
    if (nvs_get_str(handle, NVS_KEY_TIMEZONE, out, &len) != ESP_OK || out[0] == '\0') {
        strncpy(out, DEFAULT_TIMEZONE, out_size - 1);
        out[out_size - 1] = '\0';
    }
    nvs_close(handle);
}

/**
 * SNTP time synchronization callback
 */
static void time_sync_notification_cb(struct timeval *tv) {
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Time synchronized: %s", strftime_buf);
}

/**
 * Initialize SNTP for time synchronization
 */
static void initialize_sntp(void) {
    if (sntp_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Initializing SNTP...");

    char timezone[MAX_TIMEZONE_LEN];
    load_timezone(timezone, sizeof(timezone));
    setenv("TZ", timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", timezone);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    sntp_initialized = true;
    ESP_LOGI(TAG, "SNTP initialized, waiting for time sync...");
}

/**
 * Button press callback - triggers doorbell ring
 */
static void on_button_press(void) {
    ESP_LOGI(TAG, "Doorbell button pressed!");

    // Trigger status LED ring animation
    status_led_mark_ring();

    // Play gong sound on speaker (async, fire-and-forget)
    audio_output_play_gong();

    // Request SIP ring (deferred to main loop)
    if (sip_initialized && sip_is_enabled()) {
        esp_err_t err = sip_request_ring();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SIP ring requested");
        } else {
            ESP_LOGW(TAG, "SIP ring request failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "SIP not available - ring not sent");
    }

    // TODO: Add Scrypted webhook trigger here
}

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
            camera_init_pending = true;
            sntp_init_pending = true;
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
    ESP_LOGI(TAG, "Phase 5: Complete Audio Path");
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
    // Initialize Status LED (early, for visual feedback)
    // =====================================================================
    err = status_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status LED init failed (non-fatal)");
    }

    // =====================================================================
    // Initialize Doorbell Button
    // =====================================================================
    err = button_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Button init failed (non-fatal)");
    } else {
        button_set_callback(on_button_press);
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

        // Process deferred SNTP initialization
        if (sntp_init_pending && !sntp_initialized) {
            sntp_init_pending = false;
            initialize_sntp();
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

        // Process deferred camera + MJPEG server initialization (only if feature enabled)
        if (camera_init_pending && !camera_initialized) {
            camera_init_pending = false;
            if (!camera_is_enabled()) {
                ESP_LOGI(TAG, "HTTP camera streaming disabled - skipping camera init");
            } else {
                ESP_LOGI(TAG, "Initializing camera...");
                esp_err_t cam_err = camera_init();
                if (cam_err == ESP_OK) {
                    camera_initialized = true;
                    ESP_LOGI(TAG, "Camera initialized, starting MJPEG server...");
                    cam_err = mjpeg_server_start();
                    if (cam_err == ESP_OK) {
                        ESP_LOGI(TAG, "MJPEG server started on port 81");
                    } else {
                        ESP_LOGW(TAG, "MJPEG server start failed: %s", esp_err_to_name(cam_err));
                    }
                    // Start RTSP server if enabled
                    if (camera_is_rtsp_enabled()) {
                        cam_err = rtsp_server_start();
                        if (cam_err == ESP_OK) {
                            ESP_LOGI(TAG, "RTSP server started on port 8554");
                        } else {
                            ESP_LOGW(TAG, "RTSP server start failed: %s", esp_err_to_name(cam_err));
                        }
                    } else {
                        ESP_LOGI(TAG, "RTSP streaming disabled - skipping RTSP server");
                    }

                    // Initialize audio capture (mic) if enabled
                    if (audio_capture_is_enabled()) {
                        esp_err_t audio_err = audio_capture_init();
                        if (audio_err == ESP_OK) {
                            audio_err = audio_capture_start();
                            if (audio_err == ESP_OK) {
                                ESP_LOGI(TAG, "Audio capture started (source=%s)",
                                         audio_capture_get_source() == MIC_SOURCE_PDM ? "PDM" : "INMP441");
                                // Start AAC encoder pipeline for RTSP audio
                                audio_err = aac_encoder_pipe_init();
                                if (audio_err == ESP_OK) {
                                    ESP_LOGI(TAG, "AAC encoder pipeline initialized");
                                } else {
                                    ESP_LOGW(TAG, "AAC encoder init failed: %s", esp_err_to_name(audio_err));
                                }
                            } else {
                                ESP_LOGW(TAG, "Audio capture start failed: %s", esp_err_to_name(audio_err));
                            }
                        } else {
                            ESP_LOGW(TAG, "Audio capture init failed: %s", esp_err_to_name(audio_err));
                        }
                    } else {
                        ESP_LOGI(TAG, "Mic disabled - skipping audio capture");
                    }

                    // Initialize audio output (speaker) if available
                    if (audio_output_is_available()) {
                        esp_err_t spk_err = audio_output_init();
                        if (spk_err == ESP_OK) {
                            ESP_LOGI(TAG, "Audio output (speaker) initialized");
                        } else if (spk_err == ESP_ERR_NOT_SUPPORTED) {
                            ESP_LOGI(TAG, "Speaker unavailable (INMP441 mode)");
                        } else {
                            ESP_LOGW(TAG, "Audio output init failed: %s", esp_err_to_name(spk_err));
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "Camera init failed: %s (streaming disabled)", esp_err_to_name(cam_err));
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

        // Poll doorbell button (handles debouncing)
        button_poll();

        // Update status LED state based on system state
        bool is_ap_mode = !wifi_manager_has_credentials() || !wifi_manager_is_connected();
        bool is_connecting = wifi_manager_has_credentials() && !wifi_manager_is_connected();
        bool sip_ok = sip_initialized && sip_is_registered();
        bool sip_error = sip_initialized && sip_config_valid_flag && !sip_ok;

        status_led_set_state(LED_STATE_AP_MODE, is_ap_mode && !is_connecting);
        status_led_set_state(LED_STATE_WIFI_CONNECTING, is_connecting);
        status_led_set_state(LED_STATE_SIP_OK, sip_ok);
        status_led_set_state(LED_STATE_SIP_ERROR, sip_error);
        status_led_set_state(LED_STATE_RTSP_ACTIVE,
            mjpeg_server_client_count() > 0 || rtsp_server_active_session_count() > 0);

        // Update LED pattern
        status_led_update();

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
