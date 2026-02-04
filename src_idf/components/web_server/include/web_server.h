/**
 * Web Server Component
 *
 * HTTP server for serving embedded web assets and API endpoints.
 *
 * Features:
 * - Serves embedded gzip-compressed web assets
 * - RESTful API endpoints
 * - Wildcard URI matching
 * - Configurable port
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web server
 *
 * Starts HTTP server on port 80 and registers all routes:
 * - GET / * - Embedded web assets (wildcard)
 * - POST /api/wifi - Save WiFi credentials
 * - GET /api/status - System status
 *
 * @return Handle to the HTTP server, NULL on failure
 */
httpd_handle_t web_server_start(void);

/**
 * @brief Stop the web server
 *
 * @param server Handle returned from web_server_start()
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t web_server_stop(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
