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
#include "logger.h"

// Global objects shared across setup/loop.
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String wifiSSID, wifiPassword;
Tr064Config tr064Config;
SipConfig sipConfig;
bool cameraReady = false;

// Feature toggles (loaded once at boot, updated on save)
bool sipEnabled = true;
bool tr064Enabled = true;
bool httpCamEnabled = true;
bool rtspEnabled = true;

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
  if (!featPrefs.isKey("sip_enabled")) {
    featPrefs.putBool("sip_enabled", true);
    featPrefs.putBool("tr064_enabled", true);
    featPrefs.putBool("http_cam_enabled", true);
    featPrefs.putBool("rtsp_enabled", true);
    logEvent(LOG_INFO, "✅ Feature toggles initialized with defaults (all enabled)");
  }
  // Load feature flags into global variables
  sipEnabled = featPrefs.getBool("sip_enabled", true);
  tr064Enabled = featPrefs.getBool("tr064_enabled", true);
  httpCamEnabled = featPrefs.getBool("http_cam_enabled", true);
  rtspEnabled = featPrefs.getBool("rtsp_enabled", true);
  featPrefs.end();
  logEvent(LOG_INFO, "📋 Features loaded: SIP=" + String(sipEnabled) + " TR-064=" + String(tr064Enabled) + " HTTP=" + String(httpCamEnabled) + " RTSP=" + String(rtspEnabled));

  // Configure doorbell GPIO based on active-low setting.
  pinMode(DOORBELL_BUTTON_PIN, DOORBELL_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT);

  #ifdef CAMERA
  // Initialize camera and register endpoints if hardware is present.
  cameraReady = initCamera();
  if (cameraReady) {
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
      
      // Initialize SIP client and send initial REGISTER
      if (initSipClient()) {
        sendSipRegister(sipConfig);
      }
      
      // Start web server for normal operation
      server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Doorbell</title>
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
            
            <label><strong>Frame Size</strong></label>
            <select id="framesize">
                <option value="5">QVGA (320x240)</option>
                <option value="6">CIF (400x296)</option>
                <option value="7" selected>HVGA (480x320)</option>
                <option value="8">VGA (640x480)</option>
            </select>

            <label><strong>JPEG Quality</strong></label>
            <input type="range" id="quality" min="4" max="63" value="10">
            <span id="qualityValue">10</span>

            <hr>
            <div class="button-row">
                <a class="button config-btn" href="/capture" target="_blank">📸 Snapshot</a>
                <a class="button" href="http://)rawliteral" + WiFi.localIP().toString() + R"rawliteral(:81/stream" target="_blank">🎥 Live Stream</a>
            </div>
            <button class="button" onclick="applySettings()">Apply Settings</button>
            <hr>
            <div class="flash-stats">
                <strong>📡 RTSP Stream (for Scrypted):</strong>
                <p style="font-family: monospace; font-size: 0.9em; word-break: break-all;">
                    rtsp://)rawliteral" + WiFi.localIP().toString() + R"rawliteral(:8554/mjpeg/1
                </p>
                <p style="font-size: 0.85em; color: #666;">
                    Use the RTSP Camera plugin or prefix with <code>-i</code> if using FFmpeg Camera.
                </p>
            </div>
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

    <div class="log-container" id="log"></div>

    <script>
        const logEl = document.getElementById("log");
        const statusEl = document.getElementById("cameraStatus");
        const trStatusEl = document.getElementById("tr064Status");
        const darkToggle = document.getElementById("darkModeToggle");
        const rssiEl = document.getElementById("rssi");
        const uptimeEl = document.getElementById("uptime");
        const wifiConnectedEl = document.getElementById("wifiConnected");
        const qualityValueEl = document.getElementById("qualityValue");
        const wifiConnectTime = Date.now();
        let lastEventId = 0;

        function addLog(message, level = "info") {
            const entry = document.createElement("div");
            entry.className = `log-entry ${level}`;
            const ts = new Date().toLocaleTimeString();
            entry.textContent = `[${ts}] ${message}`;
            logEl.prepend(entry);
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

        function applySettings() {
            const fs = document.getElementById("framesize").value;
            const q = document.getElementById("quality").value;
            Promise.all([
                fetch(`/control?var=framesize&val=${fs}`),
                fetch(`/control?var=quality&val=${q}`)
            ]).then(() => {
                addLog(`Applied camera settings: framesize=${fs}, quality=${q}`, "info");
                fetchStatus();
            }).catch(() => addLog("Failed to apply camera settings", "error"));
        }

        document.getElementById("quality").addEventListener("input", (e) => {
            qualityValueEl.textContent = e.target.value;
        });

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
        fetchDeviceStatus();
        fetchSipStatus();
        fetchEventLog();
        setInterval(fetchDeviceStatus, 5000);
        setInterval(fetchSipStatus, 10000);
        setInterval(fetchEventLog, 2000);
        addLog("UI loaded", "info");
    </script>
</body>
</html>
)rawliteral");
      });

      server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("features", false);
        bool sip_enabled = prefs.getBool("sip_enabled", true);
        bool tr064_enabled = prefs.getBool("tr064_enabled", true);
        bool http_cam_enabled = prefs.getBool("http_cam_enabled", true);
        bool rtsp_enabled = prefs.getBool("rtsp_enabled", true);
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

    <div class="container full-width">
        <h1>⚙️ Feature Setup</h1>
        <p>Enable or disable individual features for your doorbell.</p>
        
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
                <span><strong>🎥 RTSP Camera Streaming</strong></span>
                <label class="switch">
                    <input type="checkbox" id="rtsp_enabled" )rawliteral" + String(rtsp_enabled ? "checked" : "") + R"rawliteral(>
                    <span class="slider"></span>
                </label>
            </div>
            <div style="padding: 0 8px 12px 8px; font-size: 0.9em; color: #666;">
                RTSP stream at rtsp://ESP32-IP:8554/mjpeg/1 (experimental, use HTTP for Scrypted).
            </div>
        </div>

        <div style="margin-top: 30px;">
            <button class="button" onclick="saveFeatures()">💾 Save Features</button>
            <button class="button danger-btn" onclick="location.href='/restart';">🔄 Restart</button>
            <a class="button danger-btn" href="/">Back</a>
        </div>
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
                rtsp_enabled: document.getElementById("rtsp_enabled").checked
            };

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

          Preferences prefs;
          prefs.begin("features", false);
          prefs.putBool("sip_enabled", sip_enabled);
          prefs.putBool("tr064_enabled", tr064_enabled);
          prefs.putBool("http_cam_enabled", http_cam_enabled);
          prefs.putBool("rtsp_enabled", rtsp_enabled);
          prefs.end();

          // Update global variables
          sipEnabled = sip_enabled;
          tr064Enabled = tr064_enabled;
          httpCamEnabled = http_cam_enabled;
          rtspEnabled = rtsp_enabled;

          request->send(200, "text/plain", "Feature settings saved! Restarting ESP32...");
          
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
      server.on("/eventLog", HTTP_GET, [](AsyncWebServerRequest *request) {
        uint32_t sinceId = 0;
        if (request->hasParam("since")) {
          sinceId = request->getParam("since")->value().toInt();
        }
        String json = getEventLogJson(sinceId);
        request->send(200, "application/json", json);
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
      startRtspServer();
      logEvent(LOG_INFO, "✅ RTSP streaming enabled");
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
  // Handle DNS requests in AP mode
  if (isAPModeActive()) {
    dnsServer.processNextRequest();
  }
  
  // Handle RTSP client connections for Scrypted (non-blocking)
  handleRtspClient();
  
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
