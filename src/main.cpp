/*
 * Project: HomeKitKnock-S3
 * File: src/main.cpp
 * Author: Jesse Greene
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "wifi_ap.h"
#include "config.h"
#include "tr064_client.h"
#include "sip_client.h"
#include "cameraAPI.h"
#include "cameraStream.h"
#include "rtsp_server.h"
#include "audio.h"
#include "logger.h"
#include "ota_update.h"
#include "version_info.h"

// Global objects shared across setup/loop.
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;
OtaUpdate otaUpdate;

String wifiSSID, wifiPassword;
Tr064Config tr064Config;
SipConfig sipConfig;
bool cameraReady = false;

static const char *kFeatMicEnabledKey = "mic_en";
static const char *kFeatMicMutedKey = "mic_mute";
static const char *kFeatMicSensitivityKey = "mic_sens";
static const char *kFeatAudioOutEnabledKey = "aud_en";
static const char *kFeatAudioOutMutedKey = "aud_mute";
static const char *kFeatAudioOutVolumeKey = "aud_vol";
// NVS keys must be <= 15 chars; keep feature keys short.
static const char *kFeatSipEnabledKey = "sip_en";
static const char *kFeatTr064EnabledKey = "tr064_en";
static const char *kFeatHttpCamEnabledKey = "http_en";
static const char *kFeatRtspEnabledKey = "rtsp_en";
static const char *kFeatHttpCamMaxClientsKey = "http_max";
static const char *kFeatScryptedSourceKey = "scr_src";
static const char *kFeatScryptedWebhookKey = "scr_hook";
static const char *kFeatScryptedLowLatencyKey = "scr_lat";
static const char *kFeatScryptedLowBufferKey = "scr_buf";
static const char *kFeatScryptedRtspUdpKey = "scr_udp";
static const char *kFeatTimezoneKey = "tz";

// Feature toggles (loaded once at boot, updated on save)
bool sipEnabled = true;
bool tr064Enabled = true;
bool httpCamEnabled = true;
bool rtspEnabled = true;
uint8_t httpCamMaxClients = 2;
String scryptedSource = "http";
String scryptedWebhook = "";
bool scryptedLowLatency = true;
bool scryptedLowBuffer = true;
bool scryptedRtspUdp = false;
bool micEnabled = false;
bool micMuted = false;
uint8_t micSensitivity = DEFAULT_MIC_SENSITIVITY;
bool audioOutEnabled = true;
bool audioOutMuted = false;
uint8_t audioOutVolume = DEFAULT_AUDIO_OUT_VOLUME;
String timezone = "PST8PDT,M3.2.0,M11.1.0";
bool lastButtonPressed = false;
unsigned long lastButtonChangeMs = 0;
bool doorbellLatched = false;
volatile bool sipRingQueued = false;

bool initFileSystem(AsyncWebServer &server) {
  // Mount LittleFS and expose static assets for the UI.
  if (!LittleFS.begin(true)) {
    logEvent(LOG_ERROR, "❌ LittleFS mount failed");
    return false;
  }
  server.serveStatic("/style.css", LittleFS, "/style.css");
  server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");
  return true;
}

static String loadUiTemplate(const char *path) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    logEvent(LOG_ERROR, "❌ UI template missing: " + String(path));
    return String();
  }
  String content = file.readString();
  file.close();
  return content;
}

static void writeWavHeader(AsyncResponseStream *response,
                           uint32_t sampleRate,
                           uint16_t bitsPerSample,
                           uint16_t channels,
                           uint32_t dataBytes) {
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  uint16_t blockAlign = channels * bitsPerSample / 8;
  uint32_t chunkSize = 36 + dataBytes;

  response->write(reinterpret_cast<const uint8_t *>("RIFF"), 4);
  response->write(reinterpret_cast<const uint8_t *>(&chunkSize), 4);
  response->write(reinterpret_cast<const uint8_t *>("WAVE"), 4);
  response->write(reinterpret_cast<const uint8_t *>("fmt "), 4);
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1;
  response->write(reinterpret_cast<const uint8_t *>(&subchunk1Size), 4);
  response->write(reinterpret_cast<const uint8_t *>(&audioFormat), 2);
  response->write(reinterpret_cast<const uint8_t *>(&channels), 2);
  response->write(reinterpret_cast<const uint8_t *>(&sampleRate), 4);
  response->write(reinterpret_cast<const uint8_t *>(&byteRate), 4);
  response->write(reinterpret_cast<const uint8_t *>(&blockAlign), 2);
  response->write(reinterpret_cast<const uint8_t *>(&bitsPerSample), 2);
  response->write(reinterpret_cast<const uint8_t *>("data"), 4);
  response->write(reinterpret_cast<const uint8_t *>(&dataBytes), 4);
}

static const uint32_t kRingLedDurationMs = 6000;
static const uint32_t kLedDoubleBlinkPeriodMs = 1000;
static const uint32_t kLedWifiBlinkPeriodMs = 500;   // 2 Hz blink
static const uint32_t kLedSipPulsePeriodMs = 2000;   // Slow pulse
static const uint32_t kLedRingPulsePeriodMs = 1400;  // Breathing ring pulse
static const uint32_t kLedRtspTickPeriodMs = 2000;
static const uint32_t kLedRtspTickOnMs = 80;

static const uint8_t kLedDutyLow = 24;
static const uint8_t kLedDutyPulseMax = 180;
static const uint8_t kLedDutyPulseMin = 8;
static const uint8_t kLedDutyBlink = 200;
static const uint8_t kLedDutyRingMax = 220;
static const uint8_t kLedDutyRtspTick = 200;

static const int kStatusLedcChannel = 0;
static const int kStatusLedcFreq = 5000;
static const int kStatusLedcResolution = 8;
static const uint8_t kStatusLedcMaxDuty = (1 << kStatusLedcResolution) - 1;
static uint8_t lastLedDuty = 255;
static uint32_t ringLedUntilMs = 0;

static void initStatusLed() {
  ledcSetup(kStatusLedcChannel, kStatusLedcFreq, kStatusLedcResolution);
  ledcAttachPin(STATUS_LED_PIN, kStatusLedcChannel);
  ledcWrite(kStatusLedcChannel, STATUS_LED_ACTIVE_LOW ? kStatusLedcMaxDuty : 0);
}

static void setStatusLedDuty(uint8_t duty) {
  if (duty > kStatusLedcMaxDuty) {
    duty = kStatusLedcMaxDuty;
  }
  if (STATUS_LED_ACTIVE_LOW) {
    duty = kStatusLedcMaxDuty - duty;
  }
  if (duty == lastLedDuty) {
    return;
  }
  ledcWrite(kStatusLedcChannel, duty);
  lastLedDuty = duty;
}

static void markRingLed() {
  uint32_t now = millis();
  ringLedUntilMs = now + kRingLedDurationMs;
}

static bool isRingLedActive(uint32_t now) {
  return static_cast<int32_t>(ringLedUntilMs - now) > 0;
}

static uint8_t triangleWave(uint32_t now, uint32_t periodMs, uint8_t minDuty, uint8_t maxDuty) {
  if (periodMs == 0 || maxDuty <= minDuty) {
    return maxDuty;
  }
  uint32_t phase = now % periodMs;
  uint32_t half = periodMs / 2;
  uint32_t span = maxDuty - minDuty;
  if (phase < half) {
    return static_cast<uint8_t>(minDuty + (span * phase) / half);
  }
  return static_cast<uint8_t>(maxDuty - (span * (phase - half)) / half);
}

static uint8_t doubleBlink(uint32_t now) {
  uint32_t phase = now % kLedDoubleBlinkPeriodMs;
  bool on = (phase < 80) || (phase >= 160 && phase < 240);
  return on ? kLedDutyBlink : 0;
}

static uint8_t blink(uint32_t now, uint32_t periodMs) {
  uint32_t phase = now % periodMs;
  return (phase < (periodMs / 2)) ? kLedDutyBlink : 0;
}

static uint8_t rtspTick(uint32_t now) {
  uint32_t phase = now % kLedRtspTickPeriodMs;
  return (phase < kLedRtspTickOnMs) ? kLedDutyRtspTick : 0;
}

static void updateStatusLed() {
  uint32_t now = millis();
  uint8_t duty = 0;

  // Priority: Ringing > AP mode > Wi-Fi connecting > SIP error > SIP ok > RTSP active.
  if (isRingLedActive(now)) {
    duty = triangleWave(now, kLedRingPulsePeriodMs, kLedDutyPulseMin, kLedDutyRingMax);
  } else if (isAPModeActive()) {
    duty = doubleBlink(now);
  } else if (WiFi.status() != WL_CONNECTED) {
    duty = blink(now, kLedWifiBlinkPeriodMs);
  } else {
    bool sipConfigured = sipEnabled && hasSipConfig(sipConfig);
    bool sipOk = sipConfigured && isSipRegistrationOk();
    bool sipError = false;
    if (sipConfigured && !sipOk) {
      unsigned long lastAttempt = getSipLastRegisterAttemptMs();
      sipError = lastAttempt > 0 && (now - lastAttempt > 5000);
    }
    if (sipError) {
      duty = triangleWave(now, kLedSipPulsePeriodMs, 0, kLedDutyPulseMax);
    } else if (sipOk) {
      duty = kLedDutyLow;
    } else if (rtspEnabled && getRtspActiveSessionCount() > 0) {
      duty = rtspTick(now);
    } else {
      duty = 0;
    }
  }

  setStatusLedDuty(duty);
}

static void sipRingLedTick() {
  updateStatusLed();
}

// Queue SIP rings from async handlers to avoid blocking async_tcp and tripping the WDT.
static bool queueSipRing(const char *source) {
  if (!sipEnabled) {
    logEvent(LOG_INFO, "ℹ️ SIP disabled - cannot queue ring");
    return false;
  }
  if (isSipRingActive() || sipRingQueued) {
    logEvent(LOG_INFO, "ℹ️ SIP ring already queued or active");
    return false;
  }
  sipRingQueued = true;
  logEvent(LOG_INFO, "📞 SIP ring queued (" + String(source) + ")");
  return true;
}

static void triggerDoorbellEvent(bool includeSip) {
  // Show the ring animation regardless of which downstream notifications are enabled.
  markRingLed();
  // Sequence: local gong first, then external notifications.
  playGongAsync();

  // Trigger Scrypted doorbell event (HomeKit notification).
  if (!scryptedWebhook.isEmpty()) {
    HTTPClient http;
    http.begin(scryptedWebhook);
    http.setTimeout(2000);
    int httpCode = http.GET();
    http.end();

    if (httpCode > 0) {
      logEvent(LOG_INFO, "🔔 Scrypted webhook triggered (HomeKit notification)");
    } else {
      logEvent(LOG_WARN, "⚠️ Scrypted webhook failed");
    }
  }

  if (!includeSip) {
    logEvent(LOG_INFO, "ℹ️ SIP skipped for HomeKit test");
    return;
  }

  // Ring FRITZ!Box internal phones (only if SIP enabled).
  if (!queueSipRing("doorbell")) {
    logEvent(LOG_INFO, "ℹ️ SIP ring not queued for doorbell event");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  initEventLog();
  logEvent(LOG_INFO, "\n\n🔔 ESP32-S3 Doorbell Starting...");
  logEvent(LOG_INFO, "====================================");

  // Mount filesystem early so both AP and normal mode can serve CSS.
  initFileSystem(server);

  // Load TR-064 config once at boot; UI can update it later.
  loadTr064Config(tr064Config);
  
  // Load SIP config once at boot; UI can update it later.
  loadSipConfig(sipConfig);

  // Initialize feature toggles with defaults if not already set
  Preferences featPrefs;
  if (!featPrefs.begin("features", false)) {
    logEvent(LOG_WARN, "⚠️ Failed to open features namespace, creating...");
  }
  bool audioPrefsMigrated = false;
  if (!featPrefs.isKey(kFeatMicEnabledKey) && featPrefs.isKey("mic_enabled")) {
    featPrefs.putBool(kFeatMicEnabledKey, featPrefs.getBool("mic_enabled", false));
    featPrefs.remove("mic_enabled");
    audioPrefsMigrated = true;
  }
  if (!featPrefs.isKey(kFeatMicMutedKey) && featPrefs.isKey("mic_muted")) {
    featPrefs.putBool(kFeatMicMutedKey, featPrefs.getBool("mic_muted", false));
    featPrefs.remove("mic_muted");
    audioPrefsMigrated = true;
  }
  if (!featPrefs.isKey(kFeatMicSensitivityKey) && featPrefs.isKey("mic_sensitivity")) {
    featPrefs.putUChar(kFeatMicSensitivityKey, featPrefs.getUChar("mic_sensitivity", DEFAULT_MIC_SENSITIVITY));
    featPrefs.remove("mic_sensitivity");
    audioPrefsMigrated = true;
  }
  if (!featPrefs.isKey(kFeatAudioOutEnabledKey) && featPrefs.isKey("audio_out_enabled")) {
    featPrefs.putBool(kFeatAudioOutEnabledKey, featPrefs.getBool("audio_out_enabled", true));
    featPrefs.remove("audio_out_enabled");
    audioPrefsMigrated = true;
  }
  if (!featPrefs.isKey(kFeatAudioOutMutedKey) && featPrefs.isKey("audio_out_muted")) {
    featPrefs.putBool(kFeatAudioOutMutedKey, featPrefs.getBool("audio_out_muted", false));
    featPrefs.remove("audio_out_muted");
    audioPrefsMigrated = true;
  }
  if (!featPrefs.isKey(kFeatAudioOutVolumeKey) && featPrefs.isKey("audio_out_volume")) {
    featPrefs.putUChar(kFeatAudioOutVolumeKey, featPrefs.getUChar("audio_out_volume", DEFAULT_AUDIO_OUT_VOLUME));
    featPrefs.remove("audio_out_volume");
    audioPrefsMigrated = true;
  }
  if (audioPrefsMigrated) {
    logEvent(LOG_INFO, "✅ Migrated legacy audio preference keys");
  }
  bool featurePrefsMigrated = false;
  auto migrateBoolPref = [&](const char *newKey, const char *oldKey, bool defaultValue) {
    if (!featPrefs.isKey(newKey) && featPrefs.isKey(oldKey)) {
      featPrefs.putBool(newKey, featPrefs.getBool(oldKey, defaultValue));
      featPrefs.remove(oldKey);
      featurePrefsMigrated = true;
    }
  };
  auto migrateStringPref = [&](const char *newKey, const char *oldKey, const String &defaultValue) {
    if (!featPrefs.isKey(newKey) && featPrefs.isKey(oldKey)) {
      featPrefs.putString(newKey, featPrefs.getString(oldKey, defaultValue));
      featPrefs.remove(oldKey);
      featurePrefsMigrated = true;
    }
  };

  // Migrate legacy feature keys (<= 15 chars) into the new short-key scheme.
  migrateBoolPref(kFeatSipEnabledKey, "sip_enabled", true);
  migrateBoolPref(kFeatTr064EnabledKey, "tr064_enabled", true);
  migrateBoolPref(kFeatRtspEnabledKey, "rtsp_enabled", true);
  migrateStringPref(kFeatScryptedSourceKey, "scrypted_source", "http");
  if (!featPrefs.isKey(kFeatScryptedWebhookKey)) {
    Preferences sipPrefs;
    if (sipPrefs.begin("sip", true)) {
      String legacyWebhook = sipPrefs.getString("scrypted_webhook", "");
      sipPrefs.end();
      if (!legacyWebhook.isEmpty()) {
        featPrefs.putString(kFeatScryptedWebhookKey, legacyWebhook);
        logEvent(LOG_INFO, "✅ Migrated Scrypted webhook from SIP settings");
      }
    }
  }
  if (featurePrefsMigrated) {
    logEvent(LOG_INFO, "✅ Migrated legacy feature keys");
  }
  // Initialize missing feature keys individually so new keys don't reset existing values.
  bool defaultsApplied = false;
  auto ensureBoolPref = [&](const char *key, bool value) {
    if (!featPrefs.isKey(key)) {
      featPrefs.putBool(key, value);
      defaultsApplied = true;
    }
  };
  auto ensureUCharPref = [&](const char *key, uint8_t value) {
    if (!featPrefs.isKey(key)) {
      featPrefs.putUChar(key, value);
      defaultsApplied = true;
    }
  };
  auto ensureStringPref = [&](const char *key, const String &value) {
    if (!featPrefs.isKey(key)) {
      featPrefs.putString(key, value);
      defaultsApplied = true;
    }
  };

  ensureBoolPref(kFeatSipEnabledKey, true);
  ensureBoolPref(kFeatTr064EnabledKey, true);
  ensureBoolPref(kFeatHttpCamEnabledKey, true);
  ensureBoolPref(kFeatRtspEnabledKey, true);
  ensureUCharPref(kFeatHttpCamMaxClientsKey, 2);
  ensureStringPref(kFeatScryptedWebhookKey, "");
  ensureBoolPref(kFeatScryptedLowLatencyKey, true);
  ensureBoolPref(kFeatScryptedLowBufferKey, true);
  ensureBoolPref(kFeatScryptedRtspUdpKey, false);
  ensureBoolPref(kFeatMicEnabledKey, false);
  ensureBoolPref(kFeatMicMutedKey, false);
  ensureUCharPref(kFeatMicSensitivityKey, DEFAULT_MIC_SENSITIVITY);
  ensureBoolPref(kFeatAudioOutEnabledKey, true);
  ensureBoolPref(kFeatAudioOutMutedKey, false);
  ensureUCharPref(kFeatAudioOutVolumeKey, DEFAULT_AUDIO_OUT_VOLUME);
  ensureStringPref(kFeatTimezoneKey, "PST8PDT,M3.2.0,M11.1.0");  // Default: Pacific Time with auto DST
  bool httpEnabledPref = featPrefs.getBool(kFeatHttpCamEnabledKey, true);
  bool rtspEnabledPref = featPrefs.getBool(kFeatRtspEnabledKey, true);
  if (!featPrefs.isKey(kFeatScryptedSourceKey)) {
    String defaultSource = httpEnabledPref ? "http" : (rtspEnabledPref ? "rtsp" : "http");
    ensureStringPref(kFeatScryptedSourceKey, defaultSource);
  }

  if (defaultsApplied) {
    logEvent(LOG_INFO, "✅ Feature defaults applied for missing keys");
  }
  // Load feature flags into global variables
  sipEnabled = featPrefs.getBool(kFeatSipEnabledKey, true);
  tr064Enabled = featPrefs.getBool(kFeatTr064EnabledKey, true);
  httpCamEnabled = featPrefs.getBool(kFeatHttpCamEnabledKey, true);
  rtspEnabled = featPrefs.getBool(kFeatRtspEnabledKey, true);
  httpCamMaxClients = featPrefs.getUChar(kFeatHttpCamMaxClientsKey, 2);
  scryptedSource = featPrefs.getString(kFeatScryptedSourceKey, httpCamEnabled ? "http" : "rtsp");
  scryptedWebhook = featPrefs.getString(kFeatScryptedWebhookKey, "");
  scryptedLowLatency = featPrefs.getBool(kFeatScryptedLowLatencyKey, true);
  scryptedLowBuffer = featPrefs.getBool(kFeatScryptedLowBufferKey, true);
  scryptedRtspUdp = featPrefs.getBool(kFeatScryptedRtspUdpKey, false);
  micEnabled = featPrefs.getBool(kFeatMicEnabledKey, false);
  micMuted = featPrefs.getBool(kFeatMicMutedKey, false);
  micSensitivity = featPrefs.getUChar(kFeatMicSensitivityKey, DEFAULT_MIC_SENSITIVITY);
  audioOutEnabled = featPrefs.getBool(kFeatAudioOutEnabledKey, true);
  audioOutMuted = featPrefs.getBool(kFeatAudioOutMutedKey, false);
  audioOutVolume = featPrefs.getUChar(kFeatAudioOutVolumeKey, DEFAULT_AUDIO_OUT_VOLUME);
  featPrefs.end();
  logEvent(LOG_INFO, "📋 Features loaded: SIP=" + String(sipEnabled) +
                     " TR-064=" + String(tr064Enabled) +
                     " HTTP=" + String(httpCamEnabled) +
                     " RTSP=" + String(rtspEnabled) +
                     " HTTP max clients=" + String(httpCamMaxClients));

  #ifdef CAMERA
  setCameraStreamMaxClients(httpCamMaxClients);
  setRtspAllowUdp(scryptedRtspUdp);
  #endif
  configureAudio(micEnabled, micMuted, micSensitivity, audioOutEnabled, audioOutMuted, audioOutVolume);

  // Initialize status LED PWM (off until state machine runs).
  initStatusLed();
  setSipRingTickCallback(sipRingLedTick);

  // Configure doorbell GPIO based on active-low setting.
  pinMode(DOORBELL_BUTTON_PIN, DOORBELL_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT);

  #ifdef CAMERA
  // Initialize camera and register endpoints if hardware is present.
  cameraReady = initCamera();
  if (cameraReady) {
    // Camera API endpoints (/status, /capture, /control, /stream) only exist when init succeeds.
    registerCameraEndpoints(server);
  }
  #endif

  // Load WiFi credentials from preferences
  preferences.begin("wifi", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();

  if (wifiSSID.isEmpty()) {
    // No saved WiFi; force AP provisioning mode.
    logEvent(LOG_WARN, "🚨 No WiFi credentials found. Starting AP mode...");
    startAPMode(server, dnsServer, preferences);
  } else {
    logEvent(LOG_INFO, "🔍 Found saved WiFi: " + wifiSSID);
    attemptWiFiConnection(wifiSSID, wifiPassword);
    
    if (WiFi.status() == WL_CONNECTED) {
      logEvent(LOG_INFO, "✅ Connected to WiFi successfully!");
      
      // Sync time via NTP for accurate event logging.
      // Sync time from NTP for timestamped logs.
      syncTimeFromNTP(timezone);
      // Initialize SIP client and send initial REGISTER only when enabled.
      if (sipEnabled) {
        if (initSipClient()) {
          sendSipRegister(sipConfig);
        }
      } else {
        logEvent(LOG_INFO, "ℹ️ SIP disabled - skipping SIP client init");
      }
      
      // Start web server for normal operation (dashboard + JSON endpoints).
      server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String streamInfo;
        if (httpCamEnabled) {
          streamInfo += "<div class=\"flash-stats\">"
                        "<strong>📹 HTTP Stream:</strong>"
                        "<p style=\"font-family: monospace; font-size: 0.9em; word-break: break-all;\">"
                        "http://" + WiFi.localIP().toString() + ":81/stream"
                        "</p>"
                        "<p style=\"font-size: 0.85em; color: #666;\">"
                        "Use this URL for MJPEG clients (Scrypted HTTP camera, FRITZ!Box)."
                        "</p>"
                        "</div>";
          if (micEnabled) {
            streamInfo += "<div class=\"flash-stats\">"
                          "<strong>🎧 HTTP Audio (WAV):</strong>"
                          "<p style=\"font-family: monospace; font-size: 0.9em; word-break: break-all;\">"
                          "http://" + WiFi.localIP().toString() + ":81/audio"
                          "</p>"
                          "<p style=\"font-size: 0.85em; color: #666;\">"
                          "Companion audio stream for MJPEG. For browser A/V, open <code>/live</code>."
                          "</p>"
                          "</div>";
          } else {
            streamInfo += "<div class=\"flash-stats\">"
                          "<strong>🎧 HTTP Audio:</strong> disabled (mic off)"
                          "</div>";
          }
        } else {
          streamInfo += "<div class=\"flash-stats\">"
                        "<strong>📹 HTTP Stream:</strong> disabled"
                        "</div>";
        }
        if (rtspEnabled) {
          streamInfo += "<div class=\"flash-stats\">"
                        "<strong>📡 RTSP Stream (for Scrypted):</strong>"
                        "<p style=\"font-family: monospace; font-size: 0.9em; word-break: break-all;\">"
                        "rtsp://" + WiFi.localIP().toString() + ":8554/mjpeg/1"
                        "</p>"
                        "<p style=\"font-size: 0.85em; color: #666;\">"
                        "Use the RTSP Camera plugin or prefix with <code>-i</code> if using FFmpeg Camera."
                        "</p>"
                        "</div>";
        } else {
          streamInfo += "<div class=\"flash-stats\">"
                        "<strong>📡 RTSP Stream:</strong> disabled"
                        "</div>";
        }

        String page = loadUiTemplate("/index.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        page.replace("{{LOCAL_IP}}", WiFi.localIP().toString());
        page.replace("{{STREAM_INFO}}", streamInfo);
        page.replace("{{FW_VERSION}}", String(FW_VERSION));
        page.replace("{{FW_BUILD_TIME}}", String(FW_BUILD_TIME));

        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
      });

      server.on("/live", HTTP_GET, [](AsyncWebServerRequest *request) {
        String page = loadUiTemplate("/live.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        page.replace("{{LOCAL_IP}}", WiFi.localIP().toString());
        page.replace("{{FW_VERSION}}", String(FW_VERSION));
        page.replace("{{FW_BUILD_TIME}}", String(FW_BUILD_TIME));
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
      });

      server.on("/logs/camera", HTTP_GET, [](AsyncWebServerRequest *request) {
        String page = loadUiTemplate("/logs-camera.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
      });

      server.on("/logs/doorbell", HTTP_GET, [](AsyncWebServerRequest *request) {
        String page = loadUiTemplate("/logs-doorbell.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
      });

      // Raw PCM endpoint for integrations that want bytes only (no WAV header).
      // Format: signed 16-bit little-endian mono at AUDIO_SAMPLE_RATE.
      server.on("/audio", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!isMicEnabled()) {
          request->send(503, "text/plain", "Mic disabled");
          return;
        }

        const uint32_t sampleRate = AUDIO_SAMPLE_RATE;
        const uint16_t bitsPerSample = AUDIO_SAMPLE_BITS;
        const uint16_t channels = 1;
        const uint32_t sampleCount = sampleRate / 2;
        const uint32_t dataBytes = sampleCount * sizeof(int16_t);
        int16_t *samples = static_cast<int16_t *>(malloc(dataBytes));
        if (!samples) {
          request->send(500, "text/plain", "Audio buffer allocation failed");
          return;
        }

        bool ok = captureMicSamples(samples, sampleCount, 250);
        if (!ok) {
          memset(samples, 0, dataBytes);
        }

        AsyncResponseStream *response = request->beginResponseStream("application/octet-stream");
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("X-Audio-Format", "PCM16LE");
        response->addHeader("X-Audio-Rate", String(sampleRate));
        response->addHeader("X-Audio-Bits", String(bitsPerSample));
        response->addHeader("X-Audio-Channels", String(channels));
        response->write(reinterpret_cast<const uint8_t *>(samples), dataBytes);
        request->send(response);
        free(samples);
      });

      // Short mic preview for browser testing; not a continuous stream.
      server.on("/audio.wav", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!isMicEnabled()) {
          request->send(503, "text/plain", "Mic disabled");
          return;
        }

        const uint32_t sampleRate = AUDIO_SAMPLE_RATE;
        const uint16_t bitsPerSample = AUDIO_SAMPLE_BITS;
        const uint16_t channels = 1;
        const uint32_t sampleCount = sampleRate / 2;
        const uint32_t dataBytes = sampleCount * sizeof(int16_t);
        int16_t *samples = static_cast<int16_t *>(malloc(dataBytes));
        if (!samples) {
          request->send(500, "text/plain", "Audio buffer allocation failed");
          return;
        }

        bool ok = captureMicSamples(samples, sampleCount, 250);
        if (!ok) {
          memset(samples, 0, dataBytes);
        }

        AsyncResponseStream *response = request->beginResponseStream("audio/wav");
        response->addHeader("Cache-Control", "no-store");
        writeWavHeader(response, sampleRate, bitsPerSample, channels, dataBytes);
        response->write(reinterpret_cast<const uint8_t *>(samples), dataBytes);
        request->send(response);
        free(samples);
      });

      // Manual gong trigger for testing audio-out without pressing the doorbell.
      server.on("/gong", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!isAudioOutEnabled()) {
          request->send(503, "text/plain", "Audio out disabled");
          return;
        }
        if (isAudioOutMuted()) {
          request->send(503, "text/plain", "Audio out muted");
          return;
        }
        playGongAsync();
        request->send(200, "text/plain", "Gong triggered");
      });

      server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("features", false);
        bool sip_enabled = prefs.getBool(kFeatSipEnabledKey, true);
        bool tr064_enabled = prefs.getBool(kFeatTr064EnabledKey, true);
        bool http_cam_enabled = prefs.getBool(kFeatHttpCamEnabledKey, true);
        bool rtsp_enabled = prefs.getBool(kFeatRtspEnabledKey, true);
        uint8_t http_cam_max_clients = prefs.getUChar(kFeatHttpCamMaxClientsKey, 2);
        String scrypted_source = prefs.getString(kFeatScryptedSourceKey,
                                                 http_cam_enabled ? "http" : (rtsp_enabled ? "rtsp" : "http"));
        String scrypted_webhook = prefs.getString(kFeatScryptedWebhookKey, "");
        bool scrypted_low_latency = prefs.getBool(kFeatScryptedLowLatencyKey, true);
        bool scrypted_low_buffer = prefs.getBool(kFeatScryptedLowBufferKey, true);
        bool scrypted_rtsp_udp = prefs.getBool(kFeatScryptedRtspUdpKey, false);
        bool mic_enabled = prefs.getBool(kFeatMicEnabledKey, false);
        bool mic_muted = prefs.getBool(kFeatMicMutedKey, false);
        uint8_t mic_sensitivity = prefs.getUChar(kFeatMicSensitivityKey, DEFAULT_MIC_SENSITIVITY);
        bool audio_out_enabled = prefs.getBool(kFeatAudioOutEnabledKey, true);
        bool audio_out_muted = prefs.getBool(kFeatAudioOutMutedKey, false);
        uint8_t audio_out_volume = prefs.getUChar(kFeatAudioOutVolumeKey, DEFAULT_AUDIO_OUT_VOLUME);
        String timezone = prefs.getString(kFeatTimezoneKey, "PST8PDT,M3.2.0,M11.1.0");
        prefs.end();

        if (http_cam_max_clients < 1) {
          http_cam_max_clients = 1;
        } else if (http_cam_max_clients > 4) {
          http_cam_max_clients = 4;
        }

        String page = loadUiTemplate("/setup.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        page.replace("{{SIP_ENABLED_CHECKED}}", sip_enabled ? "checked" : "");
        page.replace("{{TR064_ENABLED_CHECKED}}", tr064_enabled ? "checked" : "");
        page.replace("{{HTTP_CAM_ENABLED_CHECKED}}", http_cam_enabled ? "checked" : "");
        page.replace("{{RTSP_ENABLED_CHECKED}}", rtsp_enabled ? "checked" : "");
        page.replace("{{HTTP_CAM_MAX_CLIENTS}}", String(http_cam_max_clients));
        page.replace("{{SCRYPTED_SOURCE_HTTP_CHECKED}}", scrypted_source == "http" ? "checked" : "");
        page.replace("{{SCRYPTED_SOURCE_RTSP_CHECKED}}", scrypted_source == "rtsp" ? "checked" : "");
        page.replace("{{SCRYPTED_LOW_LATENCY_CHECKED}}", scrypted_low_latency ? "checked" : "");
        page.replace("{{SCRYPTED_LOW_BUFFER_CHECKED}}", scrypted_low_buffer ? "checked" : "");
        page.replace("{{SCRYPTED_RTSP_UDP_CHECKED}}", scrypted_rtsp_udp ? "checked" : "");
        page.replace("{{MIC_ENABLED_CHECKED}}", mic_enabled ? "checked" : "");
        page.replace("{{MIC_MUTED_CHECKED}}", mic_muted ? "checked" : "");
        page.replace("{{MIC_SENSITIVITY}}", String(mic_sensitivity));
        page.replace("{{AUDIO_OUT_ENABLED_CHECKED}}", audio_out_enabled ? "checked" : "");
        page.replace("{{AUDIO_OUT_MUTED_CHECKED}}", audio_out_muted ? "checked" : "");
        page.replace("{{AUDIO_OUT_VOLUME}}", String(audio_out_volume));
        page.replace("{{SCRYPTED_WEBHOOK}}", scrypted_webhook);
        page.replace("{{LOCAL_IP}}", WiFi.localIP().toString());
        page.replace("{{TIMEZONE}}", timezone);
        page.replace("{{FW_VERSION}}", String(FW_VERSION));
        page.replace("{{FW_BUILD_TIME}}", String(FW_BUILD_TIME));

        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
      });

      server.on("/tr064", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("tr064", true);
        String tr064_user = prefs.getString("tr064_user", "");
        String tr064_pass = prefs.getString("tr064_pass", "");
        String number = prefs.getString("number", "");
        prefs.end();

        String page = loadUiTemplate("/tr064.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        page.replace("{{GATEWAY_IP}}", WiFi.gatewayIP().toString());
        page.replace("{{TR064_USER}}", tr064_user);
        page.replace("{{TR064_PASS}}", tr064_pass);
        page.replace("{{TR064_NUMBER}}", number);
        request->send(200, "text/html", page);
      });

      server.on("/sip", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("sip", true);
        String sip_user = prefs.getString("sip_user", "");
        String sip_password = prefs.getString("sip_password", "");
        String sip_displayname = prefs.getString("sip_displayname", "Doorbell");
        String sip_target = prefs.getString("sip_target", "**11");
        prefs.end();

        String page = loadUiTemplate("/sip.html");
        if (page.isEmpty()) {
          request->send(500, "text/plain", "UI template missing");
          return;
        }
        page.replace("{{GATEWAY_IP}}", WiFi.gatewayIP().toString());
        page.replace("{{SIP_USER}}", sip_user);
        page.replace("{{SIP_PASS}}", sip_password);
        page.replace("{{SIP_DISPLAY}}", sip_displayname);
        page.replace("{{SIP_TARGET}}", sip_target);
        request->send(200, "text/html", page);
      });

      server.on("/sipDebug", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("sip", true);
        String sip_user = prefs.getString("sip_user", "");
        String sip_target = prefs.getString("sip_target", "");
        prefs.end();

        String json = "{";
        json += "\"sip_user\":\"" + sip_user + "\",";
        json += "\"sip_target\":\"" + sip_target + "\",";
        json += "\"has_sip_config\":" + String(hasSipConfig(sipConfig) ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
      });

      server.on("/saveFeatures", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
          String receivedData = String((char *)data).substring(0, len);
          logEvent(LOG_INFO, "📥 Received Feature Toggles: " + receivedData);

          JsonDocument doc;
          DeserializationError error = deserializeJson(doc, receivedData);
          if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
          }
          String tz = doc["timezone"].isNull()
            ? timezone
            : doc["timezone"].as<String>();
          bool sip_enabled = doc["sip_enabled"].as<bool>();
          bool tr064_enabled = doc["tr064_enabled"].as<bool>();
          bool http_cam_enabled = doc["http_cam_enabled"].as<bool>();
          bool rtsp_enabled = doc["rtsp_enabled"].as<bool>();
          uint8_t http_cam_max_clients = doc["http_cam_max_clients"].isNull()
            ? httpCamMaxClients
            : (uint8_t)doc["http_cam_max_clients"].as<int>();
          if (http_cam_max_clients < 1) {
            http_cam_max_clients = 1;
          } else if (http_cam_max_clients > 4) {
            http_cam_max_clients = 4;
          }
          String scrypted_source = doc["scrypted_source"].isNull()
            ? scryptedSource
            : doc["scrypted_source"].as<String>();
          String scrypted_webhook = doc["scrypted_webhook"].isNull()
            ? scryptedWebhook
            : doc["scrypted_webhook"].as<String>();
          scrypted_webhook.trim();
          bool scrypted_low_latency = doc["scrypted_low_latency"].isNull()
            ? scryptedLowLatency
            : doc["scrypted_low_latency"].as<bool>();
          bool scrypted_low_buffer = doc["scrypted_low_buffer"].isNull()
            ? scryptedLowBuffer
            : doc["scrypted_low_buffer"].as<bool>();
          bool scrypted_rtsp_udp = doc["scrypted_rtsp_udp"].isNull()
            ? scryptedRtspUdp
            : doc["scrypted_rtsp_udp"].as<bool>();
          bool mic_enabled = doc["mic_enabled"].isNull()
            ? micEnabled
            : doc["mic_enabled"].as<bool>();
          bool mic_muted = doc["mic_muted"].isNull()
            ? micMuted
            : doc["mic_muted"].as<bool>();
          int mic_sensitivity = doc["mic_sensitivity"].isNull()
            ? micSensitivity
            : doc["mic_sensitivity"].as<int>();
          bool audio_out_enabled = doc["audio_out_enabled"].isNull()
            ? audioOutEnabled
            : doc["audio_out_enabled"].as<bool>();
          bool audio_out_muted = doc["audio_out_muted"].isNull()
            ? audioOutMuted
            : doc["audio_out_muted"].as<bool>();
          int audio_out_volume = doc["audio_out_volume"].isNull()
            ? audioOutVolume
            : doc["audio_out_volume"].as<int>();
          if (scrypted_source != "http" && scrypted_source != "rtsp") {
            scrypted_source = "http";
          }
          if (scrypted_source == "rtsp" && !rtsp_enabled) {
            request->send(400, "text/plain", "RTSP source requires RTSP streaming enabled");
            return;
          }
          if (scrypted_source == "http" && !http_cam_enabled) {
            request->send(400, "text/plain", "HTTP source requires HTTP streaming enabled");
            return;
          }
          if (mic_sensitivity > 100) {
            mic_sensitivity = 100;
          }
          if (mic_sensitivity < 0) {
            mic_sensitivity = 0;
          }
          if (audio_out_volume > 100) {
            audio_out_volume = 100;
          }
          if (audio_out_volume < 0) {
            audio_out_volume = 0;
          }

          Preferences prefs;
          prefs.begin("features", false);
          prefs.putBool(kFeatSipEnabledKey, sip_enabled);
          prefs.putBool(kFeatTr064EnabledKey, tr064_enabled);
          prefs.putBool(kFeatHttpCamEnabledKey, http_cam_enabled);
          prefs.putBool(kFeatRtspEnabledKey, rtsp_enabled);
          prefs.putUChar(kFeatHttpCamMaxClientsKey, http_cam_max_clients);
          prefs.putString(kFeatScryptedSourceKey, scrypted_source);
          prefs.putString(kFeatScryptedWebhookKey, scrypted_webhook);
          prefs.putBool(kFeatScryptedLowLatencyKey, scrypted_low_latency);
          prefs.putBool(kFeatScryptedLowBufferKey, scrypted_low_buffer);
          prefs.putBool(kFeatScryptedRtspUdpKey, scrypted_rtsp_udp);
          prefs.putBool(kFeatMicEnabledKey, mic_enabled);
          prefs.putBool(kFeatMicMutedKey, mic_muted);
          prefs.putUChar(kFeatMicSensitivityKey, static_cast<uint8_t>(mic_sensitivity));
          prefs.putBool(kFeatAudioOutEnabledKey, audio_out_enabled);
          prefs.putBool(kFeatAudioOutMutedKey, audio_out_muted);
          prefs.putUChar(kFeatAudioOutVolumeKey, static_cast<uint8_t>(audio_out_volume));
          prefs.putString(kFeatTimezoneKey, tz);
          prefs.end();

          // Update global variables
          sipEnabled = sip_enabled;
          tr064Enabled = tr064_enabled;
          httpCamEnabled = http_cam_enabled;
          rtspEnabled = rtsp_enabled;
          httpCamMaxClients = http_cam_max_clients;
          scryptedSource = scrypted_source;
          scryptedWebhook = scrypted_webhook;
          scryptedLowLatency = scrypted_low_latency;
          scryptedLowBuffer = scrypted_low_buffer;
          scryptedRtspUdp = scrypted_rtsp_udp;
          micEnabled = mic_enabled;
          micMuted = mic_muted;
          micSensitivity = static_cast<uint8_t>(mic_sensitivity);
          audioOutEnabled = audio_out_enabled;
          audioOutMuted = audio_out_muted;
          audioOutVolume = static_cast<uint8_t>(audio_out_volume);
          timezone = tz;
          #ifdef CAMERA
          setCameraStreamMaxClients(httpCamMaxClients);
          setRtspAllowUdp(scryptedRtspUdp);
          #endif
          configureAudio(micEnabled, micMuted, micSensitivity, audioOutEnabled, audioOutMuted, audioOutVolume);

          String summary = "Feature settings saved: timezone=" + tz +
                           " SIP=" + String(sip_enabled ? "on" : "off") +
                           " TR-064=" + String(tr064_enabled ? "on" : "off") +
                           " HTTP=" + String(http_cam_enabled ? "on" : "off") +
                           " RTSP=" + String(rtsp_enabled ? "on" : "off") +
                           " HTTP max clients=" + String(http_cam_max_clients) +
                           " Scrypted source=" + scrypted_source +
                           " webhook=" + String(scrypted_webhook.isEmpty() ? "empty" : "set") +
                           " low-latency=" + String(scrypted_low_latency ? "on" : "off") +
                           " low-buffer=" + String(scrypted_low_buffer ? "on" : "off") +
                           " rtsp-udp=" + String(scrypted_rtsp_udp ? "on" : "off") +
                           " mic=" + String(mic_enabled ? "on" : "off") +
                           " mic-mute=" + String(mic_muted ? "on" : "off") +
                           " mic-sens=" + String(mic_sensitivity) +
                           " audio-out=" + String(audio_out_enabled ? "on" : "off") +
                           " audio-mute=" + String(audio_out_muted ? "on" : "off") +
                           " audio-vol=" + String(audio_out_volume) +
                           ". Restarting ESP32...";
          logEvent(LOG_INFO, "✅ " + summary);
          request->send(200, "text/plain", summary);
          
          // Restart ESP32 to apply changes
          delay(1000);
          ESP.restart();
        }
      );

      server.on("/saveSIP", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
          String receivedData = String((char *)data).substring(0, len);
          logEvent(LOG_INFO, "📥 Received SIP Config: " + receivedData);

          JsonDocument doc;
          DeserializationError error = deserializeJson(doc, receivedData);
          if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
          }

          String sip_user = doc["sip_user"].as<String>();
          String sip_password = doc["sip_password"].as<String>();
          String sip_displayname = doc["sip_displayname"].as<String>();
          String sip_target = doc["sip_target"].as<String>();

          Preferences prefs;
          prefs.begin("sip", false);
          if (sip_user.isEmpty() && sip_password.isEmpty()) {
            prefs.remove("sip_user");
            prefs.remove("sip_password");
            prefs.remove("sip_displayname");
            prefs.remove("sip_target");
            prefs.end();
            request->send(200, "text/plain", "SIP settings cleared");
            return;
          }

          prefs.putString("sip_user", sip_user);
          prefs.putString("sip_password", sip_password);
          prefs.putString("sip_displayname", sip_displayname);
          prefs.putString("sip_target", sip_target);
          prefs.end();

          // Reload config into global sipConfig
          loadSipConfig(sipConfig);

          request->send(200, "text/plain", "SIP settings saved");
        }
      );

      server.on("/tr064Debug", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("tr064", true);
        String tr064_user = prefs.getString("tr064_user", "");
        String number = prefs.getString("number", "");
        prefs.end();
        String gateway = WiFi.gatewayIP().toString();

        String json = "{";
        json += "\"gateway\":\"" + gateway + "\",";
        json += "\"tr064_user\":\"" + tr064_user + "\",";
        json += "\"number\":\"" + number + "\",";
        json += "\"has_tr064_config\":" + String(hasTr064Config(tr064Config) ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
      });

      server.on("/saveTR064", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
          String receivedData = String((char *)data).substring(0, len);
          logEvent(LOG_INFO, "📥 Received TR-064 Config: " + receivedData);

          JsonDocument doc;
          DeserializationError error = deserializeJson(doc, receivedData);
          if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
          }

          String tr064_user = doc["tr064_user"].as<String>();
          String tr064_pass = doc["tr064_pass"].as<String>();
          String number = doc["number"].as<String>();

          Preferences prefs;
          prefs.begin("tr064", false);
          if (number.isEmpty() && tr064_pass.isEmpty()) {
            prefs.remove("tr064_user");
            prefs.remove("tr064_pass");
            prefs.remove("number");
            prefs.end();
            request->send(200, "text/plain", "TR-064 settings cleared");
            return;
          }

          prefs.putString("tr064_user", tr064_user);
          prefs.putString("tr064_pass", tr064_pass);
          prefs.putString("number", number);
          prefs.end();

          // Reload config into global tr064Config
          loadTr064Config(tr064Config);

          request->send(200, "text/plain", "TR-064 settings saved");
        }
      );

      server.on("/deviceStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"rssi\":" + String(WiFi.RSSI()) + ",\"uptimeSeconds\":" + String(millis() / 1000) + "}";
        request->send(200, "application/json", json);
      });
      #ifdef CAMERA
      server.on("/cameraStreamInfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        bool running = isCameraStreamServerRunning();
        String clientIp = "";
        uint32_t clients = 0;
        uint32_t connectedMs = 0;
        uint32_t lastFrameAgeMs = 0;
        String clientsJson = "[]";
        bool active = getCameraStreamClientInfo(clientIp, clients, connectedMs, lastFrameAgeMs, clientsJson);
        uint32_t audioClients = getCameraStreamAudioClientCount();

        // Get RTSP session count
        int rtspSessions = getRtspActiveSessionCount();

        String json = "{";
        json += "\"running\":" + String(running ? "true" : "false") + ",";
        json += "\"active\":" + String(active ? "true" : "false") + ",";
        json += "\"clients\":" + String(clients) + ",";
        json += "\"audio_clients\":" + String(audioClients) + ",";
        json += "\"client_ip\":\"" + clientIp + "\",";
        json += "\"connected_ms\":" + String(connectedMs) + ",";
        json += "\"last_frame_age_ms\":" + String(lastFrameAgeMs) + ",";
        json += "\"clients_list\":" + clientsJson + ",";
        json += "\"rtsp_sessions\":" + String(rtspSessions) + ",";
        json += "\"rtsp_udp_endpacket_fail\":" + String(getRtspUdpEndPacketFailCount()) + ",";
        json += "\"rtsp_udp_backoff_ms\":" + String(getRtspUdpBackoffRemainingMs());
        json += "}";
        request->send(200, "application/json", json);
      });
      #endif
      server.on("/eventLog", HTTP_GET, [](AsyncWebServerRequest *request) {
        uint32_t sinceId = 0;
        if (request->hasParam("since")) {
          sinceId = request->getParam("since")->value().toInt();
        }
        String json = getEventLogJson(sinceId);
        request->send(200, "application/json", json);
      });
      server.on("/clearLog", HTTP_POST, [](AsyncWebServerRequest *request) {
        clearEventLog();
        request->send(200, "text/plain", "Log cleared");
      });

      server.on("/resetRtspUdpFails", HTTP_POST, [](AsyncWebServerRequest *request) {
        resetRtspUdpEndPacketFailCount();
        request->send(200, "text/plain", "RTSP UDP fail count reset");
      });
      server.on("/resetRtspUdpBackoff", HTTP_POST, [](AsyncWebServerRequest *request) {
        resetRtspUdpBackoffState();
        request->send(200, "text/plain", "RTSP UDP backoff cleared");
      });

      server.on("/ring", HTTP_GET, [](AsyncWebServerRequest *request) {
        markRingLed();
        if (sipEnabled) {
          if (queueSipRing("/ring")) {
            request->send(200, "text/plain", "SIP ring queued");
          } else {
            request->send(200, "text/plain", "SIP ring already queued");
          }
          return;
        }
        if (!tr064Enabled) {
          request->send(503, "text/plain", "Ring failed (TR-064 disabled)");
          return;
        }
        if (triggerTr064Ring(tr064Config)) {
          request->send(200, "text/plain", "Ring triggered via TR-064");
        } else {
          request->send(500, "text/plain", "Ring failed (TR-064 failed)");
        }
      });

      server.on("/ring/tr064", HTTP_GET, [](AsyncWebServerRequest *request) {
        markRingLed();
        if (!tr064Enabled) {
          request->send(503, "text/plain", "TR-064 ring disabled");
          return;
        }
        if (triggerTr064Ring(tr064Config)) {
          request->send(200, "text/plain", "TR-064 ring triggered");
        } else {
          request->send(500, "text/plain", "TR-064 ring failed");
        }
      });

      server.on("/ring/sip", HTTP_GET, [](AsyncWebServerRequest *request) {
        markRingLed();
        if (!sipEnabled) {
          request->send(503, "text/plain", "SIP ring disabled");
          return;
        }
        if (queueSipRing("/ring/sip")) {
          request->send(200, "text/plain", "SIP ring queued");
        } else {
          request->send(200, "text/plain", "SIP ring already queued");
        }
      });

      server.on("/ring/homekit", HTTP_GET, [](AsyncWebServerRequest *request) {
        triggerDoorbellEvent(false);
        if (scryptedWebhook.isEmpty()) {
          request->send(200, "text/plain", "HomeKit test triggered (webhook not set)");
          return;
        }
        request->send(200, "text/plain", "HomeKit gong triggered (no SIP)");
      });

      server.on("/forget", HTTP_GET, [](AsyncWebServerRequest *request) {
        // Clear WiFi credentials and reboot into provisioning mode.
        preferences.begin("wifi", false);
        preferences.remove("ssid");
        preferences.remove("password");
        preferences.end();
        
        request->send(200, "text/plain", "WiFi credentials cleared. Restarting...");
        delay(2000);
        ESP.restart();
      });

      server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
      });

      otaUpdate.begin(server);
      server.begin();
      logEvent(LOG_INFO, "✅ Web server started");
    } else {
      logEvent(LOG_ERROR, "❌ WiFi connection failed. Starting AP mode...");
      startAPMode(server, dnsServer, preferences);
    }
  }

  #ifdef CAMERA
  // Start MJPEG streaming server on port 81.
  if (cameraReady) {
    if (httpCamEnabled) {
      startCameraStreamServer();
      logEvent(LOG_INFO, "✅ HTTP camera streaming enabled");
    } else {
      logEvent(LOG_INFO, "ℹ️ HTTP camera streaming disabled");
    }
    
    if (rtspEnabled) {
      logEvent(LOG_INFO, "📞 Calling startRtspServer()...");
      bool rtspStarted = startRtspServer();
      if (rtspStarted) {
        logEvent(LOG_INFO, "✅ RTSP streaming enabled");
      } else {
        logEvent(LOG_ERROR, "❌ RTSP server failed to start");
      }
    } else {
      logEvent(LOG_INFO, "ℹ️ RTSP streaming disabled");
    }
  }
  #endif

  logEvent(LOG_INFO, "====================================");
  logEvent(LOG_INFO, "Setup complete!");
}

bool isDoorbellPressed() {
  // Normalize GPIO to a logical pressed state.
  int state = digitalRead(DOORBELL_BUTTON_PIN);
  if (DOORBELL_BUTTON_ACTIVE_LOW) {
    return state == LOW;
  }
  return state == HIGH;
}

void handleDoorbellPress() {
  triggerDoorbellEvent(true);
}

void loop() {
  // Periodic memory health logging (every 5 minutes)
  static unsigned long lastHeapLog = 0;
  unsigned long now = millis();
  if (now - lastHeapLog > 300000) {  // 5 minutes
    size_t freeHeap = ESP.getFreeHeap();
    size_t minHeap = ESP.getMinFreeHeap();
    size_t heapSize = ESP.getHeapSize();
    logEvent(LOG_INFO, "💾 Heap: " + String(freeHeap) + " bytes free (" + String(freeHeap * 100 / heapSize) + "%), min: " + String(minHeap));
    lastHeapLog = now;
  }
  
  // Handle DNS requests in AP mode
  if (isAPModeActive()) {
    dnsServer.processNextRequest();
  }
  updateStatusLed();

  #ifdef CAMERA
  if (cameraReady) {
    if (httpCamEnabled) {
      if (!isCameraStreamServerRunning()) {
        startCameraStreamServer();
      }
    } else if (isCameraStreamServerRunning()) {
      stopCameraStreamServer();
      logEvent(LOG_INFO, "ℹ️ HTTP camera streaming disabled");
    }
  }
  #endif
  
  // Handle RTSP client connections for Scrypted (non-blocking)
  if (rtspEnabled) {
    if (!isRtspTaskRunning()) {
      // Fallback when RTSP task isn't running (e.g., task creation failed).
      handleRtspClient();
    }
  } else if (isRtspServerRunning()) {
    stopRtspServer();
    logEvent(LOG_INFO, "ℹ️ RTSP disabled - server stopped");
  }
  
  // Send periodic SIP REGISTER to maintain registration with FRITZ!Box (only if enabled)
  if (sipEnabled) {
    if (sipRingQueued && !isSipRingActive()) {
      sipRingQueued = false;
      bool ringOk = triggerSipRing(sipConfig);
      if (ringOk) {
        logEvent(LOG_INFO, "📞 FRITZ!Box ring started via SIP");
      } else {
        logEvent(LOG_WARN, "⚠️ SIP ring failed - check SIP configuration");
      }
    }

    handleSipIncoming();
    processSipRing();
    if (!isSipRingActive()) {
      sendRegisterIfNeeded(sipConfig);
    }
  }
  
  // Simple debounce: detect stable press, then wait for release.
  bool pressed = isDoorbellPressed();
  unsigned long nowMs = millis();
  if (pressed != lastButtonPressed) {
    lastButtonChangeMs = nowMs;
    lastButtonPressed = pressed;
  }

  if (pressed && !doorbellLatched && (nowMs - lastButtonChangeMs) > DOORBELL_DEBOUNCE_MS) {
    handleDoorbellPress();
    doorbellLatched = true;
  } else if (!pressed && doorbellLatched && (nowMs - lastButtonChangeMs) > DOORBELL_DEBOUNCE_MS) {
    doorbellLatched = false;
  }

  otaUpdate.loop();
  delay(10);
}
