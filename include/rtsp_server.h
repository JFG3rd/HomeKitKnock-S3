/*
 * Project: HomeKitKnock-S3
 * File: include/rtsp_server.h
 * Author: Jesse Greene
 */

#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <Arduino.h>

// Initialize and start RTSP server on port 8554
// Returns true if RTSP server started successfully
bool startRtspServer();

// Stop RTSP server
void stopRtspServer();

// Check if RTSP server is running
bool isRtspServerRunning();

// Get RTSP stream URL
// Returns something like: rtsp://192.168.178.188:8554/mjpeg/1
String getRtspUrl();

// Handle RTSP client connections (call in loop)
void handleRtspClient();

#endif // RTSP_SERVER_H
