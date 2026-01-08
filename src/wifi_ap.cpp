/*
 * Project: HomeKitKnock-S3
 * File: src/wifi_ap.cpp
 * Author: Jesse Greene
 */

#include "wifi_ap.h"
#include <ArduinoJson.h>

// Tracks whether the device is currently running in AP provisioning mode.
static bool apMode = false;
// Cache of the most recent WiFi scan results to avoid blocking in request handlers.
static std::vector<String> cachedSSIDs;
static unsigned long lastScanMs = 0;
static bool scanInProgress = false;

// Kick off or complete an async scan and refresh cached results when ready.
static void refreshWiFiScanCache() {
    // If a scan is already running, poll for completion.
    int scanStatus = WiFi.scanComplete();
    if (scanStatus == WIFI_SCAN_RUNNING) {
        scanInProgress = true;
        return;
    }

    // If a scan finished, collect results and clear the scan buffer.
    if (scanStatus >= 0) {
        cachedSSIDs.clear();
        for (int i = 0; i < scanStatus; i++) {
            String ssid = WiFi.SSID(i);
            if (!ssid.isEmpty()) {
                bool found = false;
                for (const auto &existing : cachedSSIDs) {
                    if (existing == ssid) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    cachedSSIDs.push_back(ssid);
                }
            }
        }
        WiFi.scanDelete();
        scanInProgress = false;
        lastScanMs = millis();
        return;
    }

    // If no scan has run recently, start a new async scan.
    unsigned long nowMs = millis();
    if (!scanInProgress && (nowMs - lastScanMs > 15000)) {
        WiFi.scanNetworks(true);
        scanInProgress = true;
    }
}

// Force a new async scan on demand.
static void triggerWiFiRescan() {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    scanInProgress = true;
    lastScanMs = 0;
}

bool isAPModeActive() {
    // Expose AP mode state to the main loop.
    return apMode;
}

void stopAPMode() {
    // Cleanly tear down AP mode and switch back to STA-only.
    Serial.println("🛑 Stopping AP Mode...");
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    apMode = false;
    delay(500);
}

String generateWiFiSetupPage() {
    // Update the cached scan list without blocking the web request.
    refreshWiFiScanCache();

    // Build HTML page with WiFi and TR-064 settings.
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Doorbell Setup</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
    <script>
        function connectWiFi() {
            // Save WiFi credentials to NVS and reboot.
            let ssid = document.getElementById("ssid").value;
            let password = document.getElementById("password").value;

            if (ssid === "") {
                alert("⚠️ Please select or enter an SSID!");
                return;
            }

            fetch("/saveWiFi", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ "ssid": ssid, "password": password })
            }).then(response => response.text())
              .then(text => {
                  alert(text + "\nESP32 will restart now.");
                  setTimeout(() => location.reload(), 3000);
              })
              .catch(error => {
                  alert("Error saving WiFi credentials: " + error);
              });
        }

        function saveTr064() {
            // Save TR-064 credentials and ring number for FRITZ!Box.
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
            }).then(response => response.text())
              .then(text => {
                  alert(text);
              })
              .catch(error => {
                  alert("Error saving TR-064 settings: " + error);
              });
        }
        function rescanWiFi() {
            fetch("/scanWifi")
                .then(response => response.text())
                .then(text => {
                    alert(text + "\nRefresh the page in a few seconds.");
                })
                .catch(error => {
                    alert("Error starting WiFi scan: " + error);
                });
        }
    </script>
</head>
<body>
    <div class="wifi-setup-container">
        <h1>🔔 ESP32 Doorbell Wi-Fi Setup</h1>
        <p>Configure your doorbell's WiFi connection</p>
        
        <label><strong>Select Network:</strong></label>
        <select id="ssid">
            <option value="">-- Select WiFi Network --</option>
)rawliteral";

    // Add cached WiFi networks to dropdown.
    for (size_t i = 0; i < cachedSSIDs.size(); i++) {
        page += "            <option value='" + cachedSSIDs[i] + "'>" + cachedSSIDs[i] + "</option>\n";
    }

    if (cachedSSIDs.empty()) {
        page += "            <option value=\"\" disabled>(scanning...) refresh in a few seconds</option>\n";
    }

    page += R"rawliteral(
        </select>
        
        <label><strong>Or Enter SSID Manually:</strong></label>
        <input type="text" id="manual_ssid" placeholder="Enter SSID (if hidden)">
        <script>
            document.getElementById("manual_ssid").addEventListener("input", function() {
                document.getElementById("ssid").value = this.value;
            });
        </script>
        
        <label><strong>Password:</strong></label>
        <input type="password" id="password" placeholder="Enter Wi-Fi Password">
        
        <button onclick="connectWiFi()">💾 Save & Connect</button>
        <button onclick="rescanWiFi()">🔄 Rescan WiFi</button>

        <hr>

        <h3>FRITZ!Box TR-064</h3>
        <label><strong>TR-064 Username:</strong></label>
        <input type="text" id="tr064_user" placeholder="TR-064 username">

        <label><strong>TR-064 Password:</strong></label>
        <input type="password" id="tr064_pass" placeholder="TR-064 password">

        <label><strong>Internal Ring Number:</strong></label>
        <input type="text" id="tr_number" placeholder="e.g., **9 or **610">

        <button onclick="saveTr064()">💾 Save TR-064 Settings</button>
        
        <footer>
            <p class="disclaimer">ESP32-S3 HomeKit Doorbell</p>
        </footer>
    </div>
