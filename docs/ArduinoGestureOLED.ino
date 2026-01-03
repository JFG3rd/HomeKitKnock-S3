// Netbios name: esp32c6-CCB950



#include <vector>
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h> //Using 2.35.30
#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>. //ESP Async WebServer version: 3.7.6
#include <ArduinoJson.h>. //Using Arduinojson by Benoit Blanchon version 7.2.1
#include <DNSServer.h>
#include <RevEng_PAJ7620.h>. //Using version 1.5.0
// Define the sensor object
RevEng_PAJ7620 gestureSensor;
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h> //ESP Async WebServer version: 3.7.6
#include "SPIFFS.h"
#include "FS.h"


#include "nvs.h"
#include "nvs_flash.h"
unsigned long lastFlashUpdate = 0;
// Add after your includes
#define MAX_INIT_ATTEMPTS 3
#define I2C_SDA 4  // Adjust these pins according to your XIAO ESP32C6
#define I2C_SCL 5  // Adjust these pins according to your XIAO ESP32C6

AsyncEventSource events("/events");  // SSE Endpoint


#define GES_REACTION_TIME 500
#define GES_ENTRY_TIME 800
#define GES_QUIT_TIME 1000
#define MAX_GESTURES 9  // ✅ Define how many gestures we support

const char *configPageURL = "/config";  // Webpage for configuring gestures

// Initialize U8G2 display
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences preferences;
AsyncWebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

// Define physical reset button on GPIO2
const int resetButtonPin = 2;
String wifiSSID, wifiPassword;
bool isAPMode = false;
String lastGesture = "None";

// Increase JSON document size for web server
#define JSON_DOC_SIZE 16384
#define CHUNK_SIZE 4000

// Webhook storage for 9 gestures

#define MAX_LOG_ENTRIES 30  // Store the last 30 log messages
String logBuffer[MAX_LOG_ENTRIES];
int logIndex = 0;

static String receivedData;

// Global array of gesture names – this is used everywhere in the code.
const char* gestureNames[MAX_GESTURES] = {
  "Right", "Left", "Up", "Down", "Forward", "Backward", "Clock", "AClock", "Wave"
};

bool initGestureSensor() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000); // Lower speed for reliability
    
    int attempts = 0;
    while (attempts < MAX_INIT_ATTEMPTS) {
        if (gestureSensor.begin()) {
            addLog("✅ Gesture Sensor Initialized Successfully! (Attempt " + String(attempts + 1) + ")");
            return true;
        }
        
        addLog("⚠️ Gesture Sensor init failed, attempt " + String(attempts + 1) + " of " + String(MAX_INIT_ATTEMPTS));
        Wire.end();
        delay(1000);
        Wire.begin(I2C_SDA, I2C_SCL);
        attempts++;
    }
    
    return false;
}
struct SystemState {
    bool gestureSensorOk = false;
    bool spiffsOk = false;
    bool wifiOk = false;
    unsigned long lastRecoveryAttempt = 0;
    int recoveryAttempts = 0;
    const int MAX_RECOVERY_ATTEMPTS = 3;
    const unsigned long RECOVERY_INTERVAL = 60000; // 1 minute between recovery attempts
};

SystemState systemState;

bool performSystemRecovery() {
    addLog("🔄 Attempting system recovery...");
    bool recoverySuccess = false;

    // Reset I2C bus
    Wire.end();
    delay(100);
    
    // Try to reinitialize gesture sensor
    if (!systemState.gestureSensorOk) {
        systemState.gestureSensorOk = initGestureSensor();
        if (systemState.gestureSensorOk) {
            addLog("✅ Gesture sensor recovered successfully");
        }
    }

    // Try to reinitialize SPIFFS
    if (!systemState.spiffsOk) {
        systemState.spiffsOk = initSPIFFS();
        if (systemState.spiffsOk) {
            addLog("✅ SPIFFS recovered successfully");
        }
    }

    // Try to reconnect WiFi if needed
    if (!systemState.wifiOk && !isAPMode) {
        attemptWiFiConnection();
        systemState.wifiOk = (WiFi.status() == WL_CONNECTED);
        if (systemState.wifiOk) {
            addLog("✅ WiFi recovered successfully");
        }
    }

    recoverySuccess = systemState.gestureSensorOk && 
                     systemState.spiffsOk && 
                     (systemState.wifiOk || isAPMode);

    if (recoverySuccess) {
        systemState.recoveryAttempts = 0;
        addLog("✅ System recovery completed successfully");
    } else {
        systemState.recoveryAttempts++;
        addLog("⚠️ System recovery partially failed. Attempt " + 
               String(systemState.recoveryAttempts) + " of " + 
               String(systemState.MAX_RECOVERY_ATTEMPTS));
    }

    return recoverySuccess;
}

bool initSPIFFS() {
  if (!SPIFFS.begin(true)) {
      Serial.println("❌ SPIFFS initialization failed!");
      return false;
  }
  Serial.println("✅ SPIFFS initialized successfully!");
  return true;
}

void saveActionsForGesture(const char* gesture, const String& actionsJson) {
  String filename = String("/") + gesture + ".json";
  File file = SPIFFS.open(filename, "w");
  if (!file) {
      addLog("❌ Failed to open " + filename + " for writing");
      return;
  }
  if (file.print(actionsJson)) {
      addLog("✅ Saved actions for " + String(gesture));
  } else {
      addLog("❌ Failed to write actions for " + String(gesture));
  }
  file.close();
}

String loadActionsForGesture(const char* gesture) {
  String filename = String("/") + gesture + ".json";
  if (!SPIFFS.exists(filename)) {
      return "[]";
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
      addLog("❌ Failed to open " + filename + " for reading");
      return "[]";
  }
  
  String actionsJson = file.readString();
  file.close();
  return actionsJson;
}

void clearAllGestures() {
  for (int i = 0; i < MAX_GESTURES; i++) {
      String filename = String("/") + gestureNames[i] + ".json";
      if (SPIFFS.exists(filename)) {
          SPIFFS.remove(filename);
      }
  }
  addLog("✅ All gesture configurations cleared");
}

/*
void saveActionsForGesture(const char* gesture, const String& actionsJson) {
  preferences.begin("gestures", false);
  
  // Clear existing chunks for this gesture first
  int chunkIndex = 0;
  while(preferences.isKey((String(gesture) + "_" + chunkIndex).c_str())) {
    preferences.remove((String(gesture) + "_" + chunkIndex).c_str());
    chunkIndex++;
  }

  // Now save new data in chunks of <= 4000 bytes
  int totalLength = actionsJson.length();
  chunkIndex = 0;

  for (int i = 0; i < totalLength; i += CHUNK_SIZE) {
    String chunk = actionsJson.substring(i, min(i + CHUNK_SIZE, totalLength));
    preferences.putString((String(gesture) + "_" + chunkIndex).c_str(), chunk);
    chunkIndex++;
  }

  preferences.end();
  addLog("✅ Saved gesture '" + String(gesture) + "' in chunks");
}



String loadActionsForGesture(const char* gesture) {
  preferences.begin("gestures", true);

  String fullActions = "";
  int chunkIndex = 0;
  String chunkKey = String(gesture) + "_" + chunkIndex;

  while (preferences.isKey(chunkKey.c_str())) {
    fullActions += preferences.getString(chunkKey.c_str(), "");
    chunkIndex++;
    chunkKey = String(gesture) + "_" + chunkIndex;
  }

  preferences.end();

  if (fullActions == "") fullActions = "[]";  // Default empty JSON array
  addLog("✅ Loaded gesture '" + String(gesture) + "' from chunks");
  return fullActions;
}
*/


String compressJson(const String& input) {
    String compressed = input;
    // Remove unnecessary whitespace and newlines
    compressed.replace(" ", "");
    compressed.replace("\n", "");
    // Remove redundant segments
    compressed.replace("{\"stop\":0},", "");
    // Remove the last empty segments
    compressed.replace(",{\"stop\":0}]}", "]}");
    return compressed;
}

String decompressJson(const String& input) {
    return input; // We only need to decompress if using actual compression
}

// Function to store logs in a circular buffer
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "00:00:00";
  }

  char buffer[20];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);  // Format: HH:MM:SS
  return String(buffer);
}

void addLog(const String &message) {
  String timestampedMessage = "[" + getTimestamp() + "] " + message;

  logBuffer[logIndex] = timestampedMessage;     // Overwrite oldest message
  logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;  // Move to the next index

  Serial.println(timestampedMessage);                              // Print to Serial Monitor
  events.send(timestampedMessage.c_str(), "logUpdate", millis());  // Send SSE update
}

