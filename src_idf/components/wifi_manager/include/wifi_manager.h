/**
 * WiFi Manager Component
 *
 * Manages WiFi connectivity with automatic STA/AP mode switching,
 * credential storage, and event handling.
 *
 * Features:
 * - Station (STA) mode for connecting to home WiFi
 * - Access Point (AP) mode for provisioning
 * - Credential storage in NVS
 * - Event-driven architecture
 * - Automatic reconnection
 */

#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WiFi Manager Events
 */
typedef enum {
    WIFI_MGR_EVENT_STA_CONNECTED,     /**< Connected to AP */
    WIFI_MGR_EVENT_STA_DISCONNECTED,  /**< Disconnected from AP */
    WIFI_MGR_EVENT_STA_GOT_IP,        /**< Got IP address */
    WIFI_MGR_EVENT_AP_STARTED,        /**< AP mode started */
    WIFI_MGR_EVENT_AP_STOPPED,        /**< AP mode stopped */
} wifi_mgr_event_t;

/**
 * WiFi Manager Event Callback
 *
 * @param event Event type
 * @param data Event-specific data (can be NULL)
 */
typedef void (*wifi_mgr_event_cb_t)(wifi_mgr_event_t event, void *data);

/**
 * @brief Initialize WiFi manager
 *
 * Sets up event loop, network interfaces, and WiFi driver.
 * Must be called after NVS initialization.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Start WiFi in Station mode
 *
 * Loads credentials from NVS and attempts to connect.
 *
 * @return ESP_OK if connection initiated
 *         ESP_ERR_NOT_FOUND if no credentials stored
 *         Error code otherwise
 */
esp_err_t wifi_manager_start_sta(void);

/**
 * @brief Start WiFi in Access Point mode
 *
 * Creates a WiFi network for provisioning.
 * Default SSID: "doorbell-setup"
 * Default Password: "doorbell123"
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief Stop WiFi (any mode)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Save WiFi credentials to NVS
 *
 * @param ssid WiFi SSID (max 31 characters)
 * @param password WiFi password (max 63 characters)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_save_credentials(const char *ssid,
                                         const char *password);

/**
 * @brief Clear saved WiFi credentials
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_clear_credentials(void);

/**
 * @brief Check if WiFi credentials are saved
 *
 * @return true if credentials exist, false otherwise
 */
bool wifi_manager_has_credentials(void);

/**
 * @brief Register event callback
 *
 * @param callback Callback function to receive WiFi events
 */
void wifi_manager_set_event_callback(wifi_mgr_event_cb_t callback);

/**
 * @brief Check if connected to WiFi with IP address
 *
 * @return true if connected with IP, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get current IP address (if connected)
 *
 * @param ip_str Buffer to store IP string (min 16 bytes)
 * @param len Buffer length
 * @return ESP_OK if IP retrieved, ESP_FAIL if not connected
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);

/**
 * @brief Start WiFi network scan
 *
 * Initiates an asynchronous scan for available WiFi networks.
 * Results can be retrieved with wifi_manager_get_scan_results().
 *
 * @return ESP_OK if scan started, error code otherwise
 */
esp_err_t wifi_manager_start_scan(void);

/**
 * @brief Check if WiFi scan is in progress
 *
 * @return true if scan is in progress, false otherwise
 */
bool wifi_manager_is_scan_in_progress(void);

/**
 * @brief Get WiFi scan results
 *
 * @param ap_records Array to store scan results
 * @param max_records Maximum number of records to retrieve
 * @param num_found Pointer to store actual number of networks found
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_get_scan_results(wifi_ap_record_t *ap_records,
                                         uint16_t max_records,
                                         uint16_t *num_found);

#ifdef __cplusplus
}
#endif
