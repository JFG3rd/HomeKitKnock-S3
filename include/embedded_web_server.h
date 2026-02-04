/*
 * Project: HomeKitKnock-S3
 * File: include/embedded_web_server.h
 * Purpose: Serve embedded web assets from PROGMEM without filesystem
 */

#pragma once

#include <ESPAsyncWebServer.h>
#include <Arduino.h>
#include "embedded_web_assets.h"
#include <WiFi.h>

// Forward declare external AP mode check (defined in wifi_ap.h)
extern bool isAPModeActive();

// Register embedded web asset routes with web server
// Embedded assets are pre-compressed with gzip, served with Content-Encoding header
void register_embedded_web_assets(AsyncWebServer& server) {
  
  // Serve root: wifi-setup.html if in AP mode or not connected to WiFi, otherwise index.html
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Check if in AP mode or not connected to a station network
    bool shouldServeSetup = isAPModeActive() || (WiFi.status() != WL_CONNECTED);
    
    const char* filename = shouldServeSetup ? "wifi-setup.html" : "index.html";
    const EmbeddedFile* file = find_embedded_file(filename);
    
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
      // Note: Content-Encoding header added by client library after send
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve style.css
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("style.css");
    if (file) {
      request->send_P(200, "text/css", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve setup.html
  server.on("/setup.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("setup.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve wifi-setup.html
  server.on("/wifi-setup.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("wifi-setup.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve live.html
  server.on("/live.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("live.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve guide.html
  server.on("/guide.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("guide.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve ota.html
  server.on("/ota.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("ota.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve sip.html
  server.on("/sip.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("sip.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve tr064.html
  server.on("/tr064.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("tr064.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve logs-doorbell.html
  server.on("/logs-doorbell.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("logs-doorbell.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve logs-camera.html
  server.on("/logs-camera.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("logs-camera.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });
}