void displayLogsOnTFT() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_GREEN);
  tft.setTextSize(1);

  int y = 0;
  for (int i = 0; i < MAX_LOG_ENTRIES; ++i) {
    int idx = (logIndex + i) % MAX_LOG_ENTRIES;
    if (logBuffer[idx].length() > 0) {
      tft.setCursor(0, y);
      tft.println(logBuffer[idx]);
      y += 10;
      if (y > tft.height() - 10) break;
    }
  }
}


// Function to update OLED display
void updateDisplay(const char *status, const char *gesture, const char *action, const char *ipAddress) {
  static String lastDisplayStatus = "";
  static String lastDisplayGesture = "";
  static String lastDisplayAction = "";
  static String lastDisplayIP = "";
  static unsigned long scrollStartTime = 0;
  static int scrollOffsets[4] = {0, 0, 0, 0};  // For status, gesture, action, IP
  const int maxWidth = 128;  // Screen width in pixels
  const int scrollSpeed = 100; // ms between scroll shifts

  // Ensure Default Values
  if (gesture == nullptr || strlen(gesture) == 0) gesture = "Waiting for Gesture...";
  if (action == nullptr || strlen(action) == 0) action = "No Action";

  // If unchanged, return early
  if (lastDisplayStatus == status && lastDisplayGesture == gesture &&
      lastDisplayAction == action && lastDisplayIP == ipAddress) {
    // Allow scrolling even if text is same
    if (millis() - scrollStartTime < scrollSpeed) return;
  } else {
    // Reset scroll state on content change
    lastDisplayStatus = status;
    lastDisplayGesture = gesture;
    lastDisplayAction = action;
    lastDisplayIP = ipAddress;
    for (int i = 0; i < 4; i++) scrollOffsets[i] = 0;
  }

  scrollStartTime = millis(); // update scroll timer
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);

  const char* texts[4] = {status, gesture, action, ipAddress};
  const int yPositions[4] = {10, 25, 40, 55};

  for (int i = 0; i < 4; i++) {
    int textWidth = u8g2.getStrWidth(texts[i]);

    if (textWidth > maxWidth) {
      // Scroll
      u8g2.drawStr(5 - scrollOffsets[i], yPositions[i], texts[i]);
      scrollOffsets[i]++;
      if (scrollOffsets[i] > textWidth) {
        scrollOffsets[i] = -maxWidth; // restart scroll
      }
    } else {
      // No scroll needed
      u8g2.drawStr(5, yPositions[i], texts[i]);
    }
  }

  u8g2.sendBuffer();
  Serial.printf("📟 OLED Updated: [%s | %s | %s | %s]\n", status, gesture, action, ipAddress);
}
/*
void updateDisplay(const char *status, const char *gesture, const char *action, const char *ipAddress) {
  static String lastDisplayStatus = "";
  static String lastDisplayGesture = "";
  static String lastDisplayAction = "";
  static String lastDisplayIP = "";

  // ✅ Ensure Default Values
  if (gesture == nullptr || strlen(gesture) == 0) gesture = "Waiting for Gesture...";
  if (action == nullptr || strlen(action) == 0) action = "No Action";

  // ✅ Prevent unnecessary redraws
  if (lastDisplayStatus == status && lastDisplayGesture == gesture && lastDisplayAction == action && lastDisplayIP == ipAddress) {
    return;
  }

  lastDisplayStatus = status;
  lastDisplayGesture = gesture;
  lastDisplayAction = action;
  lastDisplayIP = ipAddress;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);

  u8g2.drawStr(5, 10, status);
  u8g2.drawStr(5, 25, gesture);
  u8g2.drawStr(5, 40, action);
  u8g2.drawStr(5, 55, ipAddress);

  u8g2.sendBuffer();

  Serial.printf("📟 OLED Updated: [%s | %s | %s | %s]\n", status, gesture, action, ipAddress);
}
*/


// Function to send a HTTP Push when a gesture is detected

void executeGestureActions(const String &gesture) {
  addLog("🚀 Executing actions for: " + gesture);
  JsonDocument doc;
  String actionsJson = loadActionsForGesture(gesture.c_str());
  addLog("📄 Retrieved actions: " + actionsJson);

  DeserializationError error = deserializeJson(doc, actionsJson);
  if (error || !doc.is<JsonArray>()) {
    addLog("❌ Invalid action data for gesture: " + gesture);
    updateDisplay("Action Error", gesture.c_str(), "Invalid data", WiFi.localIP().toString().c_str());
    return;
  }

  JsonArray actions = doc.as<JsonArray>();
  if (actions.size() == 0) {
    addLog("⚠️ No actions configured for gesture: " + gesture);
    updateDisplay("No Actions Configured", gesture.c_str(), "", WiFi.localIP().toString().c_str());
    return;
  }

  for (JsonObject action : actions) {
    String url = action["url"].as<String>();
    String body = action["body"].as<String>();
    String actionName = action["actionName"].as<String>();
    int delayMs = action["delay"].as<int>();  // Declare it only once

    if (WiFi.status() == WL_CONNECTED && !url.isEmpty()) {
      HTTPClient http;
      http.begin(url);
      http.addHeader("Content-Type", "application/json");

      int httpResponseCode = http.POST(body);
      String logMessage = "✅ [" + gesture + "] 🎬 [" + actionName + "] 🛜 " + body + " ➡️ " + url + " ➡️ Response: " + String(httpResponseCode);
      addLog(logMessage);

      http.end();
      updateDisplay("Gesture Executed", gesture.c_str(), actionName.c_str(), WiFi.localIP().toString().c_str());
    } else {
      addLog("❌ Wi-Fi not connected or invalid URL for gesture: " + gesture);
      updateDisplay("Wi-Fi Error", gesture.c_str(), actionName.c_str(), WiFi.localIP().toString().c_str());
    }

    delay(delayMs);  // Use the existing variable, don't redeclare it
  }

  events.send(gesture.c_str(), "gestureUpdate", millis());
}




// 🌐 Start AP Mode & Serve Wi-Fi Setup Page
void startAPMode() {
  Serial.println("🚨 startAPMode() No Wi-Fi credentials. Starting AP Mode...");
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);

  bool apStarted = WiFi.softAP("JFG_Gesture_Setup");
  if (!apStarted) {
    Serial.println("❌ Failed to start AP mode! Restarting...");
    delay(2000);
    ESP.restart();
  }

  isAPMode = true;
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.printf("📡 startAPMode() AP Mode Active. Connect to: %s\n", WiFi.softAPIP().toString().c_str());
  updateDisplay("JFG Gesture Control", "Houston we have a problem", "Wi-Fi Setup", WiFi.softAPIP().toString().c_str());

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->redirect("/wifiSetup");
  });

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  server.on("/wifiSetup", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("📡 startAPMode() Scanning Wi-Fi networks...");

    // Set to store unique SSIDs
    std::vector<String> uniqueSSIDs;

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      // Only add SSID if it's not already in the list
      if (std::find(uniqueSSIDs.begin(), uniqueSSIDs.end(), ssid) == uniqueSSIDs.end()) {
        uniqueSSIDs.push_back(ssid);
      }
    }

    // Start generating the HTML page
    String page = R"rawliteral(
            <!DOCTYPE html>
            <html>
            <head>
                <title>JFG Gesture Controller</title>
                <meta name="viewport" content="width=device-width, initial-scale=1">
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
                          .then(text => alert(text + "\\nESP32 will restart now."))
                          .then(() => setTimeout(() => location.reload(), 3000));
                    }
                </script>
                <style>
                    body { font-family: Arial, sans-serif; text-align: center; background: #f4f4f4; }
                    .container { max-width: 400px; margin: auto; padding: 20px; background: white; border-radius: 10px; }
                    select, input, button { width: 100%; padding: 10px; margin: 10px 0; }
                    button { background: #007bff; color: white; border: none; cursor: pointer; }
                    button:hover { background: #0056b3; }
                </style>
            </head>
            <body>
                <div class="container">
                    <h1>JFG ESP32 Wi-Fi Setup</h1>
                    <label>Select SSID:</label>
                    <select id="ssid">
        )rawliteral";

    for (size_t i = 0; i < uniqueSSIDs.size(); i++) {
      page += "<option value='" + uniqueSSIDs[i] + "'>" + uniqueSSIDs[i] + "</option>";
    }


    // Continue with manual SSID entry
    page += R"rawliteral(
                    </select>
                    <label>Or Enter SSID:</label>
                    <input type="text" id="manual_ssid" placeholder="Enter SSID (if hidden)">
                    <script>
                        document.getElementById("manual_ssid").addEventListener("input", function() {
                            document.getElementById("ssid").value = this.value;
                        });
                    </script>
                    <label>Password:</label>
                    <input type="password" id="password" placeholder="Enter Wi-Fi Password">
                    <button onclick="connectWiFi()">Save & Connect</button>
                </div>
            </body>
            </html>
        )rawliteral";

    request->send(200, "text/html", page);
  });

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  server.on("/wifiStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["status"] = WiFi.status();

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  server.on(
    "/saveWiFi", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      String receivedData = String((char *)data).substring(0, len);
      Serial.println("📥 startAPMode() /saveWiFi Received Wi-Fi Config: " + receivedData);

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, receivedData);

      if (error) {
        request->send(400, "text/plain", "❌ Invalid JSON format");
        return;
      }

      String newSSID = doc["ssid"].as<String>();
      String newPassword = doc["password"].as<String>();

      if (newSSID.isEmpty()) {
        request->send(400, "text/plain", "❌ SSID cannot be empty");
        return;
      }

      preferences.begin("wifi", false);
      preferences.putString("ssid", newSSID);
      preferences.putString("password", newPassword);
      preferences.end();

      Serial.println("✅ startAPMode() Wi-Fi credentials saved! Restarting...");
      request->send(200, "text/plain", "✅ Wi-Fi credentials saved!");
      delay(1000);
      ESP.restart();
    });

  server.begin();
}

