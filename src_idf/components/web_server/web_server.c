/**
 * Web Server Component Implementation
 */

#include "web_server.h"
#include "wifi_manager.h"
#include "log_buffer.h"
#include "sip_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "embedded_web_assets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "web_server";

/**
 * Find embedded asset by URI
 */
static const struct EmbeddedFile *find_asset(const char *uri) {
    // Normalize path: "/" becomes "index.html"
    const char *filename = uri && uri[0] ? uri + (uri[0] == '/' ? 1 : 0) : "index.html";
    if (filename[0] == '\0') {
        filename = "index.html";
    }

    // Strip query string if present (e.g., "logs.html?filter=core" -> "logs.html")
    static char filename_buf[64];
    const char *query = strchr(filename, '?');
    if (query) {
        size_t len = query - filename;
        if (len >= sizeof(filename_buf)) {
            len = sizeof(filename_buf) - 1;
        }
        strncpy(filename_buf, filename, len);
        filename_buf[len] = '\0';
        filename = filename_buf;
    }

    ESP_LOGD(TAG, "Looking for asset: %s", filename);

    // First try exact match
    for (size_t i = 0; i < embedded_files_count; i++) {
        if (strcmp(filename, embedded_files[i].filename) == 0) {
            ESP_LOGD(TAG, "Found: %s (%zu bytes)", filename, embedded_files[i].size);
            return &embedded_files[i];
        }
    }

    // If no extension, try adding .html (e.g., "/sip" -> "sip.html")
    size_t fname_len = strlen(filename);
    if (!strchr(filename, '.') && fname_len < sizeof(filename_buf) - 6) {
        strcpy(filename_buf, filename);
        strcat(filename_buf, ".html");
        ESP_LOGD(TAG, "Trying with .html: %s", filename_buf);
        for (size_t i = 0; i < embedded_files_count; i++) {
            if (strcmp(filename_buf, embedded_files[i].filename) == 0) {
                ESP_LOGD(TAG, "Found: %s (%zu bytes)", filename_buf, embedded_files[i].size);
                return &embedded_files[i];
            }
        }
    }

    ESP_LOGW(TAG, "Asset not found: %s", filename);
    return NULL;
}

/**
 * Handler for root path - redirects based on WiFi state
 */