</body>
</html>
)rawliteral";

    return page;
}

void startAPMode(AsyncWebServer& server, DNSServer& dnsServer, Preferences& prefs) {
    // Start captive portal AP for provisioning.
    Serial.println("🚨 Starting AP Mode...");
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_AP_STA);

    bool apStarted = WiFi.softAP(AP_SSID);
    if (!apStarted) {
        Serial.println("❌ Failed to start AP mode! Restarting...");
        delay(2000);
        ESP.restart();
    }

    apMode = true;
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

    Serial.printf("📡 AP Mode Active. Connect to: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("    SSID: %s\n", AP_SSID);
    Serial.printf("    IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Root redirects to the setup page for a simple captive UX.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/wifiSetup");
    });

    // WiFi + TR-064 setup page.
    server.on("/wifiSetup", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateWiFiSetupPage());
    });

    // WiFi status endpoint for troubleshooting/UX.
    server.on("/wifiStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["ssid"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
        doc["status"] = WiFi.status();
        doc["apMode"] = apMode;
        doc["apIP"] = WiFi.softAPIP().toString();

        String jsonResponse;
        serializeJson(doc, jsonResponse);
        request->send(200, "application/json", jsonResponse);
    });

    // Save WiFi credentials to NVS.
    server.on("/saveWiFi", HTTP_POST, 
        [](AsyncWebServerRequest *request) {}, 
        NULL,
        [&prefs](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String receivedData = String((char *)data).substring(0, len);
            Serial.println("📥 Received Wi-Fi Config: " + receivedData);

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, receivedData);

            if (error) {
                Serial.println("❌ Failed to parse JSON");
                request->send(400, "text/plain", "Invalid JSON");
                return;
            }

            String newSSID = doc["ssid"].as<String>();
            String newPassword = doc["password"].as<String>();

            if (newSSID.isEmpty()) {
                request->send(400, "text/plain", "SSID cannot be empty");
                return;
            }

            prefs.begin("wifi", false);
            prefs.putString("ssid", newSSID);
            prefs.putString("password", newPassword);
            prefs.end();

            Serial.println("✅ Wi-Fi credentials saved! Restarting...");
            request->send(200, "text/plain", "✅ Wi-Fi credentials saved!");
            
            delay(1000);
            ESP.restart();
        }
    );

    // Save TR-064 credentials and ring number to NVS.
    server.on("/saveTR064", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [&prefs](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String receivedData = String((char *)data).substring(0, len);
            Serial.println("📥 Received TR-064 Config: " + receivedData);

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, receivedData);

            if (error) {
                Serial.println("❌ Failed to parse JSON");
                request->send(400, "text/plain", "Invalid JSON");
                return;
            }

            String tr064_user = doc["tr064_user"].as<String>();
            String tr064_pass = doc["tr064_pass"].as<String>();
            String number = doc["number"].as<String>();

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

            request->send(200, "text/plain", "TR-064 settings saved");
        }
    );

    // Trigger an async WiFi scan for the setup UI.
    server.on("/scanWifi", HTTP_GET, [](AsyncWebServerRequest *request) {
        triggerWiFiRescan();
        request->send(200, "text/plain", "WiFi scan started");
    });

    server.begin();
    Serial.println("✅ AP Mode web server started");
}

void attemptWiFiConnection(const String& ssid, const String& password) {
    // Best-effort STA connection with a bounded retry window.
    Serial.println("🔄 Attempting to connect to Wi-Fi...");
    
    WiFi.disconnect(true, true);
    delay(1000);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    int retries = 60; // 30 second timeout
    while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ WiFi Connected!");
        Serial.printf("   SSID: %s\n", WiFi.SSID().c_str());
        Serial.printf("   IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("   Signal: %d dBm\n", WiFi.RSSI());
        apMode = false;
    } else {
        Serial.println("❌ WiFi connection failed!");
        Serial.println("   Starting AP mode for configuration...");
    }
}
