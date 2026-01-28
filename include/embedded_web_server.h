/*
 * Project: HomeKitKnock-S3
 * File: include/embedded_web_server.h
 * Purpose: Serve embedded web assets from PROGMEM without filesystem
 */

#pragma once

#include <ESPAsyncWebServer.h>
#include <Arduino.h>
#include "embedded_web_assets.h"

// Register embedded web asset routes with web server
// Embedded assets are pre-compressed with gzip, served with Content-Encoding header
void register_embedded_web_assets(AsyncWebServer& server) {
  
  // Serve root as index.html
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("index.html");
    if (file) {
      request->send_P(200, "text/html", file->data, file->size);
      AsyncWebHeader* header = request->getHeader("Content-Encoding");
      if (!header) {
        // Note: Content-Encoding header added by client library after send
      }
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

  // Serve other HTML pages dynamically with regex
  server.on("^/(.+)\\.html?$", HTTP_GET, [](AsyncWebServerRequest* request) {
    String pathArg = request->pathArg(0);
    String filename = "/" + pathArg + ".html";
    
    // Try to find with or without .html
    const EmbeddedFile* file = find_embedded_file(filename.c_str());
    if (!file && pathArg.endsWith(".html")) {
      // Already has .html, try without second .html
      filename = "/" + pathArg;
      file = find_embedded_file(filename.c_str());
    }
    
    if (file) {
      request->send_P(200, file->mime_type, file->data, file->size);
    } else {
      request->send(404, "text/plain", "File Not Found");
    }
  });
}

