/*
 * Project: HomeKitKnock-S3
 * File: include/embedded_web_server.h
 * Purpose: Serve embedded web assets from PROGMEM without filesystem
 */

#pragma once

#include <ESPAsyncWebServer.h>
#include <Arduino.h>
#include "embedded_web_assets.h"

// Decompress gzip data into buffer
// Returns true if decompression succeeded
bool decompress_gzip(const uint8_t* compressed_data, size_t compressed_size,
                     uint8_t* output_buffer, size_t output_size);

// Register embedded web asset routes with web server
void register_embedded_web_assets(AsyncWebServer& server) {
  // Serve root as index.html
  server.on("/", HTTP_GET, [](AsyncServerRequest* request) {
    const EmbeddedFile* file = find_embedded_file("index.html");
    if (file) {
      AsyncResponse* response = request->beginResponse(200, "text/html");
      response->addHeader("Content-Encoding", "gzip");
      response->write_P(file->data, file->size);
      request->send(response);
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  // Serve CSS and other static files
  server.on("^/([a-zA-Z0-9\\-_\\.]+\\.(css|js|svg|png|jpg|ico))$", HTTP_GET, 
    [](AsyncServerRequest* request) {
      String path = "/" + request->pathArg(0);
      const EmbeddedFile* file = find_embedded_file(path.c_str());
      
      if (file) {
        AsyncResponse* response = request->beginResponse(200, file->mime_type);
        response->addHeader("Content-Encoding", "gzip");
        response->write_P(file->data, file->size);
        request->send(response);
      } else {
        request->send(404, "text/plain", "Not Found");
      }
    });

  // Serve individual page files
  server.on("^/([a-zA-Z0-9\\-_]+)\\.html$", HTTP_GET, 
    [](AsyncServerRequest* request) {
      String path = "/" + request->pathArg(0) + ".html";
      const EmbeddedFile* file = find_embedded_file(path.c_str());
      
      if (file) {
        AsyncResponse* response = request->beginResponse(200, file->mime_type);
        response->addHeader("Content-Encoding", "gzip");
        response->write_P(file->data, file->size);
        request->send(response);
      } else {
        request->send(404, "text/plain", "Not Found");
      }
    });
}
