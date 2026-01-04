#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
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
