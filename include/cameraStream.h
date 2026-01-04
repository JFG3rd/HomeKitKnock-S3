#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include "camera_pins.h"
#ifdef CAMERA

// Start the ESP-IDF httpd server that serves MJPEG on port 81.
// This is kept separate from AsyncWebServer to avoid blocking.
void startCameraStreamServer();

#endif // CAMERA
#endif // CAMERA_STREAM_H
