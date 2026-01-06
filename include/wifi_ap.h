/*
 * Project: HomeKitKnock-S3
 * File: include/wifi_ap.h
 * Author: Jesse Greene
 */

#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <vector>

// WiFi AP configuration defaults for provisioning mode.
#define AP_SSID "ESP32_Doorbell_Setup"
#define DNS_PORT 53

// Start AP mode, DNS captive redirect, and configuration endpoints.
void startAPMode(AsyncWebServer& server, DNSServer& dnsServer, Preferences& prefs);
// Stop AP mode and return to STA-only mode.
void stopAPMode();
// Attempt STA connection with stored SSID/pass.
void attemptWiFiConnection(const String& ssid, const String& password);
// Build the WiFi + TR-064 setup HTML page.
String generateWiFiSetupPage();
// Expose current AP mode state to main loop.
bool isAPModeActive();

#endif // WIFI_AP_H
