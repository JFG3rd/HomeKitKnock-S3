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

// Core affinity guidance (ESP32-S3 dual-core).
// Reserve core 0 for Wi-Fi/LwIP; pin streaming/audio tasks to core 1.
#define WIFI_TASK_CORE 0
#define STREAM_TASK_CORE 1

// Audio defaults and I2S pin mapping.
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_SAMPLE_BITS 16
#define DEFAULT_MIC_SENSITIVITY 70
#define DEFAULT_AUDIO_OUT_VOLUME 70

// XIAO ESP32-S3 Sense onboard PDM mic (I2S0 RX).
#define I2S_PDM_MIC_CLK 42
#define I2S_PDM_MIC_DATA 41

// MAX98357A I2S DAC (I2S1 TX).
#define I2S_DAC_BCLK 7
#define I2S_DAC_LRCLK 8
#define I2S_DAC_DOUT 9

#endif // CONFIG_H