// Function to stop AP mode and switch back to Wi-Fi client mode
void stopAPMode() {
  addLog("🛑 Stopping AP Mode...");
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  isAPMode = false;
  delay(500);
}

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


// Function to start the web server and define routes
void serveWebServer() {
  Serial.println("Initializing Main Web Server (serveWebServer)...");
  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray jsonArray = doc.to<JsonArray>();

    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
      int index = (logIndex + i) % MAX_LOG_ENTRIES;
      if (logBuffer[index].length() > 0) {
        jsonArray.add(logBuffer[index]);
      }
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/wifiStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["status"] = WiFi.status();

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });


    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", R"rawliteral(
          <!DOCTYPE html>
          <html>
          <head>
              <title>ESP32 Gesture Control</title>
              <meta charset="UTF-8">
              <style>
                  body {
                      font-family: "Arial Unicode MS", "Noto Sans", "Segoe UI Emoji", Arial, sans-serif;
                      text-align: center;
                      background-color: #F9F7C9;
                      margin: 0;
                      padding: 20px;
                  }

                  h1 {
                      color: #001F4D;
                  }

                  /* ✅ System Logs Container */
                  :root {
                      --max-log-entries: 30; /* ✅ Ensures exactly 30 log entries */
                      --log-line-height: 24px; /* ✅ Approximate height of each log line */
                  }

                  /* ✅ System Logs Container - Equal Margins, Dynamic Height */
                  .log-container {
                      width: calc(100% - 40px); /* ✅ Ensures equal left & right margins */
                      max-width: 100%;
                      margin: 20px auto; /* ✅ Adds equal margin (left, right, and bottom) */
                      background: #001F4D;
                      padding: 10px;
                      border-radius: 5px;
                      overflow-y: auto;
                      text-align: left;
                      white-space: nowrap;
                      font-size: 12px;
                      font-family: monospace;
                      color: white;

                      /* ✅ Dynamically adjust height based on window size */
                      max-height: calc(var(--log-line-height) * var(--max-log-entries));

                      /* ✅ Ensure spacing from footer */
                      margin-bottom: 50px;
                  }

                  /* ✅ Ensure logs shrink proportionally when resizing */
                  @media screen and (max-height: 800px) {
                      .log-container {
                          max-height: calc(50vh); /* ✅ Logs take up 50% of the window height */
                      }
                  }

                  /* ✅ Scrollbar Styling for Normal Mode */
                  .log-container::-webkit-scrollbar {
                      width: 10px;
                  }

                  .log-container::-webkit-scrollbar-track {
                      background: #333; /* ✅ Darker track for better contrast */
                  }

                  .log-container::-webkit-scrollbar-thumb {
                      background: #555; /* ✅ Darker thumb for better contrast */
                      border-radius: 5px;
                  }

                  /* ✅ Dark Mode Scrollbar */
                  .dark-mode .log-container::-webkit-scrollbar-track {
                      background: #222; /* ✅ Darker track for dark mode */
                  }

                  .dark-mode .log-container::-webkit-scrollbar-thumb {
                      background: #888; /* ✅ Darker thumb for dark mode */
                  }

                  /* ✅ Alternate Background Colors for Log Entries */
                  .log-entry:nth-child(odd) {
                      background: black;
                      color: white;
                  }
                  .log-entry:nth-child(even) {
                      background: #001F4D;
                      color: white;
                  }

                  /* ✅ Remove Border from Log Entries */
                  .log-entry {
                      padding: 5px;
                      border: none;
                  }

                  /* ✅ Footer & Disclaimer Fix */
                  footer {
                      position: relative;
                      bottom: 0;
                      width: 100%;
                      text-align: center;
                      font-size: 0.9em;
                      color: gray;
                      margin-top: 20px; /* ✅ Ensures spacing from logs */
                  }


                  /* ✅ Match Last Action Formatting with Last Gesture */
                  #gesture, #action {
                      font-size: 1.7em;
                      font-weight: bold;
                      color: #FF3131;
                  }



                  /* 🌙 Dark Mode Toggle Button */

                  /* 🔹 Move Dark Mode Toggle to Top Right */
                  .dark-mode-toggle {
                      position: absolute;
                      top: 10px;
                      right: 15px;
                      font-size: 0.9em; /* Slightly smaller */
                  }

                  .switch {
                      position: relative;
                      display: inline-block;
                      width: 34px;
                      height: 20px;
                  }

                  .switch input { display: none; }

                  .slider {
                    position: absolute;
                    cursor: pointer;
                    top: 0;
                    left: 0;
                    right: 0;
                    bottom: 0;
                    background-color: #bbb;
                    transition: .4s;
                    border-radius: 10px;
                  }

                  .slider:before {
                    position: absolute;
                    content: "";
                    height: 14px;
                    width: 14px;
                    left: 3px;
                    bottom: 3px;
                    background-color: white;
                    transition: .4s;
                    border-radius: 50%;
                    box-shadow: 0px 0px 6px rgba(0, 0, 0, 0.5);
                  }

                  input:checked + .slider {
                    background-color: #007bff;
                    box-shadow: 0px 0px 10px rgba(0, 123, 255, 0.8);
                  }

                  input:checked + .slider:before {
                    transform: translateX(18px);
                    }
                    h1 {
                      font-size: 2.2em; /* Larger title */
                      text-align: center;
                      color: #001F4D;  /* Deep rich blue */
                      text-shadow: 0 0 12px #FF4500, 0 0 20px #B22222, 0 0 30px #660000; /* Red neon glow */
                  }

                  .gesture-text {
                      font-size: 1.7em; /* Same font size */
                      font-weight: bold;
                      color: #FF3131; /* Bright Red */
                  }

                  /* ✅ Ensure Last Action has the same format as Last Gesture */
                  #gesture, #action {
                      font-size: 1.7em;
                      font-weight: bold;
                      color: #FF3131; /* Bright Red */
                    }
                  /* ✅ Blinking Effect (Handled via JavaScript) */
                  #gesture {
                      font-size: 1.7em;
                      font-weight: bold;
                      color: #FF3131; /* Bright Red for emphasis */
                  }
                  /* ✅ Smooth Button Hover Effect */
                  button {
                      background-color: #007bff;
                      color: white;
                      padding: 12px 20px;
                      border: none;
                      cursor: pointer;
                      font-size: 16px;
                      border-radius: 4px;
                      transition: background 0.3s;
                  }

                  button:hover {
                      background-color: #0056b3; /* Subtle darker blue */
                  }

                  /* Special Styling for "Forget Wi-Fi" Button */
                  .wifi-btn {
                      background-color: red;
                  }

                  .wifi-btn:hover {
                      background-color: darkred;
                  }

                  /* ✅ Disclaimer at the Bottom */
                  footer {
                      position: fixed;
                      bottom: 10px;
                      width: 100%;
                      text-align: center;
                      font-size: 0.9em;
                      color: gray;
                  }

                  /* 🌙 Dark Mode Styles */
                  .dark-mode {
                      background-color: #222;
                      color: #eee;
                  }
                  /* Dark Mode Title - Neon Blue Glow */
                  .dark-mode h1 {
                      color: #5AB1FF;  /* Bright electric blue */
                      text-shadow: 0 0 12px #FF4500, 0 0 24px #B22222, 0 0 36px #660000; /* Stronger glow in dark mode */
                  }

                  .dark-mode .log-container {
                      background: #001F4D !important; /* Force same dark blue */
                      color: white;                  }

                  .dark-mode .log-entry {
                      border-bottom: 1px solid #666;
                  }

                  .dark-mode button {
                      background-color: #008cff;  /* Brighter blue */
                      color: white;
                      border: 1px solid #ffffff;
                  }
                  .dark-mode button:hover {
                      background-color: #0066cc;  /* Deeper blue on hover */
                  }

              </style>
          </head>


          <body>
              <!-- Main Title -->
              <h1>JFG XIAO ESP32C6 Gesture Controller</h1>

              <!-- Last Gesture Display (Blinking Effect) -->
              <!-- Last Gesture Display -->
              <p class="gesture-text">Last Gesture: <span id="gesture">None</span></p>

              <!-- Last Action Display -->
              <p class="gesture-text">Last Action: <span id="action">None</span></p>

               <!-- Flash Statistics Display -->
               <div id='flashStats' class='flash-stats'>
                    Loading Flash Memory stats...
                </div>
              <!-- Buttons Section -->
              <a href='/config'><button class="config-btn">Configure Gestures</button></a>
              <button class="wifi-btn" onclick="forgetWiFi()">Forget Wi-Fi</button>

              <!-- Dark Mode Toggle (Top Right) -->
              <div class="dark-mode-toggle">
                  🌙 Dark Mode:
                  <label class="switch">
                      <input type="checkbox" id="darkModeToggle">
                      <span class="slider round"></span>
                  </label>
              </div>


              <!-- Logs Section -->
                <h3>&#128220; System Logs</h3>
                <div class="log-container" id="logContainer"></div>



                <!-- Disclaimer (Moved Below) -->
                <footer>
                    <p class="disclaimer">Designed & Developed by Jesse Greene</p>
                </footer>
                <script>
                    var eventSource = new EventSource("/events");

                    eventSource.addEventListener("flashStats", function(event) {
                        const data = JSON.parse(event.data);
                        const statsDiv = document.getElementById("flashStats");
                        statsDiv.innerHTML = `Flash Memory: ${data.used}/${data.total} used (${data.percent.toFixed(1)}%) - ${data.free} free entries`;
                    });


                    // ✅ Adjust Log Container to Full Width & Dynamic Height
                    function adjustLogSize() {
                            let logContainer = document.getElementById("logContainer");
                            let footerHeight = document.querySelector("footer").offsetHeight;
                            let maxEntries = 30; // ✅ Matches MAX_LOG_ENTRIES
                            let lineHeight = 24; // ✅ Approximate line height (px)

                            let availableHeight = window.innerHeight - footerHeight - 200; // ✅ Ensures space for other elements
                            let maxHeight = Math.min(lineHeight * maxEntries, availableHeight); // ✅ Prevents overlap

                            logContainer.style.width = "calc(100% - 40px)"; // ✅ Keeps equal margins
                            logContainer.style.maxHeight = maxHeight + "px"; // ✅ Maintains dynamic height
                        }
                    // ✅ Adjust log size when resizing the window
                    window.addEventListener("resize", adjustLogSize);
                    window.addEventListener("load", adjustLogSize);

                    // ✅ Function to Append Log Entries Efficiently
                    function addLogEntry(logText) {
                        let logContainer = document.getElementById("logContainer");

                        let logEntry = document.createElement("div");
                        logEntry.className = "log-entry";
                        logEntry.innerText = logText;

                        logContainer.appendChild(logEntry);

                        // ✅ Maintain MAX_LOG_ENTRIES Limit
                        while (logContainer.children.length > 30) {
                            logContainer.removeChild(logContainer.firstChild);
                        }

                        logContainer.scrollTop = logContainer.scrollHeight; // Auto-scroll
                    }

                    // ✅ Event Listener for Incoming Logs
                    eventSource.addEventListener("logUpdate", function(event) {
                        addLogEntry(event.data);
                    });

                    // ✅ Fetch Logs from Server on Page Load
                    function fetchLogs() {
                        fetch("/logs")
                            .then(response => response.json())
                            .then(data => {
                                let logContainer = document.getElementById("logContainer");
                                logContainer.innerHTML = ""; // Clear previous logs

                                let fragment = document.createDocumentFragment(); // Efficient DOM updates
                                data.forEach(log => {
                                    if (log.trim()) {
                                        let logEntry = document.createElement("div");
                                        logEntry.className = "log-entry";
                                        logEntry.innerText = log;
                                        fragment.appendChild(logEntry);
                                    }
                                });

                                logContainer.appendChild(fragment);
                                logContainer.scrollTop = logContainer.scrollHeight; // Auto-scroll to latest log
                            })
                            .catch(error => console.error("❌ Error loading logs:", error));
                    }

                    // ✅ Dark Mode Toggle Function
                    function setupDarkMode() {
                        const toggle = document.getElementById("darkModeToggle");
                        const body = document.body;

                        if (localStorage.getItem("darkMode") === "enabled") {
                            body.classList.add("dark-mode");
                            toggle.checked = true;
                        }

                        toggle.addEventListener("change", function () {
                            if (toggle.checked) {
                                body.classList.add("dark-mode");
                                localStorage.setItem("darkMode", "enabled");
                            } else {
                                body.classList.remove("dark-mode");
                                localStorage.setItem("darkMode", "disabled");
                            }
                        });
                    }

                    // ✅ Reset Wi-Fi Function
                    function forgetWiFi() {
                        if (!confirm("⚠ Reset Wi-Fi settings? This will restart the device.")) return;

                        fetch("/forgetWiFi", { method: "POST" })
                            .then(response => response.text())
                            .then(text => {
                                alert("✅ " + text);
                                setTimeout(() => location.reload(), 3000);
                            })
                            .catch(error => {
                                console.error("❌ Wi-Fi reset failed:", error);
                                alert("❌ Error resetting Wi-Fi!");
                            });
                    }

                    // ✅ Setup Everything on Page Load
                    window.onload = function () {
                        fetchLogs();       // Load logs from server
                        setupDarkMode();   // Apply dark mode settings
                        adjustLogSize();   // Adjust logs width & height

                        // ✅ Listen for Gesture & Action Updates
                        eventSource.addEventListener("gestureUpdate", event => {
                            document.getElementById("gesture").innerText = event.data;
                        });

                        eventSource.addEventListener("actionUpdate", event => {
                            document.getElementById("action").innerText = event.data;
                        });

                        // ✅ Adjust Log Size Dynamically When Window is Resized
                        window.addEventListener("resize", adjustLogSize);

                        // ✅ Blinking Effect for Gesture & Action
                        setInterval(() => {
                            let gestureElement = document.getElementById("gesture");
                            let actionElement = document.getElementById("action");

                            if (gestureElement.style.visibility === "hidden") {
                                gestureElement.style.visibility = "visible";
                                actionElement.style.visibility = "hidden";
                            } else {
                                gestureElement.style.visibility = "hidden";
                                actionElement.style.visibility = "visible";
                            }
                        }, 300); // Blinking every 300ms
                    };


                </script>
          </body>


          </html>
      )rawliteral");
  });

 /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
  // /config Handler

  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
   

