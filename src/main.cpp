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

// Global objects shared across setup/loop.
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;

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

// Feature toggles (loaded once at boot, updated on save)
bool sipEnabled = true;
bool tr064Enabled = true;
bool httpCamEnabled = true;
bool rtspEnabled = true;
uint8_t httpCamMaxClients = 2;
String scryptedSource = "http";
bool scryptedLowLatency = true;
bool scryptedLowBuffer = true;
bool scryptedRtspUdp = false;
bool micEnabled = false;
bool micMuted = false;
uint8_t micSensitivity = DEFAULT_MIC_SENSITIVITY;
bool audioOutEnabled = true;
bool audioOutMuted = false;
uint8_t audioOutVolume = DEFAULT_AUDIO_OUT_VOLUME;

bool lastButtonPressed = false;
unsigned long lastButtonChangeMs = 0;

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

  ensureBoolPref("sip_enabled", true);
  ensureBoolPref("tr064_enabled", true);
  ensureBoolPref("http_cam_enabled", true);
  ensureBoolPref("rtsp_enabled", true);
  ensureUCharPref("http_cam_max_clients", 2);
  ensureBoolPref("scrypted_low_latency", true);
  ensureBoolPref("scrypted_low_buffer", true);
  ensureBoolPref("scrypted_rtsp_udp", false);
  ensureBoolPref(kFeatMicEnabledKey, false);
  ensureBoolPref(kFeatMicMutedKey, false);
  ensureUCharPref(kFeatMicSensitivityKey, DEFAULT_MIC_SENSITIVITY);
  ensureBoolPref(kFeatAudioOutEnabledKey, true);
  ensureBoolPref(kFeatAudioOutMutedKey, false);
  ensureUCharPref(kFeatAudioOutVolumeKey, DEFAULT_AUDIO_OUT_VOLUME);

  bool httpEnabledPref = featPrefs.getBool("http_cam_enabled", true);
  bool rtspEnabledPref = featPrefs.getBool("rtsp_enabled", true);
  if (!featPrefs.isKey("scrypted_source")) {
    String defaultSource = httpEnabledPref ? "http" : (rtspEnabledPref ? "rtsp" : "http");
    ensureStringPref("scrypted_source", defaultSource);
  }

  if (defaultsApplied) {
    logEvent(LOG_INFO, "✅ Feature defaults applied for missing keys");
  }
  // Load feature flags into global variables
  sipEnabled = featPrefs.getBool("sip_enabled", true);
  tr064Enabled = featPrefs.getBool("tr064_enabled", true);
  httpCamEnabled = featPrefs.getBool("http_cam_enabled", true);
  rtspEnabled = featPrefs.getBool("rtsp_enabled", true);
  httpCamMaxClients = featPrefs.getUChar("http_cam_max_clients", 2);
  scryptedSource = featPrefs.getString("scrypted_source", httpCamEnabled ? "http" : "rtsp");
  scryptedLowLatency = featPrefs.getBool("scrypted_low_latency", true);
  scryptedLowBuffer = featPrefs.getBool("scrypted_low_buffer", true);
  scryptedRtspUdp = featPrefs.getBool("scrypted_rtsp_udp", false);
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
          streamInfo += "<div class=\\\"flash-stats\\\">"
                        "<strong>📹 HTTP Stream:</strong>"
                        "<p style=\\\"font-family: monospace; font-size: 0.9em; word-break: break-all;\\\">"
                        "http://" + WiFi.localIP().toString() + ":81/stream"
                        "</p>"
                        "<p style=\\\"font-size: 0.85em; color: #666;\\\">"
                        "Use this URL for MJPEG clients (Scrypted HTTP camera, FRITZ!Box)."
                        "</p>"
                        "</div>";
        } else {
          streamInfo += "<div class=\\\"flash-stats\\\">"
                        "<strong>📹 HTTP Stream:</strong> disabled"
                        "</div>";
        }
        if (rtspEnabled) {
          streamInfo += "<div class=\\\"flash-stats\\\">"
                        "<strong>📡 RTSP Stream (for Scrypted):</strong>"
                        "<p style=\\\"font-family: monospace; font-size: 0.9em; word-break: break-all;\\\">"
                        "rtsp://" + WiFi.localIP().toString() + ":8554/mjpeg/1"
                        "</p>"
                        "<p style=\\\"font-size: 0.85em; color: #666;\\\">"
                        "Use the RTSP Camera plugin or prefix with <code>-i</code> if using FFmpeg Camera."
                        "</p>"
                        "</div>";
        } else {
          streamInfo += "<div class=\\\"flash-stats\\\">"
                        "<strong>📡 RTSP Stream:</strong> disabled"
                        "</div>";
        }

        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Doorbell</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
