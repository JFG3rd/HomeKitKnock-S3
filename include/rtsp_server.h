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

// Check if the RTSP handler task is running.
bool isRtspTaskRunning();

// Get RTSP stream URL
// Returns something like: rtsp://192.168.178.188:8554/mjpeg/1
String getRtspUrl();

// Get number of active RTSP sessions
int getRtspActiveSessionCount();

// Count of RTSP UDP endPacket failures (video + audio).
uint32_t getRtspUdpEndPacketFailCount();

// Allow or disallow RTSP UDP sessions (TCP interleaved still allowed).
void setRtspAllowUdp(bool allow);

// Handle RTSP client connections (call in loop)
void handleRtspClient();

#endif // RTSP_SERVER_H
