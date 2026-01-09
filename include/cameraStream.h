/*
 * Project: HomeKitKnock-S3
 * File: include/cameraStream.h
 * Author: Jesse Greene
 */

#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include "camera_pins.h"
#ifdef CAMERA
#include <Arduino.h>

// Start the ESP-IDF httpd server that serves MJPEG on port 81.
// This is kept separate from AsyncWebServer to avoid blocking.
void startCameraStreamServer();

// Stop the MJPEG streaming server if it's running.
void stopCameraStreamServer();

// Return true when the MJPEG stream server is active.
bool isCameraStreamServerRunning();

// Update the maximum number of simultaneous MJPEG clients.
void setCameraStreamMaxClients(uint8_t maxClients);

// Snapshot MJPEG client status for UI telemetry.
// Returns true when at least one client is connected.
bool getCameraStreamClientInfo(String &clientIp,
                               uint32_t &clientCount,
                               uint32_t &connectedMs,
                               uint32_t &lastFrameAgeMs,
                               String &clientsJson);

#endif // CAMERA
#endif // CAMERA_STREAM_H