</head>
<body class="full-width-page">
    <div class="dark-mode-toggle">
        <label class="switch">
            <input type="checkbox" id="darkModeToggle">
            <span class="slider"></span>
        </label>
        <span>Dark Mode</span>
    </div>

    <div class="header-container">
        <h1 class="main-title">🔔 ESP32-S3 Doorbell</h1>
    </div>

    <div class="dashboard">
        <!-- System Information Card -->
        <div class="container card">
            <h3>📊 System Information</h3>
            <div class="info-grid">
                <div class="info-item">
                    <span class="info-label">Status:</span>
                    <span class="info-value status-online">● Online</span>
                </div>
                <div class="info-item">
                    <span class="info-label">WiFi:</span>
                    <span class="info-value">Connected</span>
                </div>
                <div class="info-item">
                    <span class="info-label">IP Address:</span>
                    <span class="info-value">)rawliteral" + WiFi.localIP().toString() + R"rawliteral(</span>
                </div>
                <div class="info-item">
                    <span class="info-label">Signal Strength:</span>
                    <span class="info-value"><span id="rssi">--</span> dBm</span>
                </div>
                <div class="info-item">
                    <span class="info-label">System Uptime:</span>
                    <span class="info-value" id="uptime">--</span>
                </div>
                <div class="info-item">
                    <span class="info-label">WiFi Connected:</span>
                    <span class="info-value" id="wifiConnected">--</span>
                </div>
            </div>
            <hr>
            <button class="danger-btn" onclick="location.href='/forget';">Forget WiFi</button>
            <div class="button-row" style="margin-top: 12px;">
                <a class="button" href="/setup">⚙️ Feature Setup</a>
                <button class="button danger-btn" onclick="location.href='/restart';">🔄 Restart</button>
            </div>
        </div>

        <!-- Camera Settings Card -->
        <div class="container card">
            <h3>📷 Camera Settings</h3>
            <div class="flash-stats" id="cameraStatus">
                <strong>Camera:</strong> loading...
            </div>
            <div class="flash-stats" id="streamStatus">
                <strong>Stream:</strong> loading...
            </div>
            
            <hr>
            <div class="button-row">
                <a class="button config-btn" href="/capture" target="_blank">📸 Snapshot</a>
                <a class="button" href="http://)rawliteral" + WiFi.localIP().toString() + R"rawliteral(:81/stream" target="_blank">🎥 Live Stream</a>
            </div>
            <hr>
            )rawliteral" + streamInfo + R"rawliteral(
            <details class="flash-stats">
                <summary><strong>📺 Scrypted Setup (Quick Steps)</strong></summary>
                <ol style="margin: 8px 0 0 16px; padding: 0; font-size: 0.9em;">
                    <li>Add a camera in Scrypted.</li>
                    <li>Use <strong>HTTP Camera</strong> with <code>http://ESP32-IP:81/stream</code>, or <strong>RTSP Camera</strong> with <code>rtsp://ESP32-IP:8554/mjpeg/1</code>.</li>
                    <li>If using FFmpeg Camera for HomeKit, set Output Prefix to: <code>-c:v libx264 -pix_fmt yuvj420p -preset ultrafast -bf 0 -g 60 -r 15 -b:v 500000 -bufsize 1000000 -maxrate 500000</code></li>
                </ol>
            </details>
        </div>

        <!-- SIP/TR-064 Card -->
        <div class="container card">
            <h3>☎️ SIP/TR-064</h3>
            <p>FRITZ!Box SIP/TR-064 integration for doorbell notifications.</p>
            <div class="flash-stats" id="tr064Status">
                <strong>Status:</strong> loading...
            </div>
            <hr>
            <div class="button-row">
                <button class="button" onclick="testRingSip()">🔔 Test Ring (SIP)</button>
                <a class="button config-btn" href="/sip">⚙️ Setup</a>
            </div>
            <a class="button" href="/sipDebug" target="_blank">🐛 Debug JSON</a>
        </div>
    </div>

    <div class="logs-grid">
        <div>
            <h3 style="text-align: center; margin-bottom: 10px;">📹 Camera Logs</h3>
            <div class="log-container log-size-md" id="logCamera"></div>
        </div>
        <div>
            <h3 style="text-align: center; margin-bottom: 10px;">☎️ Doorbell Logs</h3>
            <div class="log-container log-size-md" id="logDoorbell"></div>
        </div>
    </div>
    
    <div class="button-row log-actions">
        <label style="display: flex; align-items: center; gap: 8px;">
            <span>Log font size</span>
            <select id="logFontSize">
                <option value="sm">Small</option>
                <option value="md" selected>Medium</option>
                <option value="lg">Large</option>
            </select>
        </label>
        <button class="button danger-btn" onclick="clearLog()">🧹 Clear Log</button>
        <button class="button" onclick="copyLog()">📋 Copy Log to Clipboard</button>
    </div>

    <script>
        const logCameraEl = document.getElementById("logCamera");
        const logDoorbellEl = document.getElementById("logDoorbell");
        const logFontSizeEl = document.getElementById("logFontSize");
        const statusEl = document.getElementById("cameraStatus");
        const trStatusEl = document.getElementById("tr064Status");
        const darkToggle = document.getElementById("darkModeToggle");
        const rssiEl = document.getElementById("rssi");
        const uptimeEl = document.getElementById("uptime");
        const wifiConnectedEl = document.getElementById("wifiConnected");
        const streamStatusEl = document.getElementById("streamStatus");
        const wifiConnectTime = Date.now();
        let lastEventId = 0;

        // Categorize log messages by keywords
        function isCameraLog(message) {
            const cameraKeywords = ["Camera", "RTSP", "HTTP", "Stream", "JPEG", "📹", "🎥", "📡", "🔌", "▶️", "🛑", "📴", "⏱️"];
            return cameraKeywords.some(kw => message.includes(kw));
        }

        function isDoorbellLog(message) {
            const doorbellKeywords = ["SIP", "TR-064", "FRITZ", "Ring", "Doorbell", "☎️", "📞", "🔔", "🔐"];
            return doorbellKeywords.some(kw => message.includes(kw));
        }

        function addLog(message, level = "info") {
            const entry = document.createElement("div");
            entry.className = `log-entry ${level}`;
            const ts = new Date().toLocaleTimeString();
            entry.textContent = `[${ts}] ${message}`;
            
            // Route to appropriate log container
            if (isCameraLog(message)) {
                logCameraEl.prepend(entry.cloneNode(true));
            } else if (isDoorbellLog(message)) {
                logDoorbellEl.prepend(entry.cloneNode(true));
            } else {
                // System/general logs go to both
                logCameraEl.prepend(entry.cloneNode(true));
                logDoorbellEl.prepend(entry);
            }
        }

        function applyLogFontSize(size) {
            const sizes = ["log-size-sm", "log-size-md", "log-size-lg"];
            sizes.forEach(cls => {
                logCameraEl.classList.remove(cls);
                logDoorbellEl.classList.remove(cls);
            });
            const cls = `log-size-${size}`;
            logCameraEl.classList.add(cls);
            logDoorbellEl.classList.add(cls);
            localStorage.setItem("logFontSize", size);
        }

        function clearLog() {
            fetch("/clearLog", { method: "POST" })
                .then(r => r.text())
                .then(() => {
                    // Reset both the UI list and the event log cursor so new entries show up again.
                    logCameraEl.innerHTML = "";
                    logDoorbellEl.innerHTML = "";
                    lastEventId = 0;
                })
                .catch(() => addLog("Failed to clear log", "error"));
        }

        function copyLog() {
            fetch("/eventLog?since=0")
                .then(r => r.json())
                .then(data => {
                    const entries = (data.entries || []).map(entry => {
                        const level = (entry.level || "info").toUpperCase();
                        return `[${entry.id}] [${level}] ${entry.message}`;
                    });
                    const text = entries.join("\n");
                    if (!text.length) {
                        addLog("Log is empty, nothing to copy", "warn");
                        return;
                    }
                    return navigator.clipboard.writeText(text)
                        .then(() => addLog("Log copied to clipboard", "info"))
                        .catch(() => addLog("Clipboard copy failed", "error"));
                })
                .catch(() => addLog("Log fetch failed for clipboard copy", "error"));
        }

        function setDarkMode(enabled) {
            document.body.classList.toggle("dark-mode", enabled);
            localStorage.setItem("darkMode", enabled ? "enabled" : "disabled");
        }

        // Initialize dark mode from localStorage (default: enabled)
        const savedDarkMode = localStorage.getItem("darkMode");
        const darkModeEnabled = savedDarkMode === null ? true : savedDarkMode === "enabled";
        darkToggle.checked = darkModeEnabled;
        setDarkMode(darkModeEnabled);

        darkToggle.addEventListener("change", () => {
            setDarkMode(darkToggle.checked);
            addLog(`Dark mode ${darkToggle.checked ? "enabled" : "disabled"}`);
        });

        function fetchStatus() {
            fetch("/status")
                .then(r => r.json())
                .then(data => {
                    statusEl.innerHTML = `<strong>Camera:</strong> ${data.PID || "Unknown"} | size=${data.framesize} | quality=${data.quality}`;
                })
                .catch(() => {
                    statusEl.innerHTML = "<strong>Camera:</strong> unavailable";
                    addLog("Camera status unavailable", "warn");
                });
        }

        function formatMs(ms) {
            if (!ms || ms < 0) return "0s";
            const sec = Math.floor(ms / 1000);
            if (sec < 60) return `${sec}s`;
            const min = Math.floor(sec / 60);
            const rem = sec % 60;
            return `${min}m ${rem}s`;
        }

        function fetchStreamInfo() {
            fetch("/cameraStreamInfo")
                .then(r => r.json())
                .then(data => {
                    if (!data.running) {
                        streamStatusEl.innerHTML = "<strong>Stream:</strong> server stopped";
                        return;
                    }
                    
                    // Build HTTP stream status
                    let httpStatus = "";
                    if (!data.active) {
                        httpStatus = "HTTP: no clients";
                    } else {
                        const ip = data.client_ip || "unknown";
                        const clients = data.clients || 0;
                        const connectedFor = formatMs(data.connected_ms);
                        const lastFrame = formatMs(data.last_frame_age_ms);
                        const list = Array.isArray(data.clients_list) ? data.clients_list.join(", ") : "";
                        const listText = list.length ? ` | ${list}` : "";
                        httpStatus = `HTTP: ${clients} client(s)${listText} | connected ${connectedFor} | last frame ${lastFrame} ago`;
                    }
                    
                    // Build RTSP stream status
                    const rtspSessions = data.rtsp_sessions || 0;
                    const rtspStatus = rtspSessions > 0 
                        ? `RTSP: ${rtspSessions} session(s)` 
                        : "RTSP: no sessions";
                    
                    streamStatusEl.innerHTML = `<strong>Stream:</strong> ${httpStatus}<br>${rtspStatus}`;
                })
                .catch(() => {
                    streamStatusEl.innerHTML = "<strong>Stream:</strong> unavailable";
                });
        }

        function fetchEventLog() {
            fetch(`/eventLog?since=${lastEventId}`)
                .then(r => r.json())
                .then(data => {
                    if (!data.entries) return;
                    data.entries.forEach(entry => {
                        addLog(entry.message, entry.level || "info");
                        if (entry.id > lastEventId) {
                            lastEventId = entry.id;
                        }
                    });
                })
                .catch(() => addLog("Event log fetch failed", "warn"));
        }

        function formatUptime(seconds) {
            const days = Math.floor(seconds / 86400);
            const hours = Math.floor((seconds % 86400) / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            const secs = seconds % 60;
            
            if (days > 0) {
                return `${days}d ${hours}h ${minutes}m`;
            } else if (hours > 0) {
                return `${hours}h ${minutes}m ${secs}s`;
            } else if (minutes > 0) {
                return `${minutes}m ${secs}s`;
            } else {
                return `${secs}s`;
            }
        }

        function formatDateTime(timestamp) {
            const date = new Date(timestamp);
            const day = String(date.getDate()).padStart(2, '0');
            const month = String(date.getMonth() + 1).padStart(2, '0');
            const year = date.getFullYear();
            const hours = String(date.getHours()).padStart(2, '0');
            const minutes = String(date.getMinutes()).padStart(2, '0');
            const seconds = String(date.getSeconds()).padStart(2, '0');
            return `${day}.${month}.${year} ${hours}:${minutes}:${seconds}`;
        }

        function fetchDeviceStatus() {
            fetch("/deviceStatus")
                .then(r => r.json())
                .then(data => {
                    rssiEl.textContent = data.rssi;
                    uptimeEl.textContent = formatUptime(data.uptimeSeconds || 0);
                    wifiConnectedEl.textContent = formatDateTime(wifiConnectTime);
                })
                .catch(() => {
                    rssiEl.textContent = "--";
                    uptimeEl.textContent = "--";
                    wifiConnectedEl.textContent = "--";
                    addLog("Device status unavailable", "warn");
                });
        }

        function testRingSip() {
            fetch("/ring/sip")
                .then(async (r) => {
                    const text = await r.text();
                    if (!r.ok) {
                        addLog(text || "SIP Ring failed", "error");
                        return fetch("/sipDebug")
                            .then(d => d.json())
                            .then(info => {
                                addLog(`SIP debug: user=${info.sip_user} target=${info.sip_target} has_sip_config=${info.has_sip_config}`, "warn");
                            })
                            .catch(() => addLog("SIP debug unavailable", "warn"));
                    }
                    addLog(text || "SIP Ring triggered", "info");
                })
                .catch(() => addLog("Failed to trigger SIP ring", "error"));
        }

        function fetchSipStatus() {
            fetch("/sipDebug")
                .then(r => r.json())
                .then(data => {
                    const sipCfg = data.has_sip_config ? "✓" : "✗";
                    trStatusEl.innerHTML = `<strong>Status:</strong> SIP ${sipCfg} | user=${data.sip_user || "-"} | target=${data.sip_target || "-"}`;
                })
                .catch(() => {
                    trStatusEl.innerHTML = "<strong>Status:</strong> unavailable";
                    addLog("SIP status unavailable", "warn");
                });
        }

        fetchStatus();
        fetchStreamInfo();
        fetchDeviceStatus();
        fetchSipStatus();
        fetchEventLog();
        setInterval(fetchDeviceStatus, 5000);
        setInterval(fetchSipStatus, 10000);
        setInterval(fetchStreamInfo, 3000);
        setInterval(fetchEventLog, 2000);

        const savedLogFontSize = localStorage.getItem("logFontSize") || "md";
        logFontSizeEl.value = savedLogFontSize;
        applyLogFontSize(savedLogFontSize);
        logFontSizeEl.addEventListener("change", (e) => applyLogFontSize(e.target.value));

        addLog("UI loaded", "info");
    </script>
</body>
</html>
)rawliteral";
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
        bool sip_enabled = prefs.getBool("sip_enabled", true);
        bool tr064_enabled = prefs.getBool("tr064_enabled", true);
        bool http_cam_enabled = prefs.getBool("http_cam_enabled", true);
        bool rtsp_enabled = prefs.getBool("rtsp_enabled", true);
        bool mic_enabled = prefs.getBool(kFeatMicEnabledKey, false);
        bool mic_muted = prefs.getBool(kFeatMicMutedKey, false);
        uint8_t mic_sensitivity = prefs.getUChar(kFeatMicSensitivityKey, DEFAULT_MIC_SENSITIVITY);
        bool audio_out_enabled = prefs.getBool(kFeatAudioOutEnabledKey, true);
        bool audio_out_muted = prefs.getBool(kFeatAudioOutMutedKey, false);
        uint8_t audio_out_volume = prefs.getUChar(kFeatAudioOutVolumeKey, DEFAULT_AUDIO_OUT_VOLUME);
        prefs.end();

        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Feature Setup</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="dark-mode-toggle">
        <label class="switch">
            <input type="checkbox" id="darkModeToggle">
            <span class="slider"></span>
        </label>
        <span>Dark Mode</span>
    </div>

    <div class="header-container">
        <h1 class="main-title">⚙️ Feature Setup</h1>
        <p>Enable or disable individual features for your doorbell.</p>
    </div>

    <div class="dashboard">
        <div class="container card setup-card">
            <h3>🔧 Core Features</h3>
            <div class="info-grid">
                <div class="info-item">
                    <span><strong>☎️ SIP Phone Ringing</strong></span>
                    <label class="switch">
                        <input type="checkbox" id="sip_enabled" )rawliteral" + String(sip_enabled ? "checked" : "") + R"rawliteral(>
                        <span class="slider"></span>
                    </label>
                </div>
                <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                    Ring FRITZ!Box phones when doorbell is pressed via SIP protocol.
                </div>

                <div class="info-item">
                    <span><strong>📡 TR-064 Integration</strong></span>
                    <label class="switch">
                        <input type="checkbox" id="tr064_enabled" )rawliteral" + String(tr064_enabled ? "checked" : "") + R"rawliteral(>
                        <span class="slider"></span>
                    </label>
                </div>
                <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                    Enable TR-064 SOAP protocol for advanced FRITZ!Box control.
                </div>

                <div class="info-item">
                    <span><strong>📹 HTTP Camera Streaming</strong></span>
                    <label class="switch">
                        <input type="checkbox" id="http_cam_enabled" )rawliteral" + String(http_cam_enabled ? "checked" : "") + R"rawliteral(>
                        <span class="slider"></span>
                    </label>
                </div>
                <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                    MJPEG stream at http://ESP32-IP:81/stream and snapshot endpoint.
                </div>
                <div class="info-item">
                    <span><strong>👥 Max MJPEG Clients</strong></span>
                    <input type="number" id="http_cam_max_clients" min="1" max="4" value=")rawliteral" + String(httpCamMaxClients) + R"rawliteral(" style="width: 80px;">
                </div>
                <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                    Limit simultaneous HTTP stream clients (e.g., Scrypted + FRITZ!Box).
                </div>

                <div class="info-item">
                    <span><strong>🎥 RTSP Camera Streaming</strong></span>
                    <label class="switch">
                        <input type="checkbox" id="rtsp_enabled" )rawliteral" + String(rtsp_enabled ? "checked" : "") + R"rawliteral(>
                        <span class="slider"></span>
                    </label>
                </div>
                <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                    RTSP stream at rtsp://ESP32-IP:8554/mjpeg/1 (experimental, use HTTP for Scrypted).
                </div>

                <div class="info-item">
                    <span><strong>🔊 Audio Out (Gong)</strong></span>
                    <label class="switch">
                        <input type="checkbox" id="audio_out_enabled" )rawliteral" + String(audio_out_enabled ? "checked" : "") + R"rawliteral(>
                        <span class="slider"></span>
                    </label>
                </div>
                <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                    Local gong playback via MAX98357A I2S DAC.
                </div>
                <div class="info-item">
                    <span><strong>🔇 Audio Out Mute</strong></span>
                    <label class="switch">
                        <input type="checkbox" id="audio_out_muted" )rawliteral" + String(audio_out_muted ? "checked" : "") + R"rawliteral(>
                        <span class="slider"></span>
                    </label>
                </div>
                <div class="info-item">
                    <span><strong>🔊 Audio Out Volume</strong></span>
                    <input type="range" id="audio_out_volume" min="0" max="100" value=")rawliteral" + String(audio_out_volume) + R"rawliteral(">
                    <span id="audioOutVolumeValue">)rawliteral" + String(audio_out_volume) + R"rawliteral(</span>
                </div>
            </div>
        </div>

        <div class="container card setup-card">
            <h3>📷 Camera Quality</h3>
            <div class="flash-stats" id="cameraSetupStatus">
                <strong>Camera:</strong> loading...
            </div>

            <label><strong>Frame Size</strong></label>
            <select id="framesize">
                <option value="5">QVGA (320x240)</option>
                <option value="6">CIF (400x296)</option>
                <option value="7">HVGA (480x320)</option>
                <option value="8">VGA (640x480)</option>
            </select>

            <label><strong>JPEG Quality</strong></label>
            <input type="range" id="quality" min="4" max="63" value="10">
            <span id="qualityValue">10</span>

            <label><strong>Brightness</strong></label>
            <input type="range" id="brightness" min="-2" max="2" step="1" value="0">
            <span id="brightnessValue">0</span>

            <label><strong>Contrast</strong></label>
            <input type="range" id="contrast" min="-2" max="2" step="1" value="0">
            <span id="contrastValue">0</span>

            <div style="margin-top: 12px;">
                <button class="button" onclick="applyCameraSettings()">Apply Camera Settings</button>
            </div>
        </div>

        <div class="container card setup-card">
            <h3>📺 Scrypted Stream Options</h3>
            <div class="flash-stats" id="scryptedStatus">
                <strong>Status:</strong> loading...
            </div>

            <label><strong>Source Type</strong></label>
            <div class="info-grid">
                <div class="info-item">
                    <span>HTTP MJPEG</span>
                    <input type="radio" name="scrypted_source" id="scrypted_source_http" value="http" )rawliteral" + String(scryptedSource == "http" ? "checked" : "") + R"rawliteral(>
                </div>
                <div class="info-item">
                    <span>RTSP MJPEG</span>
                    <input type="radio" name="scrypted_source" id="scrypted_source_rtsp" value="rtsp" )rawliteral" + String(scryptedSource == "rtsp" ? "checked" : "") + R"rawliteral(>
                </div>
            </div>

            <label style="margin-top: 12px;"><strong>Optimization</strong></label>
            <div class="info-grid">
                <div class="info-item">
                    <span>Low latency (short GOP)</span>
                    <input type="checkbox" id="scrypted_low_latency" )rawliteral" + String(scryptedLowLatency ? "checked" : "") + R"rawliteral(>
                </div>
                <div class="info-item">
                    <span>Reduce input buffering</span>
                    <input type="checkbox" id="scrypted_low_buffer" )rawliteral" + String(scryptedLowBuffer ? "checked" : "") + R"rawliteral(>
                </div>
                <div class="info-item">
                    <span>Prefer RTSP UDP transport</span>
                    <input type="checkbox" id="scrypted_rtsp_udp" )rawliteral" + String(scryptedRtspUdp ? "checked" : "") + R"rawliteral(>
                </div>
            </div>

            <label style="margin-top: 12px;"><strong>Audio (Mic)</strong></label>
            <div class="info-grid">
                <div class="info-item">
                    <span>Mic enabled</span>
                    <input type="checkbox" id="mic_enabled" )rawliteral" + String(mic_enabled ? "checked" : "") + R"rawliteral(>
                </div>
                <div class="info-item">
                    <span>Mic mute</span>
                    <input type="checkbox" id="mic_muted" )rawliteral" + String(mic_muted ? "checked" : "") + R"rawliteral(>
                </div>
            </div>
            <label><strong>Mic Sensitivity</strong></label>
            <input type="range" id="mic_sensitivity" min="0" max="100" value=")rawliteral" + String(mic_sensitivity) + R"rawliteral(">
            <span id="micSensitivityValue">)rawliteral" + String(mic_sensitivity) + R"rawliteral(</span>
            <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                Preview mic capture at http://ESP32-IP/audio.wav (RTSP audio planned).
            </div>

            <div class="flash-stats" style="margin-top: 12px;">
                <strong>Recommended Source URL:</strong>
                <p style="font-family: monospace; font-size: 0.9em; word-break: break-all;" id="scrypted_source_url"></p>
            </div>
            <div class="flash-stats">
                <strong>FFmpeg Input Args:</strong>
                <p style="font-family: monospace; font-size: 0.9em; word-break: break-all;" id="scrypted_input_args"></p>
            </div>
            <div class="flash-stats">
                <strong>FFmpeg Output Prefix:</strong>
                <p style="font-family: monospace; font-size: 0.9em; word-break: break-all;" id="scrypted_output_prefix"></p>
            </div>
        </div>
    </div>

    <div class="button-row log-actions" style="margin-top: 20px;">
        <button class="button" onclick="saveFeatures()">💾 Save Features</button>
        <button class="button danger-btn" onclick="location.href='/restart';">🔄 Restart</button>
        <a class="button danger-btn" href="/">Back</a>
    </div>

    <script>
        const darkToggle = document.getElementById("darkModeToggle");
        function setDarkMode(enabled) {
            document.body.classList.toggle("dark-mode", enabled);
            localStorage.setItem("darkMode", enabled ? "enabled" : "disabled");
        }
        const savedDarkMode = localStorage.getItem("darkMode");
        const darkModeEnabled = savedDarkMode === null ? true : savedDarkMode === "enabled";
        darkToggle.checked = darkModeEnabled;
        setDarkMode(darkModeEnabled);
        darkToggle.addEventListener("change", () => {
            setDarkMode(darkToggle.checked);
        });

        function saveFeatures() {
            const features = {
                sip_enabled: document.getElementById("sip_enabled").checked,
                tr064_enabled: document.getElementById("tr064_enabled").checked,
                http_cam_enabled: document.getElementById("http_cam_enabled").checked,
                rtsp_enabled: document.getElementById("rtsp_enabled").checked,
                http_cam_max_clients: parseInt(document.getElementById("http_cam_max_clients").value || "2", 10),
                scrypted_source: document.querySelector("input[name='scrypted_source']:checked")?.value || "http",
                scrypted_low_latency: document.getElementById("scrypted_low_latency").checked,
                scrypted_low_buffer: document.getElementById("scrypted_low_buffer").checked,
                scrypted_rtsp_udp: document.getElementById("scrypted_rtsp_udp").checked,
                mic_enabled: document.getElementById("mic_enabled").checked,
                mic_muted: document.getElementById("mic_muted").checked,
                mic_sensitivity: parseInt(document.getElementById("mic_sensitivity").value || "70", 10),
                audio_out_enabled: document.getElementById("audio_out_enabled").checked,
                audio_out_muted: document.getElementById("audio_out_muted").checked,
                audio_out_volume: parseInt(document.getElementById("audio_out_volume").value || "70", 10)
            };

            if (!validateScryptedOptions()) {
                alert("Scrypted options are invalid. Enable the required stream type.");
                return;
            }

            fetch("/saveFeatures", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(features)
            }).then(r => r.text())
              .then(t => {
                  alert(t);
                  setTimeout(() => window.location.reload(), 1000);
              })
              .catch(e => alert("Save failed: " + e));
        }

        const cameraSetupStatusEl = document.getElementById("cameraSetupStatus");
        const qualityValueEl = document.getElementById("qualityValue");
        const brightnessValueEl = document.getElementById("brightnessValue");
        const contrastValueEl = document.getElementById("contrastValue");
        const scryptedStatusEl = document.getElementById("scryptedStatus");
        const scryptedSourceUrlEl = document.getElementById("scrypted_source_url");
        const scryptedInputArgsEl = document.getElementById("scrypted_input_args");
        const scryptedOutputPrefixEl = document.getElementById("scrypted_output_prefix");
        const micSensitivityValueEl = document.getElementById("micSensitivityValue");
        const audioOutVolumeValueEl = document.getElementById("audioOutVolumeValue");

        function fetchCameraSetupStatus() {
            fetch("/status")
                .then(r => r.json())
                .then(data => {
                    cameraSetupStatusEl.innerHTML = `<strong>Camera:</strong> ${data.PID || "Unknown"} | size=${data.framesize} | quality=${data.quality}`;
                    if (data.framesize !== undefined) {
                        document.getElementById("framesize").value = data.framesize;
                    }
                    if (data.quality !== undefined) {
                        document.getElementById("quality").value = data.quality;
                        qualityValueEl.textContent = data.quality;
                    }
                    if (data.brightness !== undefined) {
                        document.getElementById("brightness").value = data.brightness;
                        brightnessValueEl.textContent = data.brightness;
                    }
                    if (data.contrast !== undefined) {
                        document.getElementById("contrast").value = data.contrast;
                        contrastValueEl.textContent = data.contrast;
                    }
                })
                .catch(() => {
                    cameraSetupStatusEl.innerHTML = "<strong>Camera:</strong> unavailable";
                });
        }

        function applyCameraSettings() {
            const fs = document.getElementById("framesize").value;
            const q = document.getElementById("quality").value;
            const b = document.getElementById("brightness").value;
            const c = document.getElementById("contrast").value;
            Promise.all([
                fetch(`/control?var=framesize&val=${fs}`),
                fetch(`/control?var=quality&val=${q}`),
                fetch(`/control?var=brightness&val=${b}`),
                fetch(`/control?var=contrast&val=${c}`)
            ]).then(() => {
                alert(`Applied camera settings: framesize=${fs}, quality=${q}, brightness=${b}, contrast=${c}`);
                fetchCameraSetupStatus();
            }).catch(() => alert("Failed to apply camera settings"));
        }

        document.getElementById("quality").addEventListener("input", (e) => {
            qualityValueEl.textContent = e.target.value;
        });
        document.getElementById("brightness").addEventListener("input", (e) => {
            brightnessValueEl.textContent = e.target.value;
        });
        document.getElementById("contrast").addEventListener("input", (e) => {
            contrastValueEl.textContent = e.target.value;
        });
        document.getElementById("mic_sensitivity").addEventListener("input", (e) => {
            micSensitivityValueEl.textContent = e.target.value;
        });
        document.getElementById("audio_out_volume").addEventListener("input", (e) => {
            audioOutVolumeValueEl.textContent = e.target.value;
        });

        function updateAudioUi() {
            const micEnabled = document.getElementById("mic_enabled").checked;
            document.getElementById("mic_muted").disabled = !micEnabled;
            document.getElementById("mic_sensitivity").disabled = !micEnabled;

            const audioOutEnabled = document.getElementById("audio_out_enabled").checked;
            document.getElementById("audio_out_muted").disabled = !audioOutEnabled;
            document.getElementById("audio_out_volume").disabled = !audioOutEnabled;
        }

        function getScryptedSource() {
            const selected = document.querySelector("input[name='scrypted_source']:checked");
            return selected ? selected.value : "http";
        }

        function buildScryptedOutputPrefix() {
            const lowLatency = document.getElementById("scrypted_low_latency").checked;
            const gop = lowLatency ? 30 : 60;
            return `-c:v libx264 -pix_fmt yuvj420p -preset ultrafast -bf 0 -g ${gop} -r 15 -b:v 500000 -bufsize 1000000 -maxrate 500000`;
        }

        function buildScryptedInputArgs() {
            const source = getScryptedSource();
            const lowBuffer = document.getElementById("scrypted_low_buffer").checked;
            const preferUdp = document.getElementById("scrypted_rtsp_udp").checked;
            const args = [];
            if (source === "rtsp") {
                args.push(`-rtsp_transport ${preferUdp ? "udp" : "tcp"}`);
            }
            if (lowBuffer) {
                args.push("-fflags nobuffer -flags low_delay -analyzeduration 0 -probesize 32");
            }
            return args.length ? args.join(" ") : "(none)";
        }

        function updateScryptedUi() {
            const httpRadio = document.getElementById("scrypted_source_http");
            const rtspRadio = document.getElementById("scrypted_source_rtsp");
            const rtspEnabled = document.getElementById("rtsp_enabled").checked;
            const httpEnabled = document.getElementById("http_cam_enabled").checked;

            httpRadio.disabled = !httpEnabled;
            rtspRadio.disabled = !rtspEnabled;

            if (!httpEnabled && rtspEnabled) {
                rtspRadio.checked = true;
            } else if (!rtspEnabled && httpEnabled) {
                httpRadio.checked = true;
            }

            const source = getScryptedSource();
            const ip = ")rawliteral" + WiFi.localIP().toString() + R"rawliteral(";
            const sourceUrl = source === "rtsp"
                ? `rtsp://${ip}:8554/mjpeg/1`
                : `http://${ip}:81/stream`;
            scryptedSourceUrlEl.textContent = sourceUrl;
            scryptedInputArgsEl.textContent = buildScryptedInputArgs();
            scryptedOutputPrefixEl.textContent = buildScryptedOutputPrefix();

            const rtspUdpEl = document.getElementById("scrypted_rtsp_udp");
            rtspUdpEl.disabled = source !== "rtsp";

            let status = "OK";
            if (!httpEnabled && !rtspEnabled) {
                status = "Enable HTTP or RTSP streaming to use Scrypted.";
            } else if (source === "rtsp" && !rtspEnabled) {
                status = "Enable RTSP streaming to use RTSP source.";
            } else if (source === "http" && !httpEnabled) {
                status = "Enable HTTP streaming to use HTTP source.";
            }
            scryptedStatusEl.innerHTML = `<strong>Status:</strong> ${status}`;
        }

        function validateScryptedOptions() {
            const source = getScryptedSource();
            const rtspEnabled = document.getElementById("rtsp_enabled").checked;
            const httpEnabled = document.getElementById("http_cam_enabled").checked;
            if (source === "rtsp" && !rtspEnabled) {
                return false;
            }
            if (source === "http" && !httpEnabled) {
                return false;
            }
            return true;
        }

        document.querySelectorAll("input[name='scrypted_source']").forEach((el) => {
            el.addEventListener("change", updateScryptedUi);
        });
        document.getElementById("scrypted_low_latency").addEventListener("change", updateScryptedUi);
        document.getElementById("scrypted_low_buffer").addEventListener("change", updateScryptedUi);
        document.getElementById("scrypted_rtsp_udp").addEventListener("change", updateScryptedUi);
        document.getElementById("rtsp_enabled").addEventListener("change", updateScryptedUi);
        document.getElementById("http_cam_enabled").addEventListener("change", updateScryptedUi);
        document.getElementById("mic_enabled").addEventListener("change", updateAudioUi);
        document.getElementById("audio_out_enabled").addEventListener("change", updateAudioUi);

        fetchCameraSetupStatus();
        updateScryptedUi();
        updateAudioUi();
    </script>
