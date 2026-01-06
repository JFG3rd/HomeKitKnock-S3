/*
 * Project: HomeKitKnock-S3
 * File: src/rtsp_server.cpp
 * Author: Jesse Greene
 */

#include "rtsp_server.h"

#ifdef CAMERA

#include <WiFi.h>
#include <esp_camera.h>
#include "OV2640.h"
#include "OV2640Streamer.h"
#include "CRtspSession.h"

// RTSP server configuration
#define RTSP_PORT 8554

// Camera object for RTSP (separate from main camera)
OV2640 cam;

// RTSP server objects
WiFiServer rtspServer(RTSP_PORT);
CStreamer* streamer = nullptr;
CRtspSession* rtspSession = nullptr;
WiFiClient rtspClient;
bool rtspRunning = false;

bool startRtspServer() {
  if (rtspRunning) {
    Serial.println("⚠️ RTSP server already running");
    return true;
  }

  Serial.println("🎥 Starting RTSP server...");
  
  // Initialize camera for RTSP
  cam.init(esp_camera_sensor_get()->pixformat, 
           esp_camera_sensor_get()->status.framesize,
           esp_camera_sensor_get()->status.quality);
  
  // Start RTSP server
  rtspServer.begin();
  
  // Create MJPEG streamer for OV2640
  streamer = new OV2640Streamer(&cam);
  
  if (!streamer) {
    Serial.println("❌ Failed to create RTSP streamer");
    return false;
  }
  
  rtspRunning = true;
  
  String rtspUrl = getRtspUrl();
  Serial.println("✅ RTSP server started");
  Serial.println("📡 RTSP URL: " + rtspUrl);
  Serial.println("   Add this URL to Scrypted as RTSP camera");
  
  return true;
}

void stopRtspServer() {
  if (!rtspRunning) {
    return;
  }
  
  if (rtspSession) {
    delete rtspSession;
    rtspSession = nullptr;
  }
  
  if (rtspClient) {
    rtspClient.stop();
  }
  
  rtspServer.stop();
  
  if (streamer) {
    delete streamer;
    streamer = nullptr;
  }
  
  rtspRunning = false;
  Serial.println("🛑 RTSP server stopped");
}

bool isRtspServerRunning() {
  return rtspRunning;
}

String getRtspUrl() {
  IPAddress ip = WiFi.localIP();
  return "rtsp://" + ip.toString() + ":" + String(RTSP_PORT) + "/mjpeg/1";
}

// Call this in loop() to handle RTSP client connections
void handleRtspClient() {
  if (!rtspRunning || !streamer) {
    return;
  }

  // If no active session, check for new client
  if (!rtspSession) {
    rtspClient = rtspServer.accept();
    
    if (rtspClient) {
      Serial.print("🔌 RTSP client connected from ");
      Serial.println(rtspClient.remoteIP());
      
      // Create RTSP session for this client
      rtspSession = new CRtspSession(&rtspClient, streamer);
    }
  }
  
  // Handle existing session
  if (rtspSession) {
    // Non-blocking request handling
    rtspSession->handleRequests(0);  // 0 = non-blocking
    
    // Check if client disconnected
    if (!rtspClient.connected()) {
      Serial.println("📴 RTSP client disconnected");
      delete rtspSession;
      rtspSession = nullptr;
      rtspClient.stop();
    }
  }
}

#else

// Dummy implementations when CAMERA is not defined
bool startRtspServer() { return false; }
void stopRtspServer() {}
bool isRtspServerRunning() { return false; }
String getRtspUrl() { return ""; }
void handleRtspClient() {}

#endif // CAMERA
