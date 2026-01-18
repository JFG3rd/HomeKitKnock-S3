/*
 * Project: HomeKitKnock-S3
 * File: src/ota_update.cpp
 * Author: Jesse Greene
 */

#include "ota_update.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <memory>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include "logger.h"
#include "version_info.h"

namespace {
constexpr const char *kPrefsNamespace = "ota";
constexpr const char *kPrefsUserKey = "ota_user";
constexpr const char *kPrefsSaltKey = "ota_salt";
constexpr const char *kPrefsHashKey = "ota_hash";
constexpr const char *kAuthRealm = "ESP32 OTA";

String loadUiTemplate(const char *path) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    logEvent(LOG_ERROR, String("OTA UI template missing: ") + path);
    return String();
  }
  String content = file.readString();
  file.close();
  return content;
}

String bytesToHex(const uint8_t *data, size_t length) {
  String hex;
  hex.reserve(length * 2);
  for (size_t i = 0; i < length; i++) {
    char buffer[3];
    snprintf(buffer, sizeof(buffer), "%02x", data[i]);
    hex += buffer;
  }
  return hex;
}

String generateSalt(size_t bytes) {
  String salt;
  salt.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i++) {
    uint8_t value = static_cast<uint8_t>(esp_random());
    char buffer[3];
    snprintf(buffer, sizeof(buffer), "%02x", value);
    salt += buffer;
  }
  return salt;
}

String sha256Hex(const String &input) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(
    &ctx,
    reinterpret_cast<const unsigned char *>(input.c_str()),
    input.length());
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  return bytesToHex(hash, sizeof(hash));
}

bool decodeBasicAuth(const String &header, String *user, String *password) {
  if (!header.startsWith("Basic ")) {
    return false;
  }

  String b64 = header.substring(6);
  size_t outputLen = (b64.length() * 3) / 4 + 1;
  std::unique_ptr<unsigned char[]> decoded(new unsigned char[outputLen + 1]);
  size_t actualLen = 0;
  int result = mbedtls_base64_decode(
    decoded.get(),
    outputLen,
    &actualLen,
    reinterpret_cast<const unsigned char *>(b64.c_str()),
    b64.length());
  if (result != 0) {
    return false;
  }

  decoded[actualLen] = '\0';
  String payload = reinterpret_cast<char *>(decoded.get());
  int colon = payload.indexOf(':');
  if (colon < 0) {
    return false;
  }

  *user = payload.substring(0, colon);
  *password = payload.substring(colon + 1);
  return true;
}

bool sameSubnet(const IPAddress &a, const IPAddress &b, const IPAddress &mask) {
  for (uint8_t i = 0; i < 4; i++) {
    if ((a[i] & mask[i]) != (b[i] & mask[i])) {
      return false;
    }
  }
  return true;
}
}  // namespace

void OtaUpdate::begin(AsyncWebServer &server) {
  server_ = &server;

  // OTA endpoints are registered here so they share the main AsyncWebServer instance.
  server.on("/ota", HTTP_GET, [this](AsyncWebServerRequest *request) {
    handleOtaPage(request);
  });

  server.on("/ota/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
    handleStatus(request);
  });

  server.on("/ota/enable", HTTP_POST, [this](AsyncWebServerRequest *request) {
    handleEnable(request);
  });

  server.on("/ota/disable", HTTP_POST, [this](AsyncWebServerRequest *request) {
    handleDisable(request);
  });

  server.on(
    "/ota/config",
    HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    nullptr,
    [this](AsyncWebServerRequest *request,
           uint8_t *data,
           size_t len,
           size_t index,
           size_t total) {
      handleConfigBody(request, data, len, index, total);
    });

  server.on(
    "/ota/update",
    HTTP_POST,
    [this](AsyncWebServerRequest *request) {
      handleUploadComplete(request);
    },
    [this](AsyncWebServerRequest *request,
           const String &filename,
           size_t index,
           uint8_t *data,
           size_t len,
           bool final) {
      handleUpload(request, filename, index, data, len, final, U_FLASH);
    });

  server.on(
    "/ota/fs",
    HTTP_POST,
    [this](AsyncWebServerRequest *request) {
      handleUploadComplete(request);
    },
    [this](AsyncWebServerRequest *request,
           const String &filename,
           size_t index,
           uint8_t *data,
           size_t len,
           bool final) {
      handleUpload(request, filename, index, data, len, final, U_SPIFFS);
    });
}