</body>
</html>
)rawliteral";
        request->send(200, "text/html", page);
      });

      server.on("/tr064", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("tr064", true);
        String tr064_user = prefs.getString("tr064_user", "");
        String tr064_pass = prefs.getString("tr064_pass", "");
        String number = prefs.getString("number", "");
        prefs.end();

        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>TR-064 Setup</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="dark-mode-toggle">
        <label class="switch">
            <input type="checkbox" id="darkModeToggle">
            <span class="slider"></span>
        </label>
        <span>Dark Mode</span>
    </div>

    <div class="container">
        <h1>☎️ TR-064 Setup</h1>
        <p>Configure FRITZ!Box credentials and the internal ring number.</p>
        <div class="flash-stats">
            <strong>FRITZ!Box TR-064 Setup (from AVM docs)</strong>
            <p><strong>1) Enable TR-064 access</strong></p>
            <ul>
                <li>FRITZ!Box UI → Heimnetz → Heimnetzübersicht → Netzwerkeinstellungen</li>
                <li>Enable: „Zugriff für Anwendungen zulassen“</li>
                <li>TR-064 endpoints: http://fritz.box:49000 and https://fritz.box:49443</li>
            </ul>
            <p><strong>2) Create a user with TR-064 permissions</strong></p>
            <ul>
                <li>System → FRITZ!Box-Benutzer</li>
                <li>Create a dedicated user (e.g., tr064-client)</li>
                <li>Permissions: FRITZ!Box Einstellungen, Telefonie, Smart Home (if needed)</li>
            </ul>
        <p><strong>TR-064 uses HTTP Digest Auth</strong></p>
        <p><strong>FRITZ!Box address:</strong> )rawliteral" + WiFi.gatewayIP().toString() + R"rawliteral(</p>
        <p>Enter TR-064 credentials below.</p>
        </div>

        <h3>📡 TR-064 SOAP</h3>
        <label><strong>TR-064 Username:</strong></label>
        <input type="text" id="tr064_user" value=")rawliteral" + tr064_user + R"rawliteral(" placeholder="TR-064 app username">

        <label><strong>TR-064 Password:</strong></label>
        <input type="password" id="tr064_pass" value=")rawliteral" + tr064_pass + R"rawliteral(" placeholder="TR-064 app password">

        <h3>📞 Ring Configuration</h3>
        <label><strong>Internal Ring Number:</strong></label>
        <input type="text" id="tr_number" value=")rawliteral" + number + R"rawliteral(" placeholder="e.g., **9 or **610">

        <div>
            <button class="button" onclick="saveTr064()">💾 Save</button>
            <button class="button" onclick="testRingTr064()">🔔 Test TR-064 Ring</button>
            <a class="button danger-btn" href="/">Back</a>
        </div>
    </div>

    <script>
        // Dark mode handling
        const darkToggle = document.getElementById("darkModeToggle");
        function setDarkMode(enabled) {
            document.body.classList.toggle("dark-mode", enabled);
            localStorage.setItem("darkMode", enabled ? "enabled" : "disabled");
        }
        // Initialize dark mode from localStorage (default: enabled)
        const savedDarkMode = localStorage.getItem("darkMode");
        const darkModeEnabled = savedDarkMode === null ? true : savedDarkMode === "enabled";
        darkToggle.checked = darkModeEnabled;
        setDarkMode(darkModeEnabled);
        darkToggle.addEventListener("change", () => {
            setDarkMode(darkToggle.checked);
        });

        // Ensure emoji text renders correctly under UTF-8.
        function saveTr064() {
            let tr064_user = document.getElementById("tr064_user").value;
            let tr064_pass = document.getElementById("tr064_pass").value;
            let number = document.getElementById("tr_number").value;

            fetch("/saveTR064", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ 
                    "tr064_user": tr064_user, 
                    "tr064_pass": tr064_pass, 
                    "number": number 
                })
            }).then(r => r.text())
              .then(t => alert(t))
              .catch(e => alert("Save failed: " + e));
        }

        function testRingTr064() {
            fetch("/ring/tr064")
                .then(r => r.text())
                .then(t => alert(t))
                .catch(e => alert("TR-064 Ring failed: " + e));
        }
    </script>
