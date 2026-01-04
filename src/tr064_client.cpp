#include "tr064_client.h"
#include <Preferences.h>
#include <WiFi.h>
#include <tr064.h>

// Default TR-064 service port used by FRITZ!Box routers.
static const uint16_t kTr064Port = 49000;

bool loadTr064Config(Tr064Config &config) {
  // Read-only access to saved credentials in NVS.
  Preferences prefs;
  if (!prefs.begin("tr064", true)) {
    return false;
  }
  config.username = prefs.getString("user", "");
  config.password = prefs.getString("pass", "");
  config.number = prefs.getString("number", "");
  prefs.end();
  return true;
}

bool hasTr064Config(const Tr064Config &config) {
  // All fields must be present to perform a TR-064 call.
  return !config.username.isEmpty() && !config.password.isEmpty() && !config.number.isEmpty();
}

bool triggerTr064Ring(const Tr064Config &config) {
  // Ensure network connectivity before attempting a SOAP call.
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (!hasTr064Config(config)) {
    return false;
  }

  // Use the gateway IP assigned by DHCP as the FRITZ!Box endpoint.
  String routerIp = WiFi.gatewayIP().toString();
  TR064 connection(kTr064Port, routerIp, config.username, config.password);
  connection.debug_level = connection.DEBUG_ERROR;
  connection.init();

  // Dial the internal number (group or handset) to trigger a ring.
  String params[][2] = {{"NewX_AVM-DE_PhoneNumber", config.number}};
  String req[][2] = {{}};
  return connection.action("X_VoIP:1", "X_AVM-DE_DialNumber", params, 1, req, 0);
}
