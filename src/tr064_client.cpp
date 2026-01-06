/*
 * Project: HomeKitKnock-S3
 * File: src/tr064_client.cpp
 * Author: Jesse Greene
 */

#include "tr064_client.h"
#include <Preferences.h>
#include <WiFi.h>
#include <tr064.h>
#include <HTTPClient.h>
#include <MD5Builder.h>

// Default TR-064 service port used by FRITZ!Box routers.
static const uint16_t kTr064Port = 49000;

bool loadTr064Config(Tr064Config &config) {
  // Read-only access to saved credentials in NVS.
  Preferences prefs;
  if (!prefs.begin("tr064", true)) {
    return false;
  }
  config.tr064_username = prefs.getString("tr064_user", "");
  config.tr064_password = prefs.getString("tr064_pass", "");
  config.http_username = prefs.getString("http_user", "");
  config.http_password = prefs.getString("http_pass", "");
  config.number = prefs.getString("number", "");
  prefs.end();
  return true;
}

bool hasTr064Config(const Tr064Config &config) {
  // All fields must be present to perform a TR-064 call.
  return !config.tr064_username.isEmpty() && !config.tr064_password.isEmpty() && !config.number.isEmpty();
}

bool hasHttpConfig(const Tr064Config &config) {
  // HTTP only needs password and number (username can be empty for admin)
  return !config.http_password.isEmpty() && !config.number.isEmpty();
}

bool triggerTr064Ring(const Tr064Config &config) {
  // Ensure network connectivity before attempting a SOAP call.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ TR-064: WiFi not connected");
    return false;
  }
  if (!hasTr064Config(config)) {
    Serial.println("❌ TR-064: Config incomplete");
    return false;
  }

  // Use the gateway IP assigned by DHCP as the FRITZ!Box endpoint.
  String routerIp = WiFi.gatewayIP().toString();
  Serial.printf("📞 TR-064: Dialing %s on %s:%d\n", config.number.c_str(), routerIp.c_str(), kTr064Port);
  
  TR064 connection(kTr064Port, routerIp, config.tr064_username, config.tr064_password);
  connection.debug_level = connection.DEBUG_WARNING;  // Reduce verbosity now that HTTP works
  connection.init();

  // Try dialing the internal number directly
  Serial.printf("📞 TR-064: Attempting to dial %s\n", config.number.c_str());
  String params[][2] = {{"NewX_AVM-DE_PhoneNumber", config.number}};
  String req[][2] = {{}};
  
  bool ok = connection.action("urn:dslforum-org:service:X_VoIP:1", "X_AVM-DE_DialNumber", params, 1, req, 0);
  
  if (ok) {
    Serial.println("✅ TR-064: Dial command sent successfully");
    return true;
  }
  
  Serial.println("❌ TR-064: Dial command failed");
  return false;
}

