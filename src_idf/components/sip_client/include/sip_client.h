/*
 * Project: HomeKitKnock-S3
 * File: src_idf/components/sip_client/include/sip_client.h
 * Purpose: SIP client for Fritz!Box integration (ESP-IDF)
 * Author: Jesse Greene
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum string lengths for SIP configuration
#define SIP_MAX_USER_LEN        32
#define SIP_MAX_PASSWORD_LEN    64
#define SIP_MAX_DISPLAYNAME_LEN 32
#define SIP_MAX_TARGET_LEN      32

// SIP configuration stored in NVS
typedef struct {
    char sip_user[SIP_MAX_USER_LEN];           // SIP username (IP phone username from Fritz!Box)
    char sip_password[SIP_MAX_PASSWORD_LEN];   // SIP password
    char sip_displayname[SIP_MAX_DISPLAYNAME_LEN]; // Display name for caller ID
    char sip_target[SIP_MAX_TARGET_LEN];       // Target number to ring (e.g., **610)
} sip_config_t;

// SIP registration status
typedef struct {
    bool registered;           // True if last REGISTER succeeded
    uint32_t last_register_ms; // Timestamp of last REGISTER attempt
    uint32_t last_ok_ms;       // Timestamp of last successful REGISTER
    int last_status_code;      // Last response status code
} sip_status_t;

// Callback invoked while SIP ring is active (for LED animation, etc.)
typedef void (*sip_ring_tick_cb_t)(void);

// Callback invoked on received DTMF digit
typedef void (*sip_dtmf_cb_t)(char digit);

/**
 * @brief Initialize the SIP client
 *
 * Binds UDP sockets for SIP signaling and RTP.
 * Must be called after WiFi is connected.
 *
 * @return ESP_OK on success
 */
esp_err_t sip_client_init(void);

/**
 * @brief Deinitialize the SIP client
 *
 * Closes UDP sockets and frees resources.
 */
void sip_client_deinit(void);

/**
 * @brief Load SIP configuration from NVS
 *
 * @param config Pointer to config structure to fill
 * @return true if config was loaded successfully
 */
bool sip_config_load(sip_config_t *config);

/**
 * @brief Save SIP configuration to NVS
 *
 * @param config Pointer to config structure to save
 * @return ESP_OK on success
 */
esp_err_t sip_config_save(const sip_config_t *config);

/**
 * @brief Check if SIP configuration is complete
 *
 * @param config Pointer to config structure
 * @return true if all required fields are present
 */
bool sip_config_valid(const sip_config_t *config);

/**
 * @brief Send SIP REGISTER to Fritz!Box
 *
 * Handles digest authentication automatically.
 *
 * @param config Pointer to SIP configuration
 * @return ESP_OK on success
 */
esp_err_t sip_register(const sip_config_t *config);

/**
 * @brief Check if periodic REGISTER is needed and send if so
 *
 * Call this periodically (e.g., in main loop).
 *
 * @param config Pointer to SIP configuration
 */
void sip_register_if_needed(const sip_config_t *config);

/**
 * @brief Trigger a SIP ring (INVITE) to the configured target
 *
 * Initiates an INVITE transaction. Use sip_ring_active() to check
 * if ring is in progress, and sip_ring_process() to advance state.
 *
 * WARNING: This function uses significant stack space. Do not call
 * from HTTP handlers or low-stack contexts. Use sip_request_ring() instead.
 *
 * @param config Pointer to SIP configuration
 * @return ESP_OK if INVITE was sent
 */
esp_err_t sip_ring(const sip_config_t *config);

/**
 * @brief Request a SIP ring (deferred execution)
 *
 * Sets a flag to trigger ring in the next main loop iteration.
 * Safe to call from HTTP handlers or other limited-stack contexts.
 *
 * @return ESP_OK if request was queued
 */
esp_err_t sip_request_ring(void);

/**
 * @brief Check and execute pending ring request
 *
 * Call this from the main loop. If a ring was requested via
 * sip_request_ring(), this function will execute it.
 *
 * @param config Pointer to SIP configuration
 */
void sip_check_pending_ring(const sip_config_t *config);

/**
 * @brief Check if a SIP ring is currently active
 *
 * @return true if INVITE transaction is in progress
 */
bool sip_ring_active(void);

/**
 * @brief Process SIP ring state machine
 *
 * Call this periodically while sip_ring_active() returns true.
 * Handles CANCEL after timeout, ACK on answer, BYE on hangup.
 */
void sip_ring_process(void);

/**
 * @brief Handle incoming SIP messages
 *
 * Call this periodically to process incoming SIP responses
 * and requests (OPTIONS, BYE, etc.).
 */
void sip_handle_incoming(void);

/**
 * @brief Process SIP media (RTP send/receive)
 *
 * Call this periodically during active calls.
 * Note: Full RTP audio requires Phase 5 (ESP-ADF integration).
 */
void sip_media_process(void);

/**
 * @brief Get current SIP registration status
 *
 * @param status Pointer to status structure to fill
 */
void sip_get_status(sip_status_t *status);

/**
 * @brief Check if SIP registration is currently OK
 *
 * @return true if registered and registration is not stale
 */
bool sip_is_registered(void);

/**
 * @brief Set callback for ring tick events
 *
 * @param callback Function to call during ring loop
 */
void sip_set_ring_tick_callback(sip_ring_tick_cb_t callback);

/**
 * @brief Set callback for DTMF events
 *
 * @param callback Function to call on DTMF digit
 */
void sip_set_dtmf_callback(sip_dtmf_cb_t callback);

/**
 * @brief Check if SIP feature is enabled
 *
 * @return true if SIP is enabled in settings
 */
bool sip_is_enabled(void);

/**
 * @brief Set SIP feature enabled state
 *
 * @param enabled true to enable SIP, false to disable
 * @return ESP_OK on success
 */
esp_err_t sip_set_enabled(bool enabled);

/**
 * @brief Check if verbose SIP logging is enabled
 *
 * When enabled, full SIP message content is logged to serial.
 *
 * @return true if verbose logging is enabled
 */
bool sip_verbose_logging(void);

/**
 * @brief Set verbose SIP logging state
 *
 * When enabled, full SIP message content is logged to serial
 * for debugging purposes.
 *
 * @param enabled true to enable verbose logging, false to disable
 * @return ESP_OK on success
 */
esp_err_t sip_set_verbose_logging(bool enabled);

#ifdef __cplusplus
}
#endif
