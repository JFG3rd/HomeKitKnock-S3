#ifndef TR064_CLIENT_H
#define TR064_CLIENT_H

#include <Arduino.h>

// User-provided TR-064 credentials and internal ring number.
// Stored in NVS by the setup UI and loaded at boot.
struct Tr064Config {
  String username;
  String password;
  String number;
};

// Load TR-064 credentials from NVS into the provided config struct.
// Returns false if preferences could not be opened.
bool loadTr064Config(Tr064Config &config);

// Validate that all required fields are present.
bool hasTr064Config(const Tr064Config &config);

// Trigger an internal ring via TR-064 using the router gateway IP.
// Returns true if the TR-064 action succeeded.
bool triggerTr064Ring(const Tr064Config &config);

#endif // TR064_CLIENT_H
