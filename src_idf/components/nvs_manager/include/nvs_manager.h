/**
 * NVS Manager Component
 *
 * Provides robust NVS (Non-Volatile Storage) abstraction with automatic
 * error recovery and corruption handling.
 *
 * Features:
 * - Automatic corruption recovery
 * - Safe initialization (idempotent)
 * - Namespace management
 * - Factory reset support
 */

#pragma once

#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize NVS with automatic recovery
 *
 * Handles corruption, version mismatches, and other NVS errors.
 * Safe to call multiple times (idempotent).
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nvs_manager_init(void);

/**
 * @brief Open NVS namespace with error logging
 *
 * @param namespace Namespace name (max 15 characters)
 * @param mode NVS_READONLY or NVS_READWRITE
 * @param handle Pointer to store the NVS handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nvs_manager_open(const char *namespace,
                            nvs_open_mode_t mode,
                            nvs_handle_t *handle);

/**
 * @brief Factory reset - erase all NVS data
 *
 * WARNING: This will erase all stored configuration!
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nvs_manager_factory_reset(void);

/**
 * @brief Check if NVS has been initialized
 *
 * @return true if initialized, false otherwise
 */
bool nvs_manager_is_initialized(void);

#ifdef __cplusplus
}
#endif