void OtaUpdate::loop() {
  if (enabledUntilMs_ != 0 && static_cast<int32_t>(enabledUntilMs_ - millis()) <= 0) {
    enabledUntilMs_ = 0;
    logEvent(LOG_INFO, "OTA window expired");
  }

  if (rebootScheduled_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
    logEvent(LOG_INFO, "Rebooting after OTA update");
    ESP.restart();
  }
}

bool OtaUpdate::isWindowActive() const {
  if (enabledUntilMs_ == 0) {
    return false;
  }
  return static_cast<int32_t>(enabledUntilMs_ - millis()) > 0;
}

uint32_t OtaUpdate::remainingMs() const {
  if (!isWindowActive()) {
    return 0;
  }
  return enabledUntilMs_ - millis();
}

void OtaUpdate::enableWindow(uint32_t durationMs) {
  enabledUntilMs_ = millis() + durationMs;
  logEvent(LOG_INFO, "OTA enabled for " + String(durationMs / 1000) + " seconds");
}

void OtaUpdate::disableWindow() {
  enabledUntilMs_ = 0;
}

void OtaUpdate::scheduleReboot(uint32_t delayMs) {
  rebootScheduled_ = true;
  rebootAtMs_ = millis() + delayMs;
}

void OtaUpdate::handleOtaPage(AsyncWebServerRequest *request) {
  if (!enforceLocalNetwork(request)) {
    return;
  }
  if (!enforceAuth(request)) {
    return;
  }

  String page = loadUiTemplate("/ota.html");
  if (page.isEmpty()) {
    request->send(500, "text/plain", "UI template missing");
    return;
  }
  page.replace("{{FW_VERSION}}", String(FW_VERSION));
  page.replace("{{FW_BUILD_TIME}}", String(FW_BUILD_TIME));

  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void OtaUpdate::handleStatus(AsyncWebServerRequest *request) {
  if (!enforceLocalNetwork(request)) {
    return;
  }
  if (!enforceAuth(request)) {
    return;
  }

  JsonDocument doc;
  doc["configured"] = hasCredentials();
  doc["enabled"] = isWindowActive();
  doc["remaining_ms"] = remainingMs();
  doc["window_ms"] = static_cast<uint32_t>(OTA_ENABLE_WINDOW_MS);

  String user;
  String salt;
  String hash;
  if (loadCredentials(user, salt, hash)) {
    doc["username"] = user;
  } else {
    doc["username"] = "";
  }

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void OtaUpdate::handleEnable(AsyncWebServerRequest *request) {
  if (!enforceLocalNetwork(request)) {
    return;
  }
  if (!enforceAuth(request)) {
    return;
  }
  if (!hasCredentials()) {
    request->send(400, "text/plain", "Configure OTA credentials first.");
    return;
  }

  // Keep OTA enablement short-lived to reduce exposure.
  enableWindow(OTA_ENABLE_WINDOW_MS);
  request->send(200, "text/plain", "OTA enabled.");
}

void OtaUpdate::handleDisable(AsyncWebServerRequest *request) {
  if (!enforceLocalNetwork(request)) {
    return;
  }
  if (!enforceAuth(request)) {
    return;
  }

  disableWindow();
  request->send(200, "text/plain", "OTA disabled.");
}

void OtaUpdate::handleConfigBody(AsyncWebServerRequest *request,
                                 uint8_t *data,
                                 size_t len,
                                 size_t index,
                                 size_t total) {
  if (index == 0) {
    request->_tempObject = new String();
  }

  String *payload = reinterpret_cast<String *>(request->_tempObject);
  if (!payload) {
    request->send(500, "text/plain", "OTA config buffer missing");
    return;
  }

  payload->concat(reinterpret_cast<const char *>(data), len);
  if (index + len < total) {
    return;
  }

  String body = *payload;
  delete payload;
  request->_tempObject = nullptr;

  if (!enforceLocalNetwork(request)) {
    return;
  }
  if (hasCredentials() && !enforceAuth(request)) {
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    request->send(400, "text/plain", "Invalid JSON");
    return;
  }

  String user = doc["username"] | "";
  String password = doc["password"] | "";

  if (!validateUsername(user)) {
    request->send(400, "text/plain", "Invalid username.");
    return;
  }
  if (!validatePassword(password)) {
    request->send(400, "text/plain", "Password must include upper, lower, digit, and symbol.");
    return;
  }

  if (!saveCredentials(user, password)) {
    request->send(500, "text/plain", "Failed to save credentials.");
    return;
  }

  // Disable the OTA window after credential changes to enforce re-authorization.
  disableWindow();
  logEvent(LOG_INFO, "OTA credentials updated");
  request->send(200, "text/plain", "OTA credentials saved.");
}

void OtaUpdate::handleUpload(AsyncWebServerRequest *request,
                             const String &filename,
                             size_t index,
                             uint8_t *data,
                             size_t len,
                             bool final,
                             uint8_t command) {
  if (index == 0) {
    uploadInProgress_ = true;
    uploadRejected_ = false;
    uploadRejectNeedsAuth_ = false;
    uploadHasError_ = false;
    uploadRejectMessage_.clear();
    uploadRejectCode_ = 0;
    uploadCommand_ = command;

    if (!hasCredentials()) {
      uploadRejected_ = true;
      uploadRejectCode_ = 403;
      uploadRejectMessage_ = "OTA credentials not configured.";
      return;
    }
  if (!isWindowActive()) {
    uploadRejected_ = true;
    uploadRejectCode_ = 403;
    uploadRejectMessage_ = "OTA window is disabled.";
    return;
  }
  if (!isLocalRequest(request)) {
    uploadRejected_ = true;
    uploadRejectCode_ = 403;
    uploadRejectMessage_ = "OTA is limited to the local network.";
    return;
  }
  if (!checkAuthHeader(request)) {
    uploadRejected_ = true;
    uploadRejectNeedsAuth_ = true;
    uploadRejectMessage_ = "Authentication required.";
    return;
  }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
      uploadRejected_ = true;
      uploadRejectCode_ = 500;
      uploadRejectMessage_ = "Update init failed.";
      return;
    }

    // Uploads arrive in chunks; capture state so the completion handler can reply once.
    logEvent(LOG_INFO, String("OTA upload started: ") + filename);
  }

  if (uploadRejected_) {
    return;
  }

  if (Update.write(data, len) != len) {
    uploadHasError_ = true;
  }

  if (final) {
    if (!Update.end(true)) {
      uploadHasError_ = true;
    }
  }
}

void OtaUpdate::handleUploadComplete(AsyncWebServerRequest *request) {
  if (uploadRejected_) {
    if (uploadRejectNeedsAuth_) {
      sendAuthRequired(request);
    } else {
      request->send(uploadRejectCode_, "text/plain", uploadRejectMessage_);
    }
    uploadInProgress_ = false;
    return;
  }

  if (!uploadInProgress_) {
    request->send(400, "text/plain", "No OTA upload in progress.");
    return;
  }

  if (uploadHasError_ || Update.hasError()) {
    logEvent(LOG_ERROR, "OTA update failed");
    Update.abort();
    request->send(500, "text/plain", "OTA update failed.");
    uploadInProgress_ = false;
    return;
  }

  logEvent(LOG_INFO, "OTA update completed");
  request->send(200, "text/plain", "OTA update successful. Rebooting...");
  uploadInProgress_ = false;
  scheduleReboot(1500);
}

bool OtaUpdate::enforceLocalNetwork(AsyncWebServerRequest *request) {
  if (!isLocalRequest(request)) {
    request->send(403, "text/plain", "OTA is restricted to the local subnet.");
    return false;
  }
  return true;
}

bool OtaUpdate::enforceAuth(AsyncWebServerRequest *request) {
  if (!hasCredentials()) {
    return true;
  }

  if (!checkAuthHeader(request)) {
    sendAuthRequired(request);
    return false;
  }

  return true;
}

bool OtaUpdate::enforceOtaWindow(AsyncWebServerRequest *request) {
  if (!isWindowActive()) {
    request->send(403, "text/plain", "OTA window is disabled.");
    return false;
  }
  return true;
}

void OtaUpdate::sendAuthRequired(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response =
    request->beginResponse(401, "text/plain", "Authentication required.");
  response->addHeader("WWW-Authenticate", String("Basic realm=\"") + kAuthRealm + "\"");
  request->send(response);
}

bool OtaUpdate::isLocalRequest(AsyncWebServerRequest *request) const {
#if OTA_REQUIRE_LOCAL_ONLY
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress remote = request->client()->remoteIP();
  IPAddress local = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  return sameSubnet(remote, local, mask);
#else
  return true;
#endif
}

bool OtaUpdate::checkAuthHeader(AsyncWebServerRequest *request) {
  if (!hasCredentials()) {
    return true;
  }

  AsyncWebHeader *auth = request->getHeader("Authorization");
  if (!auth) {
    return false;
  }

  String user;
  String password;
  if (!decodeBasicAuth(auth->value(), &user, &password)) {
    return false;
  }

  return verifyCredentials(user, password);
}

bool OtaUpdate::hasCredentials() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }
  bool hasUser = prefs.isKey(kPrefsUserKey);
  bool hasSalt = prefs.isKey(kPrefsSaltKey);
  bool hasHash = prefs.isKey(kPrefsHashKey);
  prefs.end();
  return hasUser && hasSalt && hasHash;
}

