/*
 * Project: HomeKitKnock-S3
 * File: include/sip_client.h
 * Author: Jesse Greene
 */

#ifndef SIP_CLIENT_H
#define SIP_CLIENT_H

#include <Arduino.h>
#include <WiFiUdp.h>

// SIP configuration stored in NVS
struct SipConfig {
  String sip_user;         // SIP username (IP phone username from FRITZ!Box)
  String sip_password;     // SIP password
  String sip_displayname;  // Display name for caller ID
  String sip_target;       // Target number to ring (e.g., **610)
};

// Load SIP configuration from NVS
bool loadSipConfig(SipConfig &config);

// Validate that all required SIP fields are present
bool hasSipConfig(const SipConfig &config);

// Initialize SIP client (bind UDP port)
bool initSipClient();

// Send SIP REGISTER to FRITZ!Box (call periodically, e.g., every 60 seconds)
void sendSipRegister(const SipConfig &config);

// Ring the configured target via SIP INVITE/CANCEL
// Returns true if SIP messages were sent successfully
bool triggerSipRing(const SipConfig &config);

// Optional hook invoked while the SIP ring loop is running (e.g., to keep LEDs animating).
typedef void (*SipRingTickCallback)();
void setSipRingTickCallback(SipRingTickCallback callback);

// True when a SIP ring transaction is active (INVITE in progress).
bool isSipRingActive();

// Advance the SIP ring state machine (non-blocking).
void processSipRing();

// Handle incoming SIP responses (call in loop())
void handleSipIncoming();

// Check if it's time to send another REGISTER
void sendRegisterIfNeeded(const SipConfig &config);

// True when a recent REGISTER succeeded (used for status LED).
bool isSipRegistrationOk();

// Timestamp of the last successful REGISTER (milliseconds since boot).
unsigned long getSipLastRegisterOkMs();

// Timestamp of the last REGISTER attempt (milliseconds since boot).
unsigned long getSipLastRegisterAttemptMs();

#endif // SIP_CLIENT_H