</body>
</html>
)rawliteral";
        request->send(200, "text/html", page);
      });

      server.on("/sip", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("sip", true);
        String sip_user = prefs.getString("sip_user", "");
        String sip_password = prefs.getString("sip_password", "");
        String sip_displayname = prefs.getString("sip_displayname", "Doorbell");
        String sip_target = prefs.getString("sip_target", "**610");
        String scrypted_webhook = prefs.getString("scrypted_webhook", "");
        prefs.end();

        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>SIP Setup</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="dark-mode-toggle">
        <label class="switch">
            <input type="checkbox" id="darkModeToggle">
            <span class="slider"></span>
        </label>
        <span>Dark Mode</span>
    </div>

    <div class="container full-width">
        <h1>☎️ SIP Setup</h1>
        <p>Configure FRITZ!Box SIP credentials to ring internal phones.</p>
        <div class="flash-stats">
            <strong>FRITZ!Box SIP Setup Instructions</strong>
            <p><strong>1) Create an IP Phone in FRITZ!Box</strong></p>
            <ul>
                <li>FRITZ!Box UI → Telefonie → Telefoniegeräte</li>
                <li>Click "Neues Gerät einrichten"</li>
                <li>Select "Telefon (mit und ohne Anrufbeantworter)"</li>
                <li>Select "LAN/WLAN (IP-Telefon)"</li>
                <li>Enter username (e.g., 620) and password</li>
                <li>Give it a name like "ESP32-Doorbell"</li>
            </ul>
            <p><strong>2) Note the credentials</strong></p>
            <ul>
                <li>Username (Benutzername): This is your SIP username</li>
                <li>Password (Kennwort): This is your SIP password</li>
            </ul>
            <p><strong>3) Configure target number</strong></p>
            <ul>
                <li>Use **610 to ring all DECT phones</li>
                <li>Use **9 plus extension to ring a specific phone</li>
            </ul>
        <p><strong>FRITZ!Box address:</strong> )rawliteral" + WiFi.gatewayIP().toString() + R"rawliteral(</p>
        </div>

        <h3>📡 SIP Credentials</h3>
        <label><strong>SIP Username:</strong></label>
        <input type="text" id="sip_user" value=")rawliteral" + sip_user + R"rawliteral(" placeholder="e.g., 620">

        <label><strong>SIP Password:</strong></label>
        <input type="password" id="sip_password" value=")rawliteral" + sip_password + R"rawliteral(" placeholder="IP phone password">

        <label><strong>Display Name:</strong></label>
        <input type="text" id="sip_displayname" value=")rawliteral" + sip_displayname + R"rawliteral(" placeholder="Doorbell">

        <h3>📞 Ring Configuration</h3>
        <label><strong>Target Number:</strong></label>
        <input type="text" id="sip_target" value=")rawliteral" + sip_target + R"rawliteral(" placeholder="e.g., **610">

        <h3>🏠 Scrypted Integration (HomeKit)</h3>
        <label><strong>Doorbell Webhook URL:</strong></label>
        <input type="text" id="scrypted_webhook" value=")rawliteral" + scrypted_webhook + R"rawliteral(" placeholder="http://scrypted-ip:11080/endpoint/your-id/public/">
        <p style="font-size: 0.9em; color: #666;">Get this from Scrypted doorbell device settings. Leave empty if not using Scrypted.</p>

        <div>
            <button class="button" onclick="saveSip()">💾 Save</button>
            <button class="button" onclick="testRingSip()">🔔 Test SIP Ring</button>
            <a class="button danger-btn" href="/">Back</a>
        </div>
    </div>

    <script>
        // Dark mode handling
        const darkToggle = document.getElementById("darkModeToggle");
        function setDarkMode(enabled) {
            document.body.classList.toggle("dark-mode", enabled);
            localStorage.setItem("darkMode", enabled ? "enabled" : "disabled");
        }
        const savedDarkMode = localStorage.getItem("darkMode");
        const darkModeEnabled = savedDarkMode === null ? true : savedDarkMode === "enabled";
        darkToggle.checked = darkModeEnabled;
        setDarkMode(darkModeEnabled);
        darkToggle.addEventListener("change", () => {
            setDarkMode(darkToggle.checked);
        });

        function saveSip() {
            let sip_user = document.getElementById("sip_user").value;
            let sip_password = document.getElementById("sip_password").value;
            let sip_displayname = document.getElementById("sip_displayname").value;
            let sip_target = document.getElementById("sip_target").value;
            let scrypted_webhook = document.getElementById("scrypted_webhook").value;

            fetch("/saveSIP", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ 
                    "sip_user": sip_user, 
                    "sip_password": sip_password, 
                    "sip_displayname": sip_displayname, 
                    "sip_target": sip_target,
                    "scrypted_webhook": scrypted_webhook
                })
            }).then(r => r.text())
              .then(t => alert(t))
              .catch(e => alert("Save failed: " + e));
        }

        function testRingSip() {
            fetch("/ring/sip")
                .then(r => r.text())
                .then(t => alert(t))
                .catch(e => alert("SIP Ring failed: " + e));
        }
    </script>