bool OtaUpdate::loadCredentials(String &user, String &salt, String &hash) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }
  if (!prefs.isKey(kPrefsUserKey) || !prefs.isKey(kPrefsSaltKey) || !prefs.isKey(kPrefsHashKey)) {
    prefs.end();
    return false;
  }
  user = prefs.getString(kPrefsUserKey, "");
  salt = prefs.getString(kPrefsSaltKey, "");
  hash = prefs.getString(kPrefsHashKey, "");
  prefs.end();
  return !user.isEmpty() && !salt.isEmpty() && !hash.isEmpty();
}

bool OtaUpdate::verifyCredentials(const String &user, const String &password) {
  String storedUser;
  String storedSalt;
  String storedHash;
  if (!loadCredentials(storedUser, storedSalt, storedHash)) {
    return false;
  }
  if (user != storedUser) {
    return false;
  }

  // Compare salted SHA-256 so the password is never stored in plain text.
  String computed = sha256Hex(storedSalt + ":" + password);
  return computed == storedHash;
}

bool OtaUpdate::validateUsername(const String &user) const {
  if (user.length() < 3 || user.length() > 32) {
    return false;
  }
  return true;
}

bool OtaUpdate::validatePassword(const String &password) const {
  if (password.length() < 12) {
    return false;
  }

  bool hasLower = false;
  bool hasUpper = false;
  bool hasDigit = false;
  bool hasSymbol = false;

  for (size_t i = 0; i < password.length(); i++) {
    char c = password[i];
    if (c >= 'a' && c <= 'z') {
      hasLower = true;
    } else if (c >= 'A' && c <= 'Z') {
      hasUpper = true;
    } else if (c >= '0' && c <= '9') {
      hasDigit = true;
    } else {
      hasSymbol = true;
    }
  }

  return hasLower && hasUpper && hasDigit && hasSymbol;
}

bool OtaUpdate::saveCredentials(const String &user, const String &password) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return false;
  }

  // Store only a salted SHA-256 hash to avoid persisting the raw password.
  String salt = generateSalt(16);
  String hash = sha256Hex(salt + ":" + password);

  prefs.putString(kPrefsUserKey, user);
  prefs.putString(kPrefsSaltKey, salt);
  prefs.putString(kPrefsHashKey, hash);
  prefs.end();
  return true;
}
