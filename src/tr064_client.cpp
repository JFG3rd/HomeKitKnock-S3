/*
 * Project: HomeKitKnock-S3
 * File: src/tr064_client.cpp
 * Author: Jesse Greene
 */

#include "tr064_client.h"
#include <Preferences.h>
#include <WiFi.h>
#include <tr064.h>
#include "logger.h"

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
  config.number = prefs.getString("number", "");
  prefs.end();
  return true;
}

bool hasTr064Config(const Tr064Config &config) {
  // All fields must be present to perform a TR-064 call.
  return !config.tr064_username.isEmpty() && !config.tr064_password.isEmpty() && !config.number.isEmpty();
}

bool triggerTr064Ring(const Tr064Config &config) {
  // Ensure network connectivity before attempting a SOAP call.
  if (WiFi.status() != WL_CONNECTED) {
    logEvent(LOG_ERROR, "❌ TR-064: WiFi not connected");
    return false;
  }
  if (!hasTr064Config(config)) {
    logEvent(LOG_ERROR, "❌ TR-064: Config incomplete");
    return false;
  }

  // Use the gateway IP assigned by DHCP as the FRITZ!Box endpoint.
  String routerIp = WiFi.gatewayIP().toString();
  logEvent(LOG_INFO, "📞 TR-064: Dialing " + config.number + " on " + routerIp + ":" + String(kTr064Port));
  
  TR064 connection(kTr064Port, routerIp, config.tr064_username, config.tr064_password);
  connection.debug_level = connection.DEBUG_WARNING;  // Reduce verbosity now that HTTP works
  connection.init();

  // Try dialing the internal number directly
  logEvent(LOG_INFO, "📞 TR-064: Attempting to dial " + config.number);
  String params[][2] = {{"NewX_AVM-DE_PhoneNumber", config.number}};
  String req[][2] = {{}};
  
  bool ok = connection.action("urn:dslforum-org:service:X_VoIP:1", "X_AVM-DE_DialNumber", params, 1, req, 0);
  
  if (ok) {
    logEvent(LOG_INFO, "✅ TR-064: Dial command sent successfully");
    return true;
  }
  
  logEvent(LOG_ERROR, "❌ TR-064: Dial command failed");
  return false;
}
