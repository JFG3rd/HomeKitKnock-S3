/**
 * DNS Server Component for Captive Portal
 *
 * Redirects all DNS queries to the AP's IP address (192.168.4.1)
 * to enable captive portal functionality.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the DNS server for captive portal
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t dns_server_start(void);

/**
 * Stop the DNS server
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t dns_server_stop(void);

/**
 * Check if DNS server is running
 *
 * @return true if running, false otherwise
 */
bool dns_server_is_running(void);

#ifdef __cplusplus
}
#endif