server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
  // Open the 'gestures' preferences in read-only mode.
  preferences.begin("gestures", true);
  
  // Begin constructing the HTML page.
  String page = R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <title>Gesture Configuration</title>
      <style>
        body {
          font-family: "Arial Unicode MS", "Noto Sans", "Segoe UI Emoji", Arial, sans-serif;
          background-color: #FFF2C2;
          margin: 0;
          padding: 20px;
          text-align: center;
        }
        body.dark-mode {
          background-color: #222;
          color: #eee;
        }

        .container {
          width: 100%;
          max-width: 100%;
          margin: auto;
          padding: 20px;
          background: white;
          color: black;
          border-radius: 10px;
          box-sizing: border-box;
        }

        body.dark-mode .container {
          background: #333;
          color: #eee;
        }

        table {
            width: 100%;
            border-collapse: collapse;
            table-layout: fixed; /* This helps enforce column widths */
        }   

        th, td {
            padding: 10px;
            text-align: left;
            vertical-align: top;
            border: 1px solid #ccc;
        }
        .button-container button {
          background-color: #007bff;
          color: white;
          padding: 12px 20px;
          border: none;
          cursor: pointer;
          font-size: 16px;
          border-radius: 4px;
          margin: 5px;
          transition: background 0.3s;
        }
        .button-container button:hover {
          background-color: #0056b3;
        }
        .dark-mode-toggle {
          position: absolute;
          top: 10px;
          right: 15px;
          font-size: 0.9em;
        }
        .switch {
          position: relative;
          display: inline-block;
          width: 34px;
          height: 20px;
        }
        .switch input { display: none; }
        .slider {
          position: absolute;
          cursor: pointer;
          top: 0;
          left: 0;
          right: 0;
          bottom: 0;
          background-color: #bbb;
          transition: .4s;
          border-radius: 10px;
        }
        .slider:before {
          position: absolute;
          content: "";
          height: 14px;
          width: 14px;
          left: 3px;
          bottom: 3px;
          background-color: white;
          transition: .4s;
          border-radius: 50%;
        }
        input:checked + .slider {
          background-color: #007bff;
        }
        input:checked + .slider:before {
          transform: translateX(18px);
        }
        .log-container {
          width: calc(100% - 40px);
          max-width: 100%;
          margin: 20px auto;
          background: #001F4D;
          padding: 10px;
          border-radius: 5px;
          overflow-y: auto;
          text-align: left;
          white-space: nowrap;
          font-size: 12px;
          font-family: monospace;
          color: white;
          max-height: 300px;
        }
          .danger-button {
            background-color: #dc3545 !important;
        }
        .danger-button:hover {
            background-color: #c82333 !important;
        }

        /* Light mode row striping */
        tbody tr:nth-child(odd) {
          background-color: #f9f9f9;
        }
        tbody tr:nth-child(even) {
          background-color: #e9e9e9;
        }

        /* Dark mode row striping */
        body.dark-mode tbody tr:nth-child(odd) {
          background-color: #111;
        }
        body.dark-mode tbody tr:nth-child(even) {
          background-color: #002244;
        }

        .action-row {
          display: flex;
          align-items: flex-start;
          gap: 5px;
          margin-bottom: 5px;
        }

        .action-row.dragging {
          opacity: 0.5;
        }
        h1 {
          color: inherit;
        }

        input, textarea {
          background-color: transparent;
        }

        body.dark-mode h1 {
          color: #5AB1FF;  /* Bright electric blue */
          text-shadow:
            0 0 12px #FF4500,
            0 0 24px #B22222,
            0 0 36px #660000;
        }

        body.dark-mode .button-container button {
          background-color: #3399ff;
        }
        body.dark-mode .button-container button:hover {
          background-color: #267acc;
        }

        body.dark-mode input,
        body.dark-mode textarea {
          background-color: transparent;
          color: #eee;
          border: 1px solid #666;
        }


        body.dark-mode input:focus
        body.dark-mode input::placeholder,
        body.dark-mode textarea::placeholder {
          color: #aaa;
        }

        #flashStats {
          margin: 10px;
          padding: 10px;
          background-color: #f0f0f0;
          border-radius: 5px;
          color: #000;
        }

        body.dark-mode #flashStats {
          background-color: #333;
          color: #eee;
          border: 1px solid #555;
        }

        .flash-stats {
          margin: 10px;
          padding: 10px;
          background-color: #f0f0f0;
          border-radius: 5px;
          color: #000;
        }

        body.dark-mode .flash-stats {
          background-color: #333;
          color: #eee;
          border: 1px solid #555;
        }

        /* Light Mode striping for action-row divs */
        #configTable .action-row:nth-child(odd) {
          background-color: #f9f9f9;
        }
        #configTable .action-row:nth-child(even) {
          background-color: #e9e9e9;
        }

        /* Dark Mode striping for action-row divs */
        body.dark-mode #configTable .action-row:nth-child(odd) {
          background-color: #111;
          color: #eee;
        }
        body.dark-mode #configTable .action-row:nth-child(even) {
          background-color: #002244;
          color: #eee;
        }


      </style>
      <script>
        // First, establish SSE connection
        var eventSource = new EventSource('/events');
        
        // Flash Stats Handler
        eventSource.addEventListener("flashStats", function(event) {
            const data = JSON.parse(event.data);
            const statsDiv = document.getElementById("flashStats");
            if (statsDiv) {
                statsDiv.innerHTML = `Flash Memory: ${data.used}/${data.total} used (${data.percent.toFixed(1)}%) - ${data.free} free entries`;
            }
        });

        // Setup Dark Mode
        function setupDarkMode() {
            const toggle = document.getElementById("darkModeToggle");
            const body = document.body;
            
            if (localStorage.getItem("darkMode") === "enabled") {
                body.classList.add("dark-mode");
                toggle.checked = true;
            }

            toggle.addEventListener("change", function() {
                if (this.checked) {
                    body.classList.add("dark-mode");
                    localStorage.setItem("darkMode", "enabled");
                } else {
                    body.classList.remove("dark-mode");
                    localStorage.setItem("darkMode", "disabled");
                }
            });
        }

        // Button Handlers
        function clearGestures() {
            if (!confirm("⚠️ Are you sure you want to clear all gesture configurations?")) return;
            
            fetch('/clearGestures', { method: 'POST' })
                .then(response => response.text())
                .then(text => {
                    alert(text);
                    location.reload();
                })
                .catch(error => {
                    console.error("❌ Clear gestures failed:", error);
                    alert("❌ Error clearing gestures!");
                });
        }

        function saveConfig() {
            let configData = {};
            ["Right", "Left", "Up", "Down", "Forward", "Backward", "Clock", "AClock", "Wave"].forEach(gesture => {
                let actionsArray = [];
                document.querySelectorAll(`#${gesture}_actions_container div`).forEach(actionEl => {
                    let actionName = actionEl.querySelector('.actionName')?.value.trim() || "";
                    let url = actionEl.querySelector('.actionURL')?.value.trim() || "";
                    let body = actionEl.querySelector('.actionBody')?.value.trim() || "";
                    let delay = parseInt(actionEl.querySelector('.actionDelay')?.value || "500");

                    if (actionName || url) {
                        actionsArray.push({
                            actionName: actionName || "Unnamed",
                            url: url,
                            body: body,
                            delay: delay
                        });
                    }
                });
                if (actionsArray.length > 0) {
                    configData[gesture] = actionsArray;
                }
            });

            console.log("Saving configuration:", JSON.stringify(configData, null, 2));
            
            fetch('/saveConfig', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(configData)
            })
            .then(response => response.text())
            .then(text => {
                alert(text);
                location.reload();
            })
            .catch(error => {
                console.error('❌ Save Failed:', error);
                alert('❌ Error saving configurations!');
            });
        }

        function addActionRow(gesture) {
            let container = document.getElementById(gesture + "_actions_container");
            let row = document.createElement("div");
            row.setAttribute("draggable", "true");
            row.setAttribute("ondragstart", "dragStart(event)");
            row.setAttribute("ondragover", "dragOver(event)");
            row.setAttribute("ondrop", "drop(event)");
            row.setAttribute("ondragenter", "dragEnter(event)");
            row.setAttribute("ondragleave", "dragLeave(event)");
            row.innerHTML = `
                <input type='text' placeholder='Action Name' class='actionName' style='width:10%;'>
                <input type='text' placeholder='Server URL' class='actionURL' style='width:15%;'>
                <textarea placeholder='JSON Body' class='actionBody' rows='2' style='width:65%; resize:none; overflow:hidden;' oninput='autoResizeTextarea(this)'></textarea>
                <input type='number' placeholder='Delay (ms)' class='actionDelay' value='500' style='width:10%;'>
                <button class='small-button danger-button' onclick='this.parentElement.remove()'>❌ Delete</button>
            `;
            container.appendChild(row);
        }
        function exportConfig() {
            fetch('/exportGestures')
                .then(response => response.json())
                .then(data => {
                    // Create blob and download
                    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
                    const url = window.URL.createObjectURL(blob);
                    const a = document.createElement('a');
                    a.href = url;
                    a.download = 'gesture_config.json';
                    document.body.appendChild(a);
                    a.click();
                    window.URL.revokeObjectURL(url);
                    document.body.removeChild(a);
                })
                .catch(error => {
                    console.error('Export failed:', error);
                    alert('❌ Failed to export configuration');
                });
        }

        function importConfig(event) {
            const file = event.target.files[0];
            if (!file) return;

            const reader = new FileReader();
            reader.onload = function(e) {
                try {
                    // Validate JSON format
                    JSON.parse(e.target.result);
                    
                    // Send to server
                    fetch('/importGestures', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: e.target.result
                    })
                    .then(response => response.text())
                    .then(text => {
                        alert(text);
                        location.reload();
                    })
                    .catch(error => {
                        console.error('Import failed:', error);
                        alert('❌ Failed to import configuration');
                    });
                } catch (error) {
                    alert('❌ Invalid JSON file');
                }
            };
            reader.readAsText(file);
        }
        // Initialize everything when the page loads
        window.onload = function() {
            setupDarkMode();
            fetch('/flashStats');
            document.querySelectorAll("textarea.actionBody").forEach(autoResizeTextarea);
        };

        function autoResizeTextarea(textarea) {
            textarea.style.height = 'auto'; // Reset height
            textarea.style.height = textarea.scrollHeight + 'px'; // Set to content height
        }

        let dragSrcEl = null;

        function dragStart(e) {
            dragSrcEl = e.target;
            e.dataTransfer.effectAllowed = 'move';
            e.dataTransfer.setData('text/html', e.target.outerHTML);
            e.target.classList.add('dragging');
        }

        function dragOver(e) {
            e.preventDefault();
            e.dataTransfer.dropEffect = 'move';
        }

        function drop(e) {
            e.preventDefault();
            if (dragSrcEl !== e.target && e.target.classList.contains('action-row')) {
                dragSrcEl.parentNode.removeChild(dragSrcEl);
                let dropHTML = e.dataTransfer.getData('text/html');
                e.target.insertAdjacentHTML('beforebegin', dropHTML);

                let newEl = e.target.previousSibling;
                addDragHandlers(newEl);
            }
        }

        function dragEnter(e) {
            if (e.target.classList.contains('action-row')) {
                e.target.style.borderTop = "2px dashed #007bff";
            }
        }

        function dragLeave(e) {
            if (e.target.classList.contains('action-row')) {
                e.target.style.borderTop = "";
            }
        }

        function addDragHandlers(el) {
            el.classList.add('action-row');
            el.setAttribute("draggable", "true");
            el.setAttribute("ondragstart", "dragStart(event)");
            el.setAttribute("ondragover", "dragOver(event)");
            el.setAttribute("ondrop", "drop(event)");
            el.setAttribute("ondragenter", "dragEnter(event)");
            el.setAttribute("ondragleave", "dragLeave(event)");
        }


     </script>
    </head>
    <body>
      <div class="dark-mode-toggle">
        🌙 Dark Mode:
        <label class="switch">
          <input type="checkbox" id="darkModeToggle">
          <span class="slider round"></span>
        </label>
      </div>
      <div class="container">
        <h1>JFG XIAO ESP32C6 Gesture Controller - Configure Gesture Actions</h1>
    <div class="button-container">
        <button onclick="saveConfig()">💾 Save</button>
        <button onclick="window.location.href='/'">⬅️ Back to Main Page</button>
        <button class="danger-button" onclick="clearGestures()">❌ Clear Gestures</button>
        <button onclick="exportConfig()">📤 Export JSON</button>
        <input type="file" id="importFile" accept=".json" style="display:none;" onchange="importConfig(event)">
        <button onclick="document.getElementById('importFile').click();">📥 Import JSON</button>
