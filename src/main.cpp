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
        <h1>🔔 ESP32-S3 Doorbell</h1>
        <p>System Online</p>
        <p><strong>WiFi:</strong> Connected</p>
        <p><strong>IP:</strong> )rawliteral" + WiFi.localIP().toString() + R"rawliteral(</p>
        <p><strong>Signal:</strong> <span id="rssi">--</span> dBm</p>
        <p><strong>Uptime:</strong> <span id="uptime">--</span></p>

        <div class="flash-stats" id="cameraStatus">
            <strong>Camera:</strong> loading...
        </div>

        <div>
            <a class="button config-btn" href="/capture" target="_blank">Snapshot</a>
            <a class="button" href="http://)rawliteral" + WiFi.localIP().toString() + R"rawliteral(:81/stream" target="_blank">Live Stream</a>
            <button class="button" onclick="testRing()">Test Ring</button>
            <a class="button config-btn" href="/tr064">TR-064 Setup</a>
            <a class="button" href="/tr064Debug" target="_blank">TR-064 Debug</a>
        </div>

        <hr>

        <h3>Camera Settings</h3>
        <label><strong>Frame Size</strong></label>
        <select id="framesize">
            <option value="5">QVGA (320x240)</option>
            <option value="6">CIF (400x296)</option>
            <option value="7" selected>HVGA (480x320)</option>
            <option value="8">VGA (640x480)</option>
        </select>

        <label><strong>JPEG Quality</strong></label>
        <input type="range" id="quality" min="4" max="63" value="10">

        <div>
            <button class="button" onclick="applySettings()">Apply Settings</button>
            <button class="danger-btn" onclick="location.href='/forget';">Forget WiFi</button>
        </div>
    </div>

    <div class="log-container" id="log"></div>

    <script>
        const logEl = document.getElementById("log");
        const statusEl = document.getElementById("cameraStatus");
        const darkToggle = document.getElementById("darkModeToggle");
        const rssiEl = document.getElementById("rssi");
        const uptimeEl = document.getElementById("uptime");

        function addLog(message) {
            const entry = document.createElement("div");
            entry.className = "log-entry";
            const ts = new Date().toLocaleTimeString();
            entry.textContent = `[${ts}] ${message}`;
            logEl.prepend(entry);
        }

        function setDarkMode(enabled) {
            document.body.classList.toggle("dark-mode", enabled);
        }

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
                });
        }

        function fetchDeviceStatus() {
            fetch("/deviceStatus")
                .then(r => r.json())
                .then(data => {
                    rssiEl.textContent = data.rssi;
                    uptimeEl.textContent = data.uptime;
                })
                .catch(() => {
                    rssiEl.textContent = "--";
                    uptimeEl.textContent = "--";
                });
        }

        function testRing() {
            fetch("/ring")
                .then(r => r.text())
                .then(text => addLog(text))
                .catch(() => addLog("Failed to trigger ring"));
        }

        function applySettings() {
            const fs = document.getElementById("framesize").value;
            const q = document.getElementById("quality").value;
            Promise.all([
                fetch(`/control?var=framesize&val=${fs}`),
                fetch(`/control?var=quality&val=${q}`)
            ]).then(() => {
                addLog(`Applied camera settings: framesize=${fs}, quality=${q}`);
                fetchStatus();
            }).catch(() => addLog("Failed to apply camera settings"));
        }

        fetchStatus();
        fetchDeviceStatus();
        setInterval(fetchDeviceStatus, 5000);
        addLog("UI loaded");
    </script>
</body>
</html>
)rawliteral");
      });

      server.on("/tr064", HTTP_GET, [](AsyncWebServerRequest *request) {
        Preferences prefs;
        prefs.begin("tr064", true);
        String user = prefs.getString("user", "");
        String pass = prefs.getString("pass", "");
        String number = prefs.getString("number", "");
        prefs.end();

        String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>TR-064 Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <h1>☎️ TR-064 Setup</h1>
        <p>Configure FRITZ!Box credentials and the internal ring number.</p>

        <label><strong>Username:</strong></label>
        <input type="text" id="tr_user" value=")rawliteral" + user + R"rawliteral(">

        <label><strong>Password:</strong></label>
        <input type="password" id="tr_pass" value=")rawliteral" + pass + R"rawliteral(">

        <label><strong>Internal Ring Number:</strong></label>
        <input type="text" id="tr_number" value=")rawliteral" + number + R"rawliteral(" placeholder="e.g., **9 or **610">

        <div>
            <button class="button" onclick="saveTr064()">💾 Save</button>
            <button class="button" onclick="testRing()">🔔 Test Ring</button>
            <a class="button danger-btn" href="/">Back</a>
        </div>
    </div>

    <script>
        function saveTr064() {
            let user = document.getElementById("tr_user").value;
            let pass = document.getElementById("tr_pass").value;
            let number = document.getElementById("tr_number").value;

            fetch("/saveTR064", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ "user": user, "pass": pass, "number": number })
            }).then(r => r.text())
              .then(t => alert(t))
              .catch(e => alert("Save failed: " + e));
        }

        function testRing() {
            fetch("/ring")
                .then(r => r.text())
                .then(t => alert(t))
                .catch(e => alert("Ring failed: " + e));
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
        String user = prefs.getString("user", "");
        String number = prefs.getString("number", "");
        prefs.end();
        String gateway = WiFi.gatewayIP().toString();

        String json = "{";
        json += "\"gateway\":\"" + gateway + "\",";
        json += "\"user\":\"" + user + "\",";
        json += "\"number\":\"" + number + "\",";
        json += "\"configured\":" + String(hasTr064Config(tr064Config) ? "true" : "false");
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

          String user = doc["user"].as<String>();
          String pass = doc["pass"].as<String>();
          String number = doc["number"].as<String>();

          Preferences prefs;
          prefs.begin("tr064", false);
          if (number.isEmpty() && user.isEmpty() && pass.isEmpty()) {
            prefs.remove("user");
            prefs.remove("pass");
            prefs.remove("number");
            prefs.end();
            request->send(200, "text/plain", "TR-064 settings cleared");
            return;
          }

          prefs.putString("user", user);
          prefs.putString("pass", pass);
          prefs.putString("number", number);
          prefs.end();

          request->send(200, "text/plain", "TR-064 settings saved");
        }
      );

      server.on("/deviceStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"rssi\":" + String(WiFi.RSSI()) + ",\"uptime\":\"" + String(millis() / 1000) + "s\"}";
        request->send(200, "application/json", json);
      });

      server.on("/ring", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (triggerTr064Ring(tr064Config)) {
          request->send(200, "text/plain", "Ring triggered");
        } else {
          request->send(500, "text/plain", "Ring failed");
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
  // Trigger TR-064 ring alongside any future doorbell actions.
  if (triggerTr064Ring(tr064Config)) {
    Serial.println("📞 FRITZ!DECT ring triggered");
  } else {
    Serial.println("⚠️ FRITZ!DECT ring not triggered");
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