// Get FRITZ!Box Session ID (SID) for web API authentication
String getFritzBoxSID(const String& routerIp, const String& username, const String& password) {
  HTTPClient http;
  
  // Step 1: Get challenge
  String url = "http://" + routerIp + "/login_sid.lua";
  http.begin(url);
  int httpCode = http.GET();
  String response = http.getString();
  http.end();
  
  if (httpCode != 200) {
    Serial.printf("❌ SID: Failed to get challenge (code %d)\n", httpCode);
    return "";
  }
  
  // Extract challenge from XML response
  int challengeStart = response.indexOf("<Challenge>") + 11;
  int challengeEnd = response.indexOf("</Challenge>");
  if (challengeStart < 11 || challengeEnd < 0) {
    Serial.println("❌ SID: Failed to parse challenge");
    return "";
  }
  
  String challenge = response.substring(challengeStart, challengeEnd);
  Serial.printf("🔑 SID: Got challenge: %s\n", challenge.c_str());
  
  // Step 2: Calculate response = MD5(challenge + "-" + MD5(password))
  // FRITZ!Box uses UTF-16LE encoding for password
  uint8_t passwordUtf16[256];
  int pwLen = 0;
  for (int i = 0; i < password.length() && i < 127; i++) {
    passwordUtf16[pwLen++] = (uint8_t)password[i];
    passwordUtf16[pwLen++] = 0;
  }
  
  // MD5 of UTF-16LE password
  MD5Builder md5;
  md5.begin();
  md5.add(passwordUtf16, pwLen);
  md5.calculate();
  String md5PasswordHex = md5.toString();
  
  Serial.printf("🔑 SID: Password MD5: %s\n", md5PasswordHex.c_str());
  
  // Build challenge-response string
  String challengeResponse = challenge + "-" + md5PasswordHex;
  Serial.printf("🔑 SID: Challenge-response: %s\n", challengeResponse.c_str());
  
  // MD5 of challenge-response
  md5.begin();
  md5.add(challengeResponse);
  md5.calculate();
  String responseHex = md5.toString();
  
  String finalResponse = challenge + "-" + responseHex;
  Serial.printf("🔑 SID: Final response: %s\n", finalResponse.c_str());
  
  // Step 3: Login with response
  // URL-encode username in case it has special characters
  String encodedUsername = username;
  encodedUsername.replace(" ", "%20");
  encodedUsername.replace("-", "%2D");
  
  String loginUrl = "http://" + routerIp + "/login_sid.lua?username=" + encodedUsername + "&response=" + finalResponse;
  Serial.printf("🔑 SID: Login URL: %s\n", loginUrl.c_str());
  http.begin(loginUrl);
  httpCode = http.GET();
  response = http.getString();
  http.end();
  
  Serial.printf("🔑 SID: Login response code: %d\n", httpCode);
  Serial.printf("🔑 SID: Login response (first 200 chars): %s\n", response.substring(0, 200).c_str());
  
  if (httpCode != 200) {
    Serial.printf("❌ SID: Login failed (code %d)\n", httpCode);
    return "";
  }
  
  // Extract SID
  int sidStart = response.indexOf("<SID>") + 5;
  int sidEnd = response.indexOf("</SID>");
  if (sidStart < 5 || sidEnd < 0) {
    Serial.println("❌ SID: Failed to parse SID");
    return "";
  }
  
  String sid = response.substring(sidStart, sidEnd);
  if (sid == "0000000000000000") {
    // Check if account is blocked
    int blockStart = response.indexOf("<BlockTime>") + 11;
    int blockEnd = response.indexOf("</BlockTime>");
    if (blockStart > 11 && blockEnd > 0) {
      String blockTime = response.substring(blockStart, blockEnd);
      Serial.printf("❌ SID: Account temporarily blocked for %s seconds\n", blockTime.c_str());
    } else {
      Serial.println("❌ SID: Authentication failed (wrong username/password)");
    }
    return "";
  }
  
  Serial.printf("✅ SID: Authenticated successfully: %s\n", sid.c_str());
  return sid;
}

bool triggerHttpRing(const Tr064Config &config) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ HTTP Ring: WiFi not connected");
    return false;
  }
  if (!hasHttpConfig(config)) {
    Serial.println("❌ HTTP Ring: Config incomplete (need HTTP password)");
    return false;
  }

  String routerIp = WiFi.gatewayIP().toString();
  
  Serial.printf("📞 HTTP Ring: Calling %s via click-to-dial\n", config.number.c_str());
  
  // Get session ID using HTTP credentials
  String sid = getFritzBoxSID(routerIp, config.http_username, config.http_password);
  if (sid.isEmpty()) {
    Serial.println("❌ HTTP Ring: Failed to get SID");
    return false;
  }
  
  // Make click-to-dial request with SID
  String url = "http://" + routerIp + "/fon_num/dial_foncalls.lua?sid=" + sid + "&clicktodial=" + config.number;
  Serial.printf("   URL: %s\n", url.c_str());
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  
  int httpCode = http.GET();
  String response = http.getString();
  http.end();
  
  Serial.printf("📞 HTTP Ring: Response code %d\n", httpCode);
  if (httpCode > 0 && response.length() < 500) {
    Serial.printf("   Response: %s\n", response.c_str());
  }
  
  if (httpCode == 200 || httpCode == 302 || httpCode == 303) {
    Serial.println("✅ HTTP Ring: Request sent successfully");
    return true;
  }
  
  Serial.printf("❌ HTTP Ring: Request failed with code %d\n", httpCode);
  return false;
}