</div>
        <div id='flashStats' class='flash-stats'>
            Loading Flash Memory stats...
        </div>
        <form onsubmit="event.preventDefault();">
          <table id="configTable">
            <thead>
              <tr>
                <th style="width: 10%;">Gesture</th>
                <th style="width: 90%;">Actions</th>
              </tr>
            </thead>
            <tbody>
  )rawliteral";
  
  for (int i = 0; i < MAX_GESTURES; i++) {
      String gesture = gestureNames[i];
      page += "<tr><td>" + gesture + "</td>";
      page += "<td>";
      page += "<div id='" + gesture + "_actions_container'>";

      // ✅ FIXED LINE: Explicitly using gesture.c_str()
      String actionsJson = loadActionsForGesture(gesture.c_str());

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, actionsJson);
      if (!error && doc.is<JsonArray>()) {
        JsonArray actions = doc.as<JsonArray>();
        for (JsonObject action : actions) {
          String actionName = action["actionName"].as<String>();
          String url = action["url"].as<String>();
          String body = action["body"].as<String>();
          int delay = action["delay"].as<int>();
          page += "<div class='action-row' draggable='true' ondragstart='dragStart(event)' ondragover='dragOver(event)' ondrop='drop(event)' ondragenter='dragEnter(event)' ondragleave='dragLeave(event)'>";
          page += "<input type='text' placeholder='Action Name' class='actionName' value='" + actionName + "' style='width:10%;'>";
          page += "<input type='text' placeholder='Server URL' class='actionURL' value='" + url + "' style='width:15%;'>";
          page += "<textarea placeholder='JSON Body' class='actionBody' style='width:65%; resize:vertical;' rows='2'>" + body + "</textarea>";
          page += "<input type='number' placeholder='Delay (ms)' class='actionDelay' value='" + String(delay) + "' style='width:7%;'>";
          page += "<button class='small-button danger-button' onclick='this.parentElement.remove()'>❌ Delete</button>";
          page += "</div>";
        }
      }
      page += "</div>";
      page += "<button class='small-button' onclick=\"addActionRow('" + gesture + "')\">➕ Add Action</button>";
      page += "</td></tr>";
  }



  page += R"rawliteral(
            </tbody>
          </table>
        </form>
      </div>
      <footer>
        <p>Designed & Developed by Jesse Greene</p>
      </footer>
    </body>
    </html>
  )rawliteral";
  
  preferences.end();
  request->send(200, "text/html", page);
});


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  server.on(
    "/saveConfig", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        static String receivedData;
        if (index == 0) receivedData = "";
        receivedData += String((char *)data).substring(0, len);

        if (index + len == total) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, receivedData);
            if (error) {
                addLog("❌ Invalid JSON format in saveConfig");
                request->send(400, "text/plain", "❌ Invalid JSON format");
                return;
            }

            // Debug log
            String debugJson;
            serializeJsonPretty(doc, debugJson);
            addLog("📥 Received configuration: " + debugJson);

            // First, verify all data before saving
            bool verificationPassed = true;
            for (const char* gestureName : gestureNames) {
                if (doc[gestureName].is<JsonArray>()) {
                    JsonArray actions = doc[gestureName].as<JsonArray>();
                    for (JsonObject action : actions) {
                        // Unescape the body string if it's escaped
                        String body = action["body"].as<String>();
                        body.replace("\\\"", "\""); // Remove extra escaping
                        action["body"] = body;
                    }
                    
                    String actionsJson;
                    serializeJson(actions, actionsJson);
                    /*
                    if (actionsJson.length() > 4096) {
                        addLog("❌ Config for " + String(gestureName) + " too large: " + String(actionsJson.length()) + " bytes");
                        verificationPassed = false;
                        break;
                    }
                    */
                }
            }  // End of first for loop

            if (!verificationPassed) {
                request->send(400, "text/plain", "❌ Configuration too large!");
                return;
            }

            // If verification passed, proceed with saving
            preferences.begin("gestures", false);
            preferences.clear(); // Clear existing data first

            for (const char* gestureName : gestureNames) {
                if (doc[gestureName].is<JsonArray>()) {
                    JsonArray actions = doc[gestureName].as<JsonArray>();
                    String actionsJson;
                    serializeJson(actions, actionsJson);
                    
                    //String compressedJson = compressJson(actionsJson);
                    
                    // Now use chunked storage
                    //saveActionsForGesture(gestureName, compressedJson);
                    saveActionsForGesture(gestureName, actionsJson);
                    addLog("✅ Saved " + String(gestureName));
                }
            }
            preferences.end();


            sendFlashStats(); // Update flash stats immediately
            addLog("✅ All configurations saved successfully");
            request->send(200, "text/plain", "✅ Configurations saved!");
        }
    });
    


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
  
  
  server.on("/exportGestures", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    
    for (int i = 0; i < MAX_GESTURES; i++) {
        String gestureKey = String(gestureNames[i]);
        String actionsJson = loadActionsForGesture(gestureKey.c_str());
        
        JsonDocument tempDoc;
        DeserializationError error = deserializeJson(tempDoc, actionsJson);
        if (!error) {
            doc[gestureKey] = tempDoc.as<JsonArray>();
        } else {
            doc[gestureKey] = JsonArray();
        }
    }
    
    String output;
    serializeJsonPretty(doc, output);
    request->send(200, "application/json", output);
});
  



  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  // JSON Import Endpoint
  server.on(
    "/importGestures", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      // Use a static string to accumulate the received data.
      static String receivedData;
      if (index == 0) receivedData = "";
      receivedData += String((char *)data).substring(0, len);
      
      // When the entire file is received...
      if (index + len == total) {
        JsonDocument doc;
        // Try to deserialize the incoming JSON.
        DeserializationError error = deserializeJson(doc, receivedData);
        if (error) {
          request->send(400, "text/plain", "❌ Invalid JSON file");
          return;
        }
        
        // Open preferences in write mode.
        preferences.begin("gestures", false);
        // Loop through each gesture in the global list.
        for (int i = 0; i < MAX_GESTURES; i++) {
          String gestureKey = String(gestureNames[i]);
          if (doc[gestureKey].is<JsonArray>()) {
            // Get the JSON array from the imported document.
            JsonArray actions = doc[gestureKey].as<JsonArray>();
            // Serialize the actions array to a string.
            String serializedActions;
            serializeJson(actions, serializedActions);
            // Save the serialized string to flash under the gesture key.
            saveActionsForGesture(gestureKey.c_str(), serializedActions);
            Serial.printf("📥 Imported: %s -> %s\n", gestureKey.c_str(), serializedActions.c_str());
          }
        }
        preferences.end();
        
        // Respond to the client indicating success.
        request->send(200, "text/plain", "✅ Gesture actions imported!");
      }
    });

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
  
    server.on("/clearGestures", HTTP_POST, [](AsyncWebServerRequest *request) {
      addLog("🚨 Clearing all gesture preferences...");
      clearAllGestures();
      request->send(200, "text/plain", "All gesture configurations cleared!");
  });


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
  server.on("/spiffsInfo", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["totalBytes"] = SPIFFS.totalBytes();
    doc["usedBytes"] = SPIFFS.usedBytes();
    doc["freeBytes"] = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
});
  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


  server.on("/debug/gestures", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    preferences.begin("gestures", true);
    
    for (const char* gesture : gestureNames) {
        String actionsJson = preferences.getString(gesture, "[]");
        JsonDocument tempDoc;
        deserializeJson(tempDoc, actionsJson);
        doc[gesture] = tempDoc.as<JsonArray>();
    }
    
    preferences.end();
    
    String response;
    serializeJsonPretty(doc, response);
    request->send(200, "application/json", response);
});


  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  // Serve the Wi-Fi configuration page in AP mode
