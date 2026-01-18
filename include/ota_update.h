/*
 * Project: HomeKitKnock-S3
 * File: include/ota_update.h
 * Author: Jesse Greene
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Default OTA window is 5 minutes. Override via build flags if needed.
#ifndef OTA_ENABLE_WINDOW_MS
#define OTA_ENABLE_WINDOW_MS (5UL * 60UL * 1000UL)
#endif

// Restrict OTA endpoints to the same subnet as the ESP32.
#ifndef OTA_REQUIRE_LOCAL_ONLY
#define OTA_REQUIRE_LOCAL_ONLY 1
#endif

class OtaUpdate {
 public:
  void begin(AsyncWebServer &server);
  void loop();

 private:
  AsyncWebServer *server_ = nullptr;
  uint32_t enabledUntilMs_ = 0;
  uint32_t rebootAtMs_ = 0;
  bool rebootScheduled_ = false;
  bool uploadInProgress_ = false;
  bool uploadRejected_ = false;
  bool uploadRejectNeedsAuth_ = false;
  bool uploadHasError_ = false;
  uint8_t uploadCommand_ = 0;
  String uploadRejectMessage_;
  int uploadRejectCode_ = 0;

  bool isWindowActive() const;
  uint32_t remainingMs() const;
  void enableWindow(uint32_t durationMs);
  void disableWindow();
  void scheduleReboot(uint32_t delayMs);

  void handleOtaPage(AsyncWebServerRequest *request);
  void handleStatus(AsyncWebServerRequest *request);
  void handleEnable(AsyncWebServerRequest *request);
  void handleDisable(AsyncWebServerRequest *request);
  void handleConfigBody(AsyncWebServerRequest *request,
                        uint8_t *data,
                        size_t len,
                        size_t index,
                        size_t total);
  void handleUpload(AsyncWebServerRequest *request,
                    const String &filename,
                    size_t index,
                    uint8_t *data,
                    size_t len,
                    bool final,
                    uint8_t command);
  void handleUploadComplete(AsyncWebServerRequest *request);

  bool enforceLocalNetwork(AsyncWebServerRequest *request);
  bool enforceAuth(AsyncWebServerRequest *request);
  bool enforceOtaWindow(AsyncWebServerRequest *request);
  void sendAuthRequired(AsyncWebServerRequest *request);
  bool isLocalRequest(AsyncWebServerRequest *request) const;
  bool checkAuthHeader(AsyncWebServerRequest *request);

  bool hasCredentials();
  bool loadCredentials(String &user, String &salt, String &hash);
  bool verifyCredentials(const String &user, const String &password);
  bool validateUsername(const String &user) const;
  bool validatePassword(const String &password) const;
  bool saveCredentials(const String &user, const String &password);
};

#endif // OTA_UPDATE_H
