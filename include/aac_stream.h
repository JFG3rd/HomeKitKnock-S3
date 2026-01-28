/*
 * Project: HomeKitKnock-S3
 * File: include/aac_stream.h
 * Author: Jesse Greene
 */

#ifndef AAC_STREAM_H
#define AAC_STREAM_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

struct AacStreamConfig {
  uint32_t sampleRateHz;
  uint32_t bitrateBps;
};

// Configure AAC-LC stream settings (RTSP/HTTP audio).
// Call whenever prefs change; will re-init the encoder if needed.
void setAacStreamConfig(uint32_t sampleRateHz, uint32_t bitrateBps);
AacStreamConfig getAacStreamConfig();

// AAC-LC uses 1024 samples per frame.
uint16_t getAacFrameSamples();

// SDP helpers for RTSP (MPEG4-GENERIC payload).
String getAacSdpRtpmap();
String getAacSdpFmtp();

// Encode one AAC frame from the mic and return raw AAC payload (no ADTS header).
bool getAacRawFrameFromMic(uint8_t *out, size_t outMax, size_t &outLen);

// Encode one AAC frame from the mic and return ADTS-framed bytes for HTTP streaming.
bool getAacAdtsFrameFromMic(uint8_t *out, size_t outMax, size_t &outLen);

#endif // AAC_STREAM_H
