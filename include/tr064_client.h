#ifndef TR064_CLIENT_H
#define TR064_CLIENT_H

#include <Arduino.h>

// User-provided TR-064 credentials and internal ring number.
// Stored in NVS by the setup UI and loaded at boot.
struct Tr064Config {
  String tr064_username;  // TR-064 SOAP username
  String tr064_password;  // TR-064 SOAP password
  String http_username;   // FRITZ!Box web UI username
  String http_password;   // FRITZ!Box web UI password
  String number;          // Internal ring number (e.g., **610)
};

// Load TR-064 credentials from NVS into the provided config struct.
// Returns false if preferences could not be opened.
bool loadTr064Config(Tr064Config &config);

// Validate that all required fields are present.
bool hasTr064Config(const Tr064Config &config);

// Validate that HTTP config is present.
bool hasHttpConfig(const Tr064Config &config);

// Trigger an internal ring via TR-064 using the router gateway IP.
// Returns true if the TR-064 action succeeded.
bool triggerTr064Ring(const Tr064Config &config);

// Trigger an internal ring via HTTP click-to-dial API.
// This is a simpler alternative that works on all FRITZ!Box models.
// Returns true if the HTTP request succeeded.
bool triggerHttpRing(const Tr064Config &config);

#endif // TR064_CLIENT_H
