#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <vector>

// WiFi AP Configuration
#define AP_SSID "ESP32_Doorbell_Setup"
#define DNS_PORT 53

// Function declarations
void startAPMode(AsyncWebServer& server, DNSServer& dnsServer, Preferences& prefs);
void stopAPMode();
void attemptWiFiConnection(const String& ssid, const String& password);
String generateWiFiSetupPage();
bool isAPModeActive();

#endif // WIFI_AP_H