/*
  server.on("/wifiSetup", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("📡 serveWebServer()/wifiSetup: Scanning Wi-Fi networks...");

    // Set to store unique SSIDs
    std::vector<String> uniqueSSIDs;

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);

      // ✅ Manually check for duplicates
      bool found = false;
      for (const auto &existingSSID : uniqueSSIDs) {
        if (existingSSID == ssid) {
          found = true;
          break;
        }
      }

      if (!found) {
        uniqueSSIDs.push_back(ssid);
      }
    }

    // Start generating the HTML page
    String page = R"rawliteral(
            <!DOCTYPE html>
            <html>
            <head>
                <title>JFG Gesture Controller</title>
                <meta name="viewport" content="width=device-width, initial-scale=1">
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
                          .then(text => alert(text + "\\nESP32 will restart now."))
                          .then(() => setTimeout(() => location.reload(), 3000));
                    }
                </script>
                <style>
                    body { font-family: Arial, sans-serif; text-align: center; background: #f4f4f4; }
                    .container { max-width: 400px; margin: auto; padding: 20px; background: white; border-radius: 10px; }
                    select, input, button { width: 100%; padding: 10px; margin: 10px 0; }
                    button { background: #007bff; color: white; border: none; cursor: pointer; }
                    button:hover { background: #0056b3; }
                </style>
            </head>
            <body>
                <div class="container">
                    <h1>JFG ESP32 Wi-Fi Setup</h1>
                    <label>Select SSID:</label>
                    <select id="ssid">
        )rawliteral";

    for (size_t i = 0; i < uniqueSSIDs.size(); i++) {
      page += "<option value='" + uniqueSSIDs[i] + "'>" + uniqueSSIDs[i] + "</option>";
    }


    // Continue with manual SSID entry
    page += R"rawliteral(
                    </select>
                    <label>Or Enter SSID:</label>
                    <input type="text" id="manual_ssid" placeholder="Enter SSID (if hidden)">
                    <script>
                        document.getElementById("manual_ssid").addEventListener("input", function() {
                            document.getElementById("ssid").value = this.value;
                        });
                    </script>
                    <label>Password:</label>
                    <input type="password" id="password" placeholder="Enter Wi-Fi Password">
                    <button onclick="connectWiFi()">Save & Connect</button>
                </div>
            </body>
            </html>
        )rawliteral";

    request->send(200, "text/html", page);
  });

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

/*
  server.on(
    "/saveWiFi", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      String receivedData = String((char *)data).substring(0, len);
      addLog("📥 Received Wi-Fi Config: " + receivedData);
      Serial.println("📡 serveWebServer()/saveWiFi: Saving Wi-Fi Setup...");

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, receivedData);

      if (error) {
        addLog("❌ Failed to parse Wi-Fi JSON");
        request->send(400, "text/plain", "Invalid JSON");
        return;
      }

      String newSSID = doc["ssid"].as<String>();
      String newPassword = doc["password"].as<String>();

      if (newSSID.isEmpty()) {
        request->send(400, "text/plain", "SSID cannot be empty");
        return;
      }

      preferences.begin("wifi", false);
      preferences.putString("ssid", newSSID);
      preferences.putString("password", newPassword);
      preferences.end();
      Serial.println("📡 serveWebServer()/saveWiFi: Wi-Fi credentials saved! Restarting ESP...");

      addLog("✅ Wi-Fi credentials saved! Restarting ESP...");
      request->send(200, "text/plain", "Wi-Fi credentials saved! Rebooting...");

      delay(1000);
      ESP.restart();  // ✅ Restart ESP32 to apply new Wi-Fi settings
    });

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
/*

  server.on("/forgetWiFi", HTTP_POST, [](AsyncWebServerRequest *request) {
    addLog("🚨 Forgetting Wi-Fi settings...");

    preferences.begin("wifi", false);
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();

    request->send(200, "text/plain", "Wi-Fi settings erased! Restarting...");

    delay(1000);
    ESP.restart();  // ✅ Restart ESP to enter AP mode
  });
*/
    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


    /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray jsonArray = doc.to<JsonArray>();

    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
      int index = (logIndex + i) % MAX_LOG_ENTRIES;
      if (logBuffer[index].length() > 0) {
        jsonArray.add(logBuffer[index]);
      }
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
    });

  server.addHandler(&events);  // Ensure events handler is registered!
  server.begin();
  Serial.println("Web server started successfully!");
}

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

