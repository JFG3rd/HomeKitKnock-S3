#include "wifi_ap.h"
#include <ArduinoJson.h>

static bool apMode = false;

bool isAPModeActive() {
    return apMode;
}

void stopAPMode() {
    Serial.println("🛑 Stopping AP Mode...");
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    apMode = false;
    delay(500);
}

String generateWiFiSetupPage() {
    // Scan for WiFi networks
    Serial.println("📡 Scanning Wi-Fi networks...");
    std::vector<String> uniqueSSIDs;
    
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        // Check for duplicates
        bool found = false;
        for (const auto &existingSSID : uniqueSSIDs) {
            if (existingSSID == ssid) {
                found = true;
                break;
            }
        }
        if (!found && !ssid.isEmpty()) {
            uniqueSSIDs.push_back(ssid);
        }
    }

    // Build HTML page
    String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Doorbell Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="/style.css">
    <script>
        function connectWiFi() {
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

    // Add WiFi networks to dropdown
    for (size_t i = 0; i < uniqueSSIDs.size(); i++) {
        page += "            <option value='" + uniqueSSIDs[i] + "'>" + uniqueSSIDs[i] + "</option>\n";
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

    // Serve CSS file
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/css", R"rawliteral(
/* Inline CSS - In production, serve from SPIFFS */
:root {
    --primary-color: #007bff;
    --primary-hover: #0056b3;
    --danger-color: #dc3545;
    --light-bg: #F9F7C9;
}
body {
    font-family: Arial, sans-serif;
    text-align: center;
    background: var(--light-bg);
    margin: 0;
    padding: 20px;
}
h1 {
    color: #001F4D;
    text-shadow: 0 0 10px #FF4500;
}
.wifi-setup-container {
    max-width: 500px;
    margin: 50px auto;
    padding: 30px;
    background: white;
    border-radius: 10px;
    box-shadow: 0 4px 6px rgba(0,0,0,0.1);
}
select, input, button {
    width: 100%;
    padding: 12px;
    margin: 10px 0;
    border: 1px solid #ccc;
    border-radius: 4px;
    box-sizing: border-box;
}
button {
    background: var(--primary-color);
    color: white;
    border: none;
    cursor: pointer;
    font-size: 16px;
}
button:hover {
    background: var(--primary-hover);
}
.disclaimer {
    margin-top: 20px;
    font-size: 0.85em;
    color: #666;
}
)rawliteral");
    });

    // Root redirects to WiFi setup
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/wifiSetup");
    });

    // WiFi setup page
    server.on("/wifiSetup", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", generateWiFiSetupPage());
    });

    // WiFi status endpoint
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

    // Save WiFi credentials
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

    server.begin();
    Serial.println("✅ AP Mode web server started");
}

void attemptWiFiConnection(const String& ssid, const String& password) {
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