</body>
</html>
)rawliteral";
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
          prefs.putBool("sip_enabled", sip_enabled);
          prefs.putBool("tr064_enabled", tr064_enabled);
          prefs.putBool("http_cam_enabled", http_cam_enabled);
          prefs.putBool("rtsp_enabled", rtsp_enabled);
          prefs.putUChar("http_cam_max_clients", http_cam_max_clients);
          prefs.putString("scrypted_source", scrypted_source);
          prefs.putBool("scrypted_low_latency", scrypted_low_latency);
          prefs.putBool("scrypted_low_buffer", scrypted_low_buffer);
          prefs.putBool("scrypted_rtsp_udp", scrypted_rtsp_udp);
          prefs.putBool(kFeatMicEnabledKey, mic_enabled);
          prefs.putBool(kFeatMicMutedKey, mic_muted);
          prefs.putUChar(kFeatMicSensitivityKey, static_cast<uint8_t>(mic_sensitivity));
          prefs.putBool(kFeatAudioOutEnabledKey, audio_out_enabled);
          prefs.putBool(kFeatAudioOutMutedKey, audio_out_muted);
          prefs.putUChar(kFeatAudioOutVolumeKey, static_cast<uint8_t>(audio_out_volume));
          prefs.end();

          // Update global variables
          sipEnabled = sip_enabled;
          tr064Enabled = tr064_enabled;
          httpCamEnabled = http_cam_enabled;
          rtspEnabled = rtsp_enabled;
          httpCamMaxClients = http_cam_max_clients;
          scryptedSource = scrypted_source;
          scryptedLowLatency = scrypted_low_latency;
          scryptedLowBuffer = scrypted_low_buffer;
          scryptedRtspUdp = scrypted_rtsp_udp;
          micEnabled = mic_enabled;
          micMuted = mic_muted;
          micSensitivity = static_cast<uint8_t>(mic_sensitivity);
          audioOutEnabled = audio_out_enabled;
          audioOutMuted = audio_out_muted;
          audioOutVolume = static_cast<uint8_t>(audio_out_volume);
          #ifdef CAMERA
          setCameraStreamMaxClients(httpCamMaxClients);
          setRtspAllowUdp(scryptedRtspUdp);
          #endif
          configureAudio(micEnabled, micMuted, micSensitivity, audioOutEnabled, audioOutMuted, audioOutVolume);

          String summary = "Feature settings saved: SIP=" + String(sip_enabled ? "on" : "off") +
                           " TR-064=" + String(tr064_enabled ? "on" : "off") +
                           " HTTP=" + String(http_cam_enabled ? "on" : "off") +
                           " RTSP=" + String(rtsp_enabled ? "on" : "off") +
                           " HTTP max clients=" + String(http_cam_max_clients) +
                           " Scrypted source=" + scrypted_source +
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
          String scrypted_webhook = doc["scrypted_webhook"].as<String>();

          Preferences prefs;
          prefs.begin("sip", false);
          if (sip_user.isEmpty() && sip_password.isEmpty()) {
            prefs.remove("sip_user");
            prefs.remove("sip_password");
            prefs.remove("sip_displayname");
            prefs.remove("sip_target");
            prefs.remove("scrypted_webhook");
            prefs.end();
            request->send(200, "text/plain", "SIP settings cleared");
            return;
          }

          prefs.putString("sip_user", sip_user);
          prefs.putString("sip_password", sip_password);
          prefs.putString("sip_displayname", sip_displayname);
          prefs.putString("sip_target", sip_target);
          prefs.putString("scrypted_webhook", scrypted_webhook);
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

        // Get RTSP session count
        int rtspSessions = getRtspActiveSessionCount();

        String json = "{";
        json += "\"running\":" + String(running ? "true" : "false") + ",";
        json += "\"active\":" + String(active ? "true" : "false") + ",";
        json += "\"clients\":" + String(clients) + ",";
        json += "\"client_ip\":\"" + clientIp + "\",";
        json += "\"connected_ms\":" + String(connectedMs) + ",";
        json += "\"last_frame_age_ms\":" + String(lastFrameAgeMs) + ",";
        json += "\"clients_list\":" + clientsJson + ",";
        json += "\"rtsp_sessions\":" + String(rtspSessions);
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

      server.on("/ring", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (sipEnabled && triggerSipRing(sipConfig)) {
          request->send(200, "text/plain", "Ring triggered via SIP");
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
        if (!sipEnabled) {
          request->send(503, "text/plain", "SIP ring disabled");
          return;
        }
        if (triggerSipRing(sipConfig)) {
          request->send(200, "text/plain", "SIP ring triggered");
        } else {
          request->send(500, "text/plain", "SIP ring failed - check configuration");
        }
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
  // Sequence: local gong first, then external notifications.
  playGongAsync();

  // Trigger Scrypted doorbell event (HomeKit notification)
  if (!sipConfig.scrypted_webhook.isEmpty()) {
    HTTPClient http;
    http.begin(sipConfig.scrypted_webhook);
    http.setTimeout(2000); // 2 second timeout
    int httpCode = http.GET();
    http.end();
    
    if (httpCode > 0) {
      logEvent(LOG_INFO, "🔔 Scrypted webhook triggered (HomeKit notification)");
    } else {
      logEvent(LOG_WARN, "⚠️ Scrypted webhook failed");
    }
  }
  
  // Ring FRITZ!Box internal phones (only if SIP enabled)
  if (sipEnabled) {
    if (triggerSipRing(sipConfig)) {
      logEvent(LOG_INFO, "📞 FRITZ!Box ring triggered via SIP");
    } else {
      logEvent(LOG_WARN, "⚠️ SIP ring failed - check SIP configuration");
    }
  } else {
    logEvent(LOG_INFO, "ℹ️ SIP disabled - skipping phone ring");
  }
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
    sendRegisterIfNeeded(sipConfig);
    // Handle any incoming SIP responses
    handleSipIncoming();
  }
  
  // Simple debounce: detect stable press, then wait for release.
  bool pressed = isDoorbellPressed();
  unsigned long nowMs = millis();
  if (pressed != lastButtonPressed) {
    lastButtonChangeMs = nowMs;
    lastButtonPressed = pressed;
  }

  if (pressed && (nowMs - lastButtonChangeMs) > DOORBELL_DEBOUNCE_MS) {
    handleDoorbellPress();
    while (isDoorbellPressed()) {
      delay(10);
    }
    lastButtonPressed = false;
    lastButtonChangeMs = millis();
  }

  delay(10);
}