c
  /*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

  void attemptWiFiConnection() {
    addLog("🔄 attemptWiFiConnection() Attempting to connect to Wi-Fi...");

    WiFi.disconnect(true, true);  // Ensure a clean start
    delay(1000);                  // Small delay for clean reconnection
    WiFi.mode(WIFI_STA);          // Set Wi-Fi mode to station
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    int retries = 60;  // Increase retries (30s timeout)
    while (WiFi.status() != WL_CONNECTED && retries-- > 0) {
      delay(500);
      Serial.printf("⏳ Connecting... (%d)\n", retries);
      updateDisplay("Connecting to Wi-Fi...", "...", "", "");
    }

    if (WiFi.status() == WL_CONNECTED) {
      addLog("✅ Wi-Fi Connected!");
      Serial.printf("🎉 Connected! IP Address: %s\n", WiFi.localIP().toString().c_str());

      stopAPMode();  // Stop AP mode since we are connected
      isAPMode = false;
      // Try to sync Time.

      configTime(0, 0, "pool.ntp.org", "time.nist.gov");

      Serial.print("⏳ Syncing Time...");
      struct tm timeinfo;
      if (!getLocalTime(&timeinfo)) {
        Serial.println("❌ Failed to obtain time.");
      } else {
        Serial.println("✅ Time Synchronized!");
      }

      serveWebServer();  // Start web server in normal mode

      //updateDisplay("JFG Gesture Controller", "Waiting for gesture", "No Action", WiFi.localIP().toString().c_str());
    } else {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n❌ Wi-Fi Connection Failed. Checking failure reason...");
        switch (WiFi.status()) {
          case WL_NO_SSID_AVAIL:
            Serial.println("⚠️ No SSID found. Check if the router is powered on.");
            break;
          case WL_CONNECT_FAILED:
            Serial.println("🚨 Connection failed. Incorrect password?");
            break;
          case WL_IDLE_STATUS:
            Serial.println("🔄 Connection idle. Waiting...");
            break;
          default:
            Serial.println("🛑 Unknown Wi-Fi error.");
        }
      }
      Serial.println("\n❌ Wi-Fi Connection Failed. Entering AP Mode.");
      startAPMode();
      updateDisplay("Wi-Fi Failed", "AP Mode Active", "", WiFi.softAPIP().toString().c_str());
    }
  }

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


void setup() {
    Serial.begin(115200);
    addLog("JFG ESP32 Gesture System Initializing...");

    // Initialize SPIFFS
    systemState.spiffsOk = initSPIFFS();
    if (!systemState.spiffsOk) {
        addLog("❌ Critical: SPIFFS initialization failed!");
        return;
    }

    u8g2.begin();
    pinMode(resetButtonPin, INPUT_PULLUP);
    updateDisplay("JFG Gesture Controller", "", "", "Initializing...");

    // Initialize Gesture Sensor
    systemState.gestureSensorOk = initGestureSensor();
    if (!systemState.gestureSensorOk) {
        addLog("❌ Critical: Gesture sensor initialization failed!");
        updateDisplay("Sensor Error", "Retrying...", "", "");
    }

    // Initialize WiFi
    WiFi.mode(WIFI_STA);
    preferences.begin("wifi", true);
    wifiSSID = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("password", "");
    preferences.end();

    if (wifiSSID.isEmpty()) {
        startAPMode();
        systemState.wifiOk = true; // AP mode is considered "ok"
    } else {
        attemptWiFiConnection();
        systemState.wifiOk = (WiFi.status() == WL_CONNECTED);
    }

    // Initial system check
    if (!systemState.gestureSensorOk || !systemState.spiffsOk || !systemState.wifiOk) {
        performSystemRecovery();
    } else {
                
          // Load Gesture Preferences after initialization
          for (int i = 0; i < MAX_GESTURES; i++) {
            String actionsJson = loadActionsForGesture(gestureNames[i]);
            if (actionsJson != "[]") {
              addLog("✔️ Loaded actions for " + String(gestureNames[i]));
            }
          }
          updateDisplay("JFG ESP32 Gesture System", "", "System initialized", WiFi.localIP().toString().c_str());
      }

}

/*
void setup() {
  Serial.begin(115200);


  addLog("JFG ESP32 Gesture System Initializing...");


    // Initialize SPIFFS
    if (!initSPIFFS()) {
      addLog("❌ Critical: SPIFFS initialization failed!");
      return;
  }
  u8g2.begin();
  pinMode(resetButtonPin, INPUT_PULLUP);

  updateDisplay("JFG Gesture Controller", "", "", "Connecting...");

  WiFi.mode(WIFI_STA);

  preferences.begin("wifi", true);  // Open in read-only mode
  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();
  String addLogMessage = "📌 Debug: Stored SSID = " + String(wifiSSID.c_str());
  addLog(addLogMessage);
  addLogMessage = "📌 Debug: Stored Password = " + String(wifiPassword.c_str());
  addLog(addLogMessage);  // Remove this in production



  if (wifiSSID.isEmpty()) {
    Serial.println("🚨 No saved Wi-Fi credentials. Entering AP Mode.");
    startAPMode();
  } else {
    addLog("🔎 Checking Saved Wi-Fi SSID...");
    Serial.printf("Saved Wi-Fi SSID: %s\n", wifiSSID.c_str());
    attemptWiFiConnection();
  }





  // Initialize Gesture Sensor using RevEng_PAJ7620
  Serial.println("🖐 Initializing Gesture Sensor...");
  if (gestureSensor.begin()) {  // success = 1 (true)
    Serial.println("✅ Gesture Sensor Initialized Successfully!");
    addLog("✅ Gesture Sensor Ready! Waiting for gesture...");
    updateDisplay("JFG Gesture Controller", "Waiting for Gesture...", "No Action", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("❌ Gesture Sensor Initialization Failed!");
    updateDisplay("❌ Sensor Error", "Check Connections", "No Action", WiFi.localIP().toString().c_str());
    addLog("❌ Gesture Sensor not initialized. Check wiring or I2C address.");
  }

  // Load Gesture Preferences after initialization


  for (int i = 0; i < MAX_GESTURES; i++) {
    String actionsJson = loadActionsForGesture(gestureNames[i]);
    if (actionsJson != "[]") {
      addLog("✔️ Loaded actions for " + String(gestureNames[i]));
    }
  }

}
*/

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

