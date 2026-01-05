#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "wifi_ap.h"
#include "config.h"
#include "tr064_client.h"
#include "cameraAPI.h"
#include "cameraStream.h"

// Global objects shared across setup/loop.
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String wifiSSID, wifiPassword;
Tr064Config tr064Config;
bool cameraReady = false;

bool lastButtonPressed = false;
unsigned long lastButtonChangeMs = 0;

bool initFileSystem(AsyncWebServer &server) {
  // Mount LittleFS and expose static assets for the UI.
  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS mount failed");
    return false;
  }
  server.serveStatic("/style.css", LittleFS, "/style.css");
  server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n🔔 ESP32-S3 Doorbell Starting...");
  Serial.println("====================================");

  // Mount filesystem early so both AP and normal mode can serve CSS.
  initFileSystem(server);

  // Load TR-064 config once at boot; UI can update it later.
  loadTr064Config(tr064Config);

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
    Serial.println("🚨 No WiFi credentials found. Starting AP mode...");
    startAPMode(server, dnsServer, preferences);
  } else {
    Serial.printf("🔍 Found saved WiFi: %s\n", wifiSSID.c_str());
    attemptWiFiConnection(wifiSSID, wifiPassword);
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ Connected to WiFi successfully!");
      
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
        </div>

        <!-- TR-064 Card -->
        <div class="container card">
            <h3>☎️ TR-064</h3>
            <p>FRITZ!Box TR-064 integration for doorbell notifications.</p>
            <div class="flash-stats" id="tr064Status">
                <strong>Status:</strong> loading...
            </div>
            <hr>
            <div class="button-row">
                <button class="button" onclick="testRingHttp()">🔔 Test Ring</button>
                <a class="button config-btn" href="/tr064">⚙️ Setup</a>
            </div>
            <a class="button" href="/tr064Debug" target="_blank">🐛 Debug JSON</a>
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

        function testRingHttp() {
            fetch("/ring/http")
                .then(async (r) => {
                    const text = await r.text();
                    if (!r.ok) {
                        addLog(text || "HTTP Ring failed", "error");
                        return fetch("/tr064Debug")
                            .then(d => d.json())
                            .then(info => {
                                addLog(`HTTP debug: gateway=${info.gateway} http_user=${info.http_user} number=${info.number} has_http_config=${info.has_http_config}`, "warn");
                            })
                            .catch(() => addLog("HTTP debug unavailable", "warn"));
                    }
                    addLog(text || "HTTP Ring triggered", "info");
                })
                .catch(() => addLog("Failed to trigger HTTP ring", "error"));
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

        function fetchTr064Status() {
            fetch("/tr064Debug")
                .then(r => r.json())
                .then(data => {
                    const httpCfg = data.has_http_config ? "✓" : "✗";
                    const tr064Cfg = data.has_tr064_config ? "✓" : "✗";
                    trStatusEl.innerHTML = `<strong>Status:</strong> HTTP ${httpCfg} | TR-064 ${tr064Cfg} | number=${data.number || "-"}`;
                })
                .catch(() => {
                    trStatusEl.innerHTML = "<strong>Status:</strong> unavailable";
                    addLog("TR-064 status unavailable", "warn");
                });
        }

        fetchStatus();
        fetchDeviceStatus();
        fetchTr064Status();
        setInterval(fetchDeviceStatus, 5000);
        setInterval(fetchTr064Status, 10000);
        addLog("UI loaded", "info");
    </script>
</body>
</html>
)rawliteral");
      });

      server.on("/tr064", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("tr064", true);
        String http_user = prefs.getString("http_user", "");
        String http_pass = prefs.getString("http_pass", "");
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
        <p>Enter credentials for both HTTP (web UI) and TR-064 (SOAP) access below.</p>
        </div>

        <h3>🌐 HTTP Click-to-Dial (Web UI)</h3>
        <label><strong>Web UI Username:</strong></label>
        <input type="text" id="http_user" value=")rawliteral" + http_user + R"rawliteral(" placeholder="Leave empty or enter admin username">

        <label><strong>Web UI Password:</strong></label>
        <input type="password" id="http_pass" value=")rawliteral" + http_pass + R"rawliteral(" placeholder="FRITZ!Box admin password">

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
            <button class="button" onclick="testRingHttp()">🔔 Test HTTP Ring</button>
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
            let http_user = document.getElementById("http_user").value;
            let http_pass = document.getElementById("http_pass").value;
            let tr064_user = document.getElementById("tr064_user").value;
            let tr064_pass = document.getElementById("tr064_pass").value;
            let number = document.getElementById("tr_number").value;

            fetch("/saveTR064", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ 
                    "http_user": http_user, 
                    "http_pass": http_pass, 
                    "tr064_user": tr064_user, 
                    "tr064_pass": tr064_pass, 
                    "number": number 
                })
            }).then(r => r.text())
              .then(t => alert(t))
              .catch(e => alert("Save failed: " + e));
        }

        function testRingHttp() {
            fetch("/ring/http")
                .then(r => r.text())
                .then(t => alert(t))
                .catch(e => alert("HTTP Ring failed: " + e));
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

      server.on("/tr064Debug", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("tr064", true);
        String http_user = prefs.getString("http_user", "");
        String tr064_user = prefs.getString("tr064_user", "");
        String number = prefs.getString("number", "");
        prefs.end();
        String gateway = WiFi.gatewayIP().toString();

        String json = "{";
        json += "\"gateway\":\"" + gateway + "\",";
        json += "\"http_user\":\"" + http_user + "\",";
        json += "\"tr064_user\":\"" + tr064_user + "\",";
        json += "\"number\":\"" + number + "\",";
        json += "\"has_http_config\":" + String(hasHttpConfig(tr064Config) ? "true" : "false") + ",";
        json += "\"has_tr064_config\":" + String(hasTr064Config(tr064Config) ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
      });

      server.on("/saveTR064", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
          String receivedData = String((char *)data).substring(0, len);
          Serial.println("📥 Received TR-064 Config: " + receivedData);

          JsonDocument doc;
          DeserializationError error = deserializeJson(doc, receivedData);
          if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
          }

          String http_user = doc["http_user"].as<String>();
          String http_pass = doc["http_pass"].as<String>();
          String tr064_user = doc["tr064_user"].as<String>();
          String tr064_pass = doc["tr064_pass"].as<String>();
          String number = doc["number"].as<String>();

          Preferences prefs;
          prefs.begin("tr064", false);
          if (number.isEmpty() && http_pass.isEmpty() && tr064_pass.isEmpty()) {
            prefs.remove("http_user");
            prefs.remove("http_pass");
            prefs.remove("tr064_user");
            prefs.remove("tr064_pass");
            prefs.remove("number");
            prefs.end();
            request->send(200, "text/plain", "TR-064 settings cleared");
            return;
          }

          prefs.putString("http_user", http_user);
          prefs.putString("http_pass", http_pass);
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

      server.on("/ring", HTTP_GET, [](AsyncWebServerRequest *request) {
        // Try HTTP click-to-dial first (works on all FRITZ!Box models)
        if (triggerHttpRing(tr064Config)) {
          request->send(200, "text/plain", "Ring triggered via HTTP");
        } else if (triggerTr064Ring(tr064Config)) {
          request->send(200, "text/plain", "Ring triggered via TR-064");
        } else {
          request->send(500, "text/plain", "Ring failed (both HTTP and TR-064 failed)");
        }
      });

      server.on("/ring/http", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (triggerHttpRing(tr064Config)) {
          request->send(200, "text/plain", "HTTP ring triggered");
        } else {
          request->send(500, "text/plain", "HTTP ring failed");
        }
      });

      server.on("/ring/tr064", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (triggerTr064Ring(tr064Config)) {
          request->send(200, "text/plain", "TR-064 ring triggered");
        } else {
          request->send(500, "text/plain", "TR-064 ring failed");
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

      server.begin();
      Serial.println("✅ Web server started");
    } else {
      Serial.println("❌ WiFi connection failed. Starting AP mode...");
      startAPMode(server, dnsServer, preferences);
    }
  }

  #ifdef CAMERA
  // Start MJPEG streaming server on port 81.
  if (cameraReady) {
    startCameraStreamServer();
  }
  #endif

  Serial.println("====================================");
  Serial.println("Setup complete!\n");
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
  // Try HTTP click-to-dial first, fall back to TR-064
  bool ringTriggered = false;
  
  if (triggerHttpRing(tr064Config)) {
    Serial.println("📞 FRITZ!DECT ring triggered via HTTP");
    ringTriggered = true;
  } else if (triggerTr064Ring(tr064Config)) {
    Serial.println("📞 FRITZ!DECT ring triggered via TR-064");
    ringTriggered = true;
  }
  
  if (!ringTriggered) {
    Serial.println("⚠️ FRITZ!DECT ring not triggered (both methods failed)");
  }
}

void loop() {
  // Handle DNS requests in AP mode
  if (isAPModeActive()) {
    dnsServer.processNextRequest();
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
