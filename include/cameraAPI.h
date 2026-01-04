#ifndef CAMERA_API_H
#define CAMERA_API_H

#include "camera_pins.h"
#ifdef CAMERA
#include <ESPAsyncWebServer.h>

// Initialize the camera sensor and configure defaults.
// Returns true when the camera is ready for capture/streaming.
bool initCamera();
// Register REST-style endpoints for status, capture, control, and stream redirect.
void registerCameraEndpoints(AsyncWebServer &server);

#endif // CAMERA
#endif // CAMERA_API_H