// Loop function continuously runs
void loop() {

  // Check if system needs recovery
  if ((!systemState.gestureSensorOk || !systemState.spiffsOk || !systemState.wifiOk) && 
      (millis() - systemState.lastRecoveryAttempt > systemState.RECOVERY_INTERVAL) && 
      (systemState.recoveryAttempts < systemState.MAX_RECOVERY_ATTEMPTS)) {
      
      systemState.lastRecoveryAttempt = millis();
      performSystemRecovery();
      
      if (systemState.recoveryAttempts >= systemState.MAX_RECOVERY_ATTEMPTS) {
          addLog("🚨 Maximum recovery attempts reached. System needs manual restart.");
          updateDisplay("System Error", "Manual Restart", "Required", "");
      }
  }


  if (isAPMode) {
    dnsServer.processNextRequest();
  }

  // Handle Reset Button
  static unsigned long buttonPressTime = 0;
  if (digitalRead(resetButtonPin) == LOW) {
    if (buttonPressTime == 0) {
      buttonPressTime = millis();
    } else if (millis() - buttonPressTime > 3000) {
      addLog("🚨 Reset Button Held! Erasing Wi-Fi...");
      preferences.begin("wifi", false);
      preferences.clear();
      preferences.end();
      delay(500);
      ESP.restart();
    }
  } else {
    buttonPressTime = 0;
  }

  // Memory Management: Periodically free up resources
  static unsigned long lastMemCheck = 0;
  if (millis() - lastMemCheck > 30000) {  // Every 30 seconds
    size_t freeHeap = ESP.getFreeHeap();
    addLog("📊 Free Heap: " + String(freeHeap) + " bytes");
    lastMemCheck = millis();
  }

  // Flash Stats Update
  if (millis() - lastFlashUpdate > 60000) {
    sendFlashStats();
    lastFlashUpdate = millis();
  }

  // Gesture Detection with Debouncing
  static unsigned long lastGestureTime = 0;
  static String lastProcessedGesture = "";
  
  if (millis() - lastGestureTime > 500) {  // Debounce time
    Gesture gesture = gestureSensor.readGesture();
    String currentGesture = "";

    switch (gesture) {
      case GES_RIGHT: currentGesture = "Right"; break;
      case GES_LEFT: currentGesture = "Left"; break;
      case GES_UP: currentGesture = "Up"; break;
      case GES_DOWN: currentGesture = "Down"; break;
      case GES_FORWARD: currentGesture = "Forward"; break;
      case GES_BACKWARD: currentGesture = "Backward"; break;
      case GES_CLOCKWISE: currentGesture = "Clock"; break;
      case GES_ANTICLOCKWISE: currentGesture = "AClock"; break;
      case GES_WAVE: currentGesture = "Wave"; break;
      default: break;
    }

    if (!currentGesture.isEmpty() && currentGesture != lastProcessedGesture) {
      lastProcessedGesture = currentGesture;
      lastGestureTime = millis();
      executeGestureActions(currentGesture);
    }
  }

  // WiFi Reconnection Logic
  static unsigned long lastWiFiCheck = 0;
  if (!isAPMode && millis() - lastWiFiCheck > 10000) {  // Check every 10 seconds
    if (WiFi.status() != WL_CONNECTED) {
      addLog("🔄 WiFi disconnected, attempting to reconnect...");
      attemptWiFiConnection();
    }
    lastWiFiCheck = millis();
  }

  // Small delay to prevent tight loops
  delay(50);  // Reduced from 200ms to improve responsiveness
}