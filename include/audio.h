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

bool isMicEnabled();
bool isMicMuted();
uint8_t getMicSensitivity();

bool isAudioOutEnabled();
bool isAudioOutMuted();
uint8_t getAudioOutVolume();

bool captureMicSamples(int16_t *buffer, size_t sampleCount, uint32_t timeoutMs);
void playGongAsync();

#endif // AUDIO_H