static esp_err_t root_handler(httpd_req_t *req) {
    // Check if we have credentials AND are connected
    bool has_creds = wifi_manager_has_credentials();
    bool is_connected = wifi_manager_is_connected();

    ESP_LOGI(TAG, "Root request: has_credentials=%d, is_connected=%d", has_creds, is_connected);

    if (has_creds && is_connected) {
        // Connected to WiFi with valid credentials - serve index.html
        ESP_LOGI(TAG, "Root request: serving index.html (WiFi connected)");
        const struct EmbeddedFile *file = find_asset("index.html");
        if (file) {
            httpd_resp_set_type(req, file->mime_type);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            return httpd_resp_send(req, (const char *)file->data, file->size);
        }
    } else {
        // No credentials or not connected (AP mode) - redirect to wifi-setup
        ESP_LOGI(TAG, "Root request: redirecting to wifi-setup.html (setup mode)");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi-setup.html");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_OK;
}

/**
 * Handler for embedded web assets
 */
static esp_err_t asset_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Asset request: %s", req->uri);

    const struct EmbeddedFile *file = find_asset(req->uri);

    if (!file) {
        ESP_LOGW(TAG, "404: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Serving: %s (%zu bytes)", file->filename, file->size);

    // Set content type and encoding
    httpd_resp_set_type(req, file->mime_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");

    // Send the file
    esp_err_t err = httpd_resp_send(req, (const char *)file->data, file->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
    }

    return err;
}

/**
 * Common WiFi credential save logic
 * Used by both /api/wifi and /saveWiFi endpoints
 */
static esp_err_t save_wifi_credentials(httpd_req_t *req) {
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI(TAG, "Received WiFi config: %s", content);

    // Simple JSON parsing (for production, use cJSON)
    char ssid[32] = {0};
    char password[64] = {0};

    // Find ssid field
    char *ssid_start = strstr(content, "\"ssid\"");
    if (ssid_start) {
        ssid_start = strchr(ssid_start, ':');
        if (ssid_start) {
            ssid_start = strchr(ssid_start, '"');
            if (ssid_start) {
                ssid_start++;
                char *ssid_end = strchr(ssid_start, '"');
                if (ssid_end) {
                    size_t len = ssid_end - ssid_start;
                    if (len < sizeof(ssid)) {
                        strncpy(ssid, ssid_start, len);
                    }
                }
            }
        }
    }

    // Find password field
    char *pass_start = strstr(content, "\"password\"");
    if (pass_start) {
        pass_start = strchr(pass_start, ':');
        if (pass_start) {
            pass_start = strchr(pass_start, '"');
            if (pass_start) {
                pass_start++;
                char *pass_end = strchr(pass_start, '"');
                if (pass_end) {
                    size_t len = pass_end - pass_start;
                    if (len < sizeof(password)) {
                        strncpy(password, pass_start, len);
                    }
                }
            }
        }
    }

    if (strlen(ssid) == 0) {
        const char *resp = "⚠️ Missing SSID";
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    // Save credentials
    esp_err_t err = wifi_manager_save_credentials(ssid, password);
    if (err != ESP_OK) {
        const char *resp = "❌ Failed to save credentials";
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    // Send success response (plain text for /saveWiFi)
    const char *resp = "✅ WiFi credentials saved! Restarting...";
    httpd_resp_send(req, resp, strlen(resp));

    ESP_LOGI(TAG, "WiFi credentials saved: SSID=%s, restarting in 2 seconds...", ssid);

    // Restart device to apply new credentials
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/**
 * Handler for WiFi configuration API (RESTful JSON endpoint)
 * POST /api/wifi
 * Body: {"ssid": "MyWiFi", "password": "mypassword"}
 */
static esp_err_t api_wifi_handler(httpd_req_t *req) {
    return save_wifi_credentials(req);
}

/**
 * Handler for clearing WiFi credentials (RESTful JSON endpoint)
 * DELETE /api/wifi
 * Clears stored WiFi credentials and returns to AP mode
 */
static esp_err_t api_wifi_delete_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Clearing WiFi credentials");

    esp_err_t err = wifi_manager_clear_credentials();

    if (err == ESP_OK) {
        const char *resp = "{\"success\":true,\"message\":\"WiFi credentials cleared\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        ESP_LOGI(TAG, "✓ WiFi credentials cleared");
    } else {
        const char *resp = "{\"success\":false,\"message\":\"Failed to clear credentials\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, resp);
        ESP_LOGE(TAG, "Failed to clear WiFi credentials: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

/**
 * Handler for WiFi save (legacy Arduino endpoint)
 * POST /saveWiFi
 * Body: {"ssid": "MyWiFi", "password": "mypassword"}
 */
static esp_err_t save_wifi_handler(httpd_req_t *req) {
    return save_wifi_credentials(req);
}

/**
 * Handler for WiFi scan request (legacy Arduino endpoint)
 * GET /scanWifi
 */
static esp_err_t scan_wifi_handler(httpd_req_t *req) {
    esp_err_t err = wifi_manager_start_scan();

    if (err == ESP_OK) {
        const char *resp = "OK";
        httpd_resp_send(req, resp, strlen(resp));
        ESP_LOGI(TAG, "WiFi scan initiated");
    } else {
        const char *resp = "ERROR";
        httpd_resp_send_500(req);
        httpd_resp_send(req, resp, strlen(resp));
        ESP_LOGE(TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

/**
 * Handler for WiFi scan results (legacy Arduino endpoint)
 * GET /wifiScanResults
 * Returns: {"ssids": ["Network1", "Network2"], "inProgress": false}
 * Deduplicates SSIDs, keeping the one with strongest signal
 */
static esp_err_t wifi_scan_results_handler(httpd_req_t *req) {
    bool in_progress = wifi_manager_is_scan_in_progress();

    if (in_progress) {
        // Scan still in progress
        const char *resp = "{\"ssids\":[],\"inProgress\":true}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    // Get scan results
    #define MAX_APS 20
    wifi_ap_record_t ap_records[MAX_APS];
    uint16_t num_found = 0;

    esp_err_t err = wifi_manager_get_scan_results(ap_records, MAX_APS, &num_found);

    if (err != ESP_OK) {
        const char *resp = "{\"ssids\":[],\"inProgress\":false}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    // Deduplicate SSIDs - keep the one with strongest signal (highest RSSI)
    // Create a list of unique SSIDs
    char unique_ssids[MAX_APS][33];  // 32 chars + null terminator
    int8_t unique_rssi[MAX_APS];
    int unique_count = 0;

    for (int i = 0; i < num_found; i++) {
        const char *ssid = (const char *)ap_records[i].ssid;
        int8_t rssi = ap_records[i].rssi;

        // Skip empty SSIDs
        if (strlen(ssid) == 0) continue;

        // Check if we already have this SSID
        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (strcmp(unique_ssids[j], ssid) == 0) {
                // Update if this one has stronger signal
                if (rssi > unique_rssi[j]) {
                    unique_rssi[j] = rssi;
                }
                found = true;
                break;
            }
        }

        // Add new unique SSID
        if (!found && unique_count < MAX_APS) {
            strncpy(unique_ssids[unique_count], ssid, 32);
            unique_ssids[unique_count][32] = '\0';
            unique_rssi[unique_count] = rssi;
            unique_count++;
        }
    }

    // Build JSON response with unique SSIDs
    char response[2048];
    int offset = snprintf(response, sizeof(response), "{\"ssids\":[");

    for (int i = 0; i < unique_count && offset < (int)sizeof(response) - 100; i++) {
        if (i > 0) {
            offset += snprintf(response + offset, sizeof(response) - offset, ",");
        }
        offset += snprintf(response + offset, sizeof(response) - offset,
                          "\"%s\"", unique_ssids[i]);
    }

    offset += snprintf(response + offset, sizeof(response) - offset,
                      "],\"inProgress\":false}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    ESP_LOGI(TAG, "Returned %d unique WiFi networks (from %d APs)", unique_count, num_found);
    return ESP_OK;
}

/**
 * Handler for status API
 * GET /api/status
 */
static esp_err_t api_status_handler(httpd_req_t *req) {
    char response[768];
    char ip[16] = "Not connected";
    char gateway[16] = "";

    if (wifi_manager_is_connected()) {
        wifi_manager_get_ip(ip, sizeof(ip));
        // Get gateway IP
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&ip_info.gw));
            }
        }
    }

    // Get system info
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t uptime_sec = esp_log_timestamp() / 1000;

    // Get chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // Get PSRAM info
    uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // Get RSSI (signal strength) if connected
    int8_t rssi = 0;
    if (wifi_manager_is_connected()) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    snprintf(response, sizeof(response),
             "{"
             "\"connected\":%s,"
             "\"ip\":\"%s\","
             "\"gateway\":\"%s\","
             "\"has_credentials\":%s,"
             "\"uptime\":%lu,"
             "\"free_heap\":%lu,"
             "\"min_free_heap\":%lu,"
             "\"psram_total\":%lu,"
             "\"psram_free\":%lu,"
             "\"rssi\":%d,"
             "\"chip_model\":\"%s\","
             "\"chip_cores\":%d,"
             "\"chip_revision\":%d"
             "}",
             wifi_manager_is_connected() ? "true" : "false",
             ip,
             gateway,
             wifi_manager_has_credentials() ? "true" : "false",
             uptime_sec,
             free_heap,
             min_free_heap,
             psram_total,
             psram_free,
             rssi,
             "ESP32-S3",
             chip_info.cores,
             chip_info.revision);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    return ESP_OK;
}

/**
 * Handler for logs API
 * GET /api/logs?filter=all|core|camera|doorbell
 * Returns JSON array of log entries
 */
static esp_err_t api_logs_handler(httpd_req_t *req) {
    // Parse filter query parameter
    log_filter_t filter = LOG_FILTER_ALL;
    char query[64] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "filter", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "core") == 0) {
                filter = LOG_FILTER_CORE;
            } else if (strcmp(param, "camera") == 0) {
                filter = LOG_FILTER_CAMERA;
            } else if (strcmp(param, "doorbell") == 0) {
                filter = LOG_FILTER_DOORBELL;
            }
        }
    }

    // Allocate response buffer (logs can be large)
    char *response = malloc(32768);  // 32KB buffer
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // Get logs as JSON
    log_buffer_get_json(response, 32768, filter, 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    free(response);
    return ESP_OK;
}

/**
 * Handler for clearing logs
 * DELETE /api/logs
 */
static esp_err_t api_logs_clear_handler(httpd_req_t *req) {
    log_buffer_clear();

    const char *resp = "{\"success\":true,\"message\":\"Logs cleared\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    ESP_LOGI(TAG, "Logs cleared via web interface");
    return ESP_OK;
}

/**
 * SIP API Handlers
 */

// GET /api/sip - Get SIP configuration (without password)
static esp_err_t api_sip_get_handler(httpd_req_t *req) {
    sip_config_t config;
    sip_status_t status;

    sip_config_load(&config);
    sip_get_status(&status);

    char response[512];
    snprintf(response, sizeof(response),
        "{\"user\":\"%s\",\"displayname\":\"%s\",\"target\":\"%s\","
        "\"registered\":%s,\"last_status\":%d}",
        config.sip_user,
        config.sip_displayname,
        config.sip_target,
        status.registered ? "true" : "false",
        status.last_status_code);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

// Helper to extract JSON string value
static bool extract_json_string(const char *json, const char *key, char *out, size_t out_size) {
    char search_key[64];
    char *start, *end;
    size_t len;

    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    start = strstr(json, search_key);
    if (!start) return false;

    start = strchr(start, ':');
    if (!start) return false;

    start = strchr(start, '"');
    if (!start) return false;

    start++;
    end = strchr(start, '"');
    if (!end) return false;

    len = end - start;
    if (len >= out_size) len = out_size - 1;
    strncpy(out, start, len);
    out[len] = '\0';
    return true;
}

// Helper to extract JSON boolean value
static bool extract_json_bool(const char *json, const char *key, bool *out) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *start = strstr(json, search_key);
    if (!start) return false;

    start = strchr(start, ':');
    if (!start) return false;

    // Skip whitespace
    start++;
    while (*start == ' ' || *start == '\t') start++;

    if (strncmp(start, "true", 4) == 0) {
        *out = true;
        return true;
    } else if (strncmp(start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

// GET /api/features - Get feature toggle states
static esp_err_t api_features_get_handler(httpd_req_t *req) {
    char response[256];

    bool sip_enabled = sip_is_enabled();

    snprintf(response, sizeof(response),
             "{\"sip_enabled\":%s,\"tr064_enabled\":false,\"http_cam_enabled\":false,\"rtsp_enabled\":false}",
             sip_enabled ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

// POST /saveFeatures - Save feature toggle states
static esp_err_t save_features_handler(httpd_req_t *req) {
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI(TAG, "Saving features");

    bool sip_enabled = true;
    if (extract_json_bool(content, "sip_enabled", &sip_enabled)) {
        sip_set_enabled(sip_enabled);
        ESP_LOGI(TAG, "SIP feature %s", sip_enabled ? "enabled" : "disabled");
    }

    // TODO: Handle other feature toggles when implemented
    // extract_json_bool(content, "tr064_enabled", &tr064_enabled);
    // extract_json_bool(content, "http_cam_enabled", &http_cam_enabled);
    // extract_json_bool(content, "rtsp_enabled", &rtsp_enabled);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Features saved successfully", HTTPD_RESP_USE_STRLEN);
}

// POST /api/sip - Save SIP configuration
static esp_err_t api_sip_post_handler(httpd_req_t *req) {
    char content[256];
    int ret;
    sip_config_t config;

    ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI(TAG, "Received SIP config");

    memset(&config, 0, sizeof(config));

    extract_json_string(content, "sip_user", config.sip_user, sizeof(config.sip_user));
    extract_json_string(content, "sip_password", config.sip_password, sizeof(config.sip_password));
    extract_json_string(content, "sip_displayname", config.sip_displayname, sizeof(config.sip_displayname));
    extract_json_string(content, "sip_target", config.sip_target, sizeof(config.sip_target));

    // Set defaults if not provided
    if (config.sip_displayname[0] == '\0') {
        strcpy(config.sip_displayname, "Doorbell");
    }

    if (sip_config_save(&config) == ESP_OK) {
        const char *resp = "{\"success\":true,\"message\":\"SIP configuration saved\"}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, strlen(resp));
    } else {
        const char *resp = "{\"success\":false,\"message\":\"Failed to save SIP configuration\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, resp, strlen(resp));
    }
}

// POST /api/sip/ring - Trigger SIP ring (deferred to main loop)
static esp_err_t api_sip_ring_handler(httpd_req_t *req) {
    sip_config_t config;
    if (!sip_config_load(&config) || !sip_config_valid(&config)) {
        const char *resp = "{\"success\":false,\"message\":\"SIP not configured\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, resp, strlen(resp));
    }

    // Use deferred ring to avoid stack overflow in HTTP handler context
    esp_err_t err = sip_request_ring();
    if (err == ESP_OK) {
        const char *resp = "{\"success\":true,\"message\":\"SIP ring initiated\"}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, strlen(resp));
    } else {
        const char *resp = "{\"success\":false,\"message\":\"Failed to initiate SIP ring\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, resp, strlen(resp));
    }
}

// Legacy POST /saveSIP handler for sip.html compatibility
static esp_err_t save_sip_handler(httpd_req_t *req) {
    return api_sip_post_handler(req);
}

// Legacy GET /ring/sip handler (deferred to main loop)
static esp_err_t ring_sip_handler(httpd_req_t *req) {
    sip_config_t config;
    if (!sip_config_load(&config) || !sip_config_valid(&config)) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "SIP not configured", HTTPD_RESP_USE_STRLEN);
    }

    // Use deferred ring to avoid stack overflow in HTTP handler context
    esp_err_t err = sip_request_ring();
    httpd_resp_set_type(req, "text/plain");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "SIP ring initiated", HTTPD_RESP_USE_STRLEN);
    } else {
        return httpd_resp_send(req, "SIP ring failed", HTTPD_RESP_USE_STRLEN);
    }
}

// GET /api/sip/verbose - Get verbose logging state
static esp_err_t api_sip_verbose_get_handler(httpd_req_t *req) {
    char response[64];
    bool verbose = sip_verbose_logging();
    snprintf(response, sizeof(response), "{\"verbose\":%s}", verbose ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

// POST /api/sip/verbose - Set verbose logging state
static esp_err_t api_sip_verbose_post_handler(httpd_req_t *req) {
    char content[64];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    bool verbose = false;
    if (extract_json_bool(content, "verbose", &verbose)) {
        sip_set_verbose_logging(verbose);
        ESP_LOGI(TAG, "SIP verbose logging %s", verbose ? "enabled" : "disabled");
    }

    char response[64];
    snprintf(response, sizeof(response), "{\"verbose\":%s}", verbose ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

/**
 * Stub handlers for legacy Arduino endpoints
 * These return empty/placeholder responses to prevent 404 errors
 */
static esp_err_t stub_camera_info_handler(httpd_req_t *req) {
    const char *resp = "{\"streaming\":false,\"message\":\"Camera not implemented yet\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t stub_device_status_handler(httpd_req_t *req) {
    char response[512];
    char ip[16] = "Not connected";
    if (wifi_manager_is_connected()) {
        wifi_manager_get_ip(ip, sizeof(ip));
    }

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime_sec = esp_log_timestamp() / 1000;

    snprintf(response, sizeof(response),
             "{\"wifi_connected\":%s,\"ip\":\"%s\",\"uptime\":%lu,\"free_heap\":%lu}",
             wifi_manager_is_connected() ? "true" : "false",
             ip,
             uptime_sec,
             free_heap);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t stub_sip_debug_handler(httpd_req_t *req) {
    char response[512];
    sip_config_t config;
    bool has_config = sip_config_load(&config) && sip_config_valid(&config);
    bool enabled = sip_is_enabled();
    bool registered = sip_is_registered();
    sip_status_t status;
    sip_get_status(&status);

    snprintf(response, sizeof(response),
             "{\"sip_enabled\":%s,\"configured\":%s,\"registered\":%s,"
             "\"user\":\"%s\",\"target\":\"%s\",\"displayname\":\"%s\","
             "\"last_status\":%d,\"ringing\":%s}",
             enabled ? "true" : "false",
             has_config ? "true" : "false",
             registered ? "true" : "false",
             has_config ? config.sip_user : "",
             has_config ? config.sip_target : "",
             has_config ? config.sip_displayname : "",
             status.last_status_code,
             sip_ring_active() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t stub_status_handler(httpd_req_t *req) {
    // Redirect to /api/status
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/api/status");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * Captive Portal Handlers
 * These respond to common captive portal detection URLs from various OSes
 * to trigger the "Sign in to network" popup
 */

// Android captive portal detection
static esp_err_t captive_generate_204_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: Android detection (generate_204)");
    // Return 302 redirect to trigger captive portal popup
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

// iOS/macOS captive portal detection
static esp_err_t captive_hotspot_detect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: iOS/macOS detection (hotspot-detect)");
    // iOS expects a specific response - return redirect instead
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

// iOS/macOS older detection
static esp_err_t captive_success_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: iOS detection (success.html)");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

// Windows captive portal detection
static esp_err_t captive_connecttest_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: Windows detection (connecttest.txt)");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

// Windows NCSI detection
static esp_err_t captive_ncsi_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: Windows detection (ncsi.txt)");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

// Firefox captive portal detection
static esp_err_t captive_canonical_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: Firefox detection (canonical.html)");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

// Generic redirect handler
static esp_err_t captive_redirect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal: generic redirect");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-setup.html");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * Handler for device restart
 * GET /restart
 * Serves a styled page that shows restart progress and auto-reconnects
 */
static esp_err_t restart_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Restart requested via web interface");

    // Serve a styled restart page with auto-reconnect
    const char *html =
        "<!DOCTYPE html>"
        "<html><head>"
        "<title>Restarting...</title>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"stylesheet\" href=\"/style.css\">"
        "<style>"
        ".restart-container { text-align: center; padding: 40px 20px; max-width: 400px; margin: 0 auto; }"
        ".spinner { width: 50px; height: 50px; border: 4px solid #333; border-top: 4px solid #4CAF50; "
        "border-radius: 50%; animation: spin 1s linear infinite; margin: 20px auto; }"
        "@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }"
        ".status { font-size: 1.2em; margin: 20px 0; }"
        ".progress { background: #333; border-radius: 10px; height: 20px; overflow: hidden; margin: 20px 0; }"
        ".progress-bar { background: linear-gradient(90deg, #4CAF50, #8BC34A); height: 100%; width: 0%; "
        "transition: width 0.5s ease; }"
        ".reconnect-status { color: #888; font-size: 0.9em; }"
        "</style>"
        "</head><body>"
        "<div class=\"restart-container\">"
        "<h1>Restarting Device</h1>"
        "<div class=\"spinner\"></div>"
        "<div class=\"status\" id=\"status\">Sending restart command...</div>"
        "<div class=\"progress\"><div class=\"progress-bar\" id=\"progress\"></div></div>"
        "<div class=\"reconnect-status\" id=\"reconnect\"></div>"
        "</div>"
        "<script>"
        "let stage = 0;"
        "const stages = ["
        "  { msg: 'Restarting device...', pct: 20 },"
        "  { msg: 'Waiting for reboot...', pct: 40 },"
        "  { msg: 'Device rebooting...', pct: 60 },"
        "  { msg: 'Reconnecting...', pct: 80 },"
        "  { msg: 'Connected!', pct: 100 }"
        "];"
        "function updateUI(msg, pct) {"
        "  document.getElementById('status').textContent = msg;"
        "  document.getElementById('progress').style.width = pct + '%';"
        "}"
        "function tryConnect() {"
        "  fetch('/api/status', { method: 'GET', cache: 'no-store' })"
        "    .then(r => r.json())"
        "    .then(data => {"
        "      updateUI('Connected!', 100);"
        "      document.getElementById('reconnect').textContent = 'Redirecting...';"
        "      setTimeout(() => { window.location.href = '/'; }, 1000);"
        "    })"
        "    .catch(() => {"
        "      document.getElementById('reconnect').textContent = 'Waiting for device...';"
        "      setTimeout(tryConnect, 1500);"
        "    });"
        "}"
        "setTimeout(() => { updateUI('Waiting for reboot...', 40); }, 1000);"
        "setTimeout(() => { updateUI('Device rebooting...', 60); }, 3000);"
        "setTimeout(() => {"
        "  updateUI('Reconnecting...', 80);"
        "  tryConnect();"
        "}, 5000);"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));

    // Schedule restart after response is sent
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

/**
 * Handler for OTA firmware update
 * POST /api/ota
 * Body: Binary firmware file
 */
static esp_err_t api_ota_handler(httpd_req_t *req) {
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = NULL;
    esp_err_t err;

    ESP_LOGI(TAG, "Starting OTA update...");

    // Get next OTA partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "No OTA partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx",
             update_partition->label, update_partition->address);

    // Begin OTA
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "OTA begin failed");
        return err;
    }

    // Receive and write firmware data
    char buf[1024];
    int remaining = req->content_len;
    int received;
    size_t total_written = 0;

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "OTA receive failed");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Firmware upload failed");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "OTA write failed");
            return err;
        }

        remaining -= received;
        total_written += received;

        // Log progress every 100KB
        if (total_written % (100 * 1024) == 0) {
            ESP_LOGI(TAG, "OTA progress: %zu bytes written", total_written);
        }
    }

    ESP_LOGI(TAG, "OTA write complete: %zu bytes", total_written);

    // End OTA and validate
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "OTA validation failed");
        return err;
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                           "Failed to set boot partition");
        return err;
    }

    ESP_LOGI(TAG, "✓ OTA update successful! Rebooting...");

    // Send success response
    const char *resp = "{\"success\":true,\"message\":\"Update successful, rebooting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    // Reboot after a short delay
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

httpd_handle_t web_server_start(void) {
    ESP_LOGI(TAG, "Starting HTTP server");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.max_uri_handlers = 32;  // Increased for all handlers including SIP API

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    // Register API endpoints (must be before wildcard)
    httpd_uri_t api_wifi = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = api_wifi_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi);

    httpd_uri_t api_wifi_delete = {
        .uri = "/api/wifi",
        .method = HTTP_DELETE,
        .handler = api_wifi_delete_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_delete);

    httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_status);

    httpd_uri_t api_ota = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = api_ota_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ota);

    httpd_uri_t api_logs = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_logs);

    httpd_uri_t api_logs_clear = {
        .uri = "/api/logs",
        .method = HTTP_DELETE,
        .handler = api_logs_clear_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_logs_clear);

    // Features API endpoints
    httpd_uri_t api_features = {
        .uri = "/api/features",
        .method = HTTP_GET,
        .handler = api_features_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_features);

    httpd_uri_t save_features = {
        .uri = "/saveFeatures",
        .method = HTTP_POST,
        .handler = save_features_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_features);

    // Register legacy Arduino endpoints for wifi-setup.html compatibility
    httpd_uri_t save_wifi = {
        .uri = "/saveWiFi",
        .method = HTTP_POST,
        .handler = save_wifi_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_wifi);

    httpd_uri_t scan_wifi = {
        .uri = "/scanWifi",
        .method = HTTP_GET,
        .handler = scan_wifi_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &scan_wifi);

    httpd_uri_t wifi_scan_results = {
        .uri = "/wifiScanResults",
        .method = HTTP_GET,
        .handler = wifi_scan_results_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_scan_results);

    // Register stub endpoints for legacy Arduino compatibility
    httpd_uri_t stub_camera = {
        .uri = "/cameraStreamInfo",
        .method = HTTP_GET,
        .handler = stub_camera_info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stub_camera);

    httpd_uri_t stub_device = {
        .uri = "/deviceStatus",
        .method = HTTP_GET,
        .handler = stub_device_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stub_device);

    httpd_uri_t stub_sip = {
        .uri = "/sipDebug",
        .method = HTTP_GET,
        .handler = stub_sip_debug_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stub_sip);

    // SIP API endpoints
    httpd_uri_t api_sip_get = {
        .uri = "/api/sip",
        .method = HTTP_GET,
        .handler = api_sip_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_sip_get);

    httpd_uri_t api_sip_post = {
        .uri = "/api/sip",
        .method = HTTP_POST,
        .handler = api_sip_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_sip_post);

    httpd_uri_t api_sip_ring = {
        .uri = "/api/sip/ring",
        .method = HTTP_POST,
        .handler = api_sip_ring_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_sip_ring);

    // Legacy SIP endpoints
    httpd_uri_t save_sip = {
        .uri = "/saveSIP",
        .method = HTTP_POST,
        .handler = save_sip_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_sip);

    httpd_uri_t ring_sip = {
        .uri = "/ring/sip",
        .method = HTTP_GET,
        .handler = ring_sip_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ring_sip);

    httpd_uri_t sip_verbose_get = {
        .uri = "/api/sip/verbose",
        .method = HTTP_GET,
        .handler = api_sip_verbose_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sip_verbose_get);

    httpd_uri_t sip_verbose_post = {
        .uri = "/api/sip/verbose",
        .method = HTTP_POST,
        .handler = api_sip_verbose_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sip_verbose_post);

    httpd_uri_t stub_status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = stub_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stub_status);

    httpd_uri_t restart = {
        .uri = "/restart",
        .method = HTTP_GET,
        .handler = restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &restart);

    // Register captive portal detection handlers
    httpd_uri_t captive_generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = captive_generate_204_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_generate_204);

    httpd_uri_t captive_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = captive_hotspot_detect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_hotspot);

    httpd_uri_t captive_success = {
        .uri = "/library/test/success.html",
        .method = HTTP_GET,
        .handler = captive_success_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_success);

    httpd_uri_t captive_connecttest = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = captive_connecttest_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_connecttest);

    httpd_uri_t captive_ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = captive_ncsi_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_ncsi);

    httpd_uri_t captive_canonical = {
        .uri = "/canonical.html",
        .method = HTTP_GET,
        .handler = captive_canonical_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_canonical);

    httpd_uri_t captive_redirect = {
        .uri = "/redirect",
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive_redirect);

    ESP_LOGI(TAG, "Captive portal handlers registered");

    // Register root handler for WiFi-based redirect (before wildcard)
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    // Register /capture handler to silently handle camera requests when camera is disabled
    httpd_uri_t capture = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = stub_camera_info_handler,  // Reuse stub handler - returns "not implemented"
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &capture);

    // Register wildcard handler for assets (must be last)
    httpd_uri_t assets = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = asset_handler,
        .user_ctx = NULL
    };
    esp_err_t err = httpd_register_uri_handler(server, &assets);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register wildcard handler: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wildcard handler registered for /*");
    }

    ESP_LOGI(TAG, "✓ HTTP server started on port %d with %zu embedded assets",
             config.server_port, embedded_files_count);
    ESP_LOGI(TAG, "API endpoints: /api/wifi, /api/status, /api/ota");
    ESP_LOGI(TAG, "WiFi endpoints: /saveWiFi, /scanWifi, /wifiScanResults");
    ESP_LOGI(TAG, "Stub endpoints: /cameraStreamInfo, /deviceStatus, /sipDebug, /status");
    return server;
}

esp_err_t web_server_stop(httpd_handle_t server) {
    if (server) {
        ESP_LOGI(TAG, "Stopping HTTP server");
        return httpd_stop(server);
    }
    return ESP_OK;
}
