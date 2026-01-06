/*
 * Project: HomeKitKnock-S3
 * File: include/config.h
 * Author: Jesse Greene
 */

#ifndef CONFIG_H
#define CONFIG_H

// Doorbell button wiring and debounce defaults.
// These are compile-time settings so the pin can be changed without touching logic.
#define DOORBELL_BUTTON_PIN 5
#define DOORBELL_BUTTON_ACTIVE_LOW 1
#define DOORBELL_DEBOUNCE_MS 50

#endif // CONFIG_H
