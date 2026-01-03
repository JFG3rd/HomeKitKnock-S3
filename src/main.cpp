#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "wifi_ap.h"

// Global objects
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String wifiSSID, wifiPassword;

bool initFileSystem(AsyncWebServer &server) {
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

  initFileSystem(server);

  // Load WiFi credentials from preferences
  preferences.begin("wifi", true);
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();

  if (wifiSSID.isEmpty()) {
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
    <div class="container">
        <h1>🔔 ESP32-S3 Doorbell</h1>
        <p>System Online</p>
        <p><strong>WiFi:</strong> Connected</p>
        <p><strong>IP:</strong> )rawliteral" + WiFi.localIP().toString() + R"rawliteral(</p>
        <p><strong>Signal:</strong> )rawliteral" + String(WiFi.RSSI()) + R"rawliteral( dBm</p>
        <hr>
        <button class="danger-btn" onclick="location.href='/forget';">
            Forget WiFi
        </button>
    </div>
</body>
</html>
)rawliteral");
      });

      server.on("/forget", HTTP_GET, [](AsyncWebServerRequest *request) {
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

  Serial.println("====================================");
  Serial.println("Setup complete!\n");
}

void loop() {
  // Handle DNS requests in AP mode
  if (isAPModeActive()) {
    dnsServer.processNextRequest();
  }
  
  // Add your doorbell logic here
  delay(10);
}
