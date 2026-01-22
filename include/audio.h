/*
 * Project: HomeKitKnock-S3
 * File: include/audio.h
 * Author: Jesse Greene
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>

void configureAudio(bool micEnable,
                    bool micMute,
                    uint8_t micSensitivity,
                    bool audioOutEnable,
                    bool audioOutMute,
                    uint8_t audioOutVolume);

// Accessors for audio settings (used by RTSP and planned diagnostics).
bool isMicEnabled();
bool isMicMuted();
uint8_t getMicSensitivity();

bool isAudioOutEnabled();
bool isAudioOutMuted();
uint8_t getAudioOutVolume();

// Capture PCM samples at AUDIO_SAMPLE_RATE into caller-provided buffer.
// Returns false on read errors (buffer may be zeroed).
bool captureMicSamples(int16_t *buffer, size_t sampleCount, uint32_t timeoutMs);
// Output PCM samples to the DAC path (used by SIP/RTSP audio).
bool playAudioSamples(const int16_t *samples, size_t sampleCount, uint32_t timeoutMs);
// Play the gong clip (LittleFS /gong.pcm) or a fallback tone asynchronously.
void playGongAsync();

#endif // AUDIO_H
