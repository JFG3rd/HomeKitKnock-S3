/**
 * Configuration Manager Component
 *
 * Manages device configuration stored in NVS:
 * - SIP settings (server, username, password, extension)
 * - Camera settings (resolution, frame rate, quality)
 * - Audio settings (volume, codec)
 * - System settings (device name, timezone)
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration structure for SIP
typedef struct {
    char server[64];        // SIP server address
    uint16_t port;          // SIP server port (default 5060)
    char username[32];      // SIP username
    char password[64];      // SIP password
    char extension[16];     // Extension number
    bool enabled;           // SIP enabled flag
} sip_config_t;

// Configuration structure for Camera
typedef struct {
    uint16_t width;         // Frame width
    uint16_t height;        // Frame height
    uint8_t fps;            // Frames per second
    uint8_t quality;        // JPEG quality (1-100)
    bool enabled;           // Camera enabled flag
} camera_config_t;

// Configuration structure for Audio
typedef struct {
    uint8_t volume;         // Volume level (0-100)
    bool enabled;           // Audio enabled flag
} audio_config_t;

// Configuration structure for System
typedef struct {
    char device_name[32];   // Device name
    int8_t timezone;        // Timezone offset (-12 to +14)
    uint32_t uptime_offset; // Uptime offset for persistence
} system_config_t;

/**
 * @brief Initialize configuration manager
 *
 * Loads configuration from NVS or sets defaults if not found
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_manager_init(void);

/**
 * @brief Get SIP configuration
 *
 * @param config Pointer to sip_config_t structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_get_sip(sip_config_t *config);

/**
 * @brief Set SIP configuration
 *
 * @param config Pointer to sip_config_t structure with new values
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_set_sip(const sip_config_t *config);

/**
 * @brief Get Camera configuration
 *
 * @param config Pointer to camera_config_t structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_get_camera(camera_config_t *config);

/**
 * @brief Set Camera configuration
 *
 * @param config Pointer to camera_config_t structure with new values
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_set_camera(const camera_config_t *config);

/**
 * @brief Get Audio configuration
 *
 * @param config Pointer to audio_config_t structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_get_audio(audio_config_t *config);

/**
 * @brief Set Audio configuration
 *
 * @param config Pointer to audio_config_t structure with new values
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_set_audio(const audio_config_t *config);

/**
 * @brief Get System configuration
 *
 * @param config Pointer to system_config_t structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_get_system(system_config_t *config);

/**
 * @brief Set System configuration
 *
 * @param config Pointer to system_config_t structure with new values
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_set_system(const system_config_t *config);

/**
 * @brief Reset all configuration to defaults
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t config_reset_all(void);

#ifdef __cplusplus
}
#endif
