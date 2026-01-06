/*
 * Project: HomeKitKnock-S3
 * File: src/sip_client.cpp
 * Author: Jesse Greene
 */

#include "sip_client.h"
#include <Preferences.h>
#include <WiFi.h>
#include <MD5Builder.h>

// FRITZ!Box SIP settings
static const char* SIP_DOMAIN = "fritz.box";
static const char* SIP_PROXY = "fritz.box";
static const uint16_t SIP_PORT = 5060;

// Local SIP client settings
static const uint16_t LOCAL_SIP_PORT = 5062;
static WiFiUDP sipUdp;
static unsigned long lastRegisterTime = 0;
static const unsigned long REGISTER_INTERVAL_MS = 60UL * 1000; // 60 seconds

// Authentication state
struct AuthChallenge {
  String realm;
  String nonce;
  String algorithm;
  String qop;
  String opaque;
  bool valid = false;
};

static AuthChallenge lastAuthChallenge;
static uint32_t nonceCount = 1;

// Helper functions for generating unique SIP identifiers
static String generateTag() {
  return String((uint32_t)esp_random(), HEX);
}

static String generateCallID() {
  IPAddress localIP = WiFi.localIP();
  return String((uint32_t)esp_random(), HEX) + "@" + localIP.toString();
}

// MD5 hash helper
static String md5(const String& input) {
  MD5Builder md5builder;
  md5builder.begin();
  md5builder.add(input);
  md5builder.calculate();
  return md5builder.toString();
}

// Parse authentication challenge from 401/407 response
static AuthChallenge parseAuthChallenge(const String& response) {
  AuthChallenge challenge;
  
  // Find WWW-Authenticate or Proxy-Authenticate header
  int authPos = response.indexOf("WWW-Authenticate:");
  if (authPos == -1) {
    authPos = response.indexOf("Proxy-Authenticate:");
  }
  if (authPos == -1) {
    return challenge;
  }
  
  // Extract the header line
  int endPos = response.indexOf("\r\n", authPos);
  if (endPos == -1) return challenge;
  
  String authLine = response.substring(authPos, endPos);
  
  // Parse realm
  int realmStart = authLine.indexOf("realm=\"");
  if (realmStart != -1) {
    realmStart += 7;
    int realmEnd = authLine.indexOf("\"", realmStart);
    challenge.realm = authLine.substring(realmStart, realmEnd);
  }
  
  // Parse nonce
  int nonceStart = authLine.indexOf("nonce=\"");
  if (nonceStart != -1) {
    nonceStart += 7;
    int nonceEnd = authLine.indexOf("\"", nonceStart);
    challenge.nonce = authLine.substring(nonceStart, nonceEnd);
  }
  
  // Parse algorithm (optional, default MD5)
  int algoStart = authLine.indexOf("algorithm=");
  if (algoStart != -1) {
    algoStart += 10;
    int algoEnd = authLine.indexOf(",", algoStart);
    if (algoEnd == -1) algoEnd = authLine.indexOf("\r", algoStart);
    challenge.algorithm = authLine.substring(algoStart, algoEnd);
    challenge.algorithm.trim();
  } else {
    challenge.algorithm = "MD5";
  }
  
  // Parse qop (optional)
  int qopStart = authLine.indexOf("qop=\"");
  if (qopStart != -1) {
    qopStart += 5;
    int qopEnd = authLine.indexOf("\"", qopStart);
    challenge.qop = authLine.substring(qopStart, qopEnd);
  }
  
  // Parse opaque (optional)
  int opaqueStart = authLine.indexOf("opaque=\"");
  if (opaqueStart != -1) {
    opaqueStart += 8;
    int opaqueEnd = authLine.indexOf("\"", opaqueStart);
    challenge.opaque = authLine.substring(opaqueStart, opaqueEnd);
  }
  
  challenge.valid = !challenge.realm.isEmpty() && !challenge.nonce.isEmpty();
  return challenge;
}

// Calculate digest response for authentication
static String calculateDigestResponse(
  const String& username,
  const String& password,
  const String& method,
  const String& uri,
  const AuthChallenge& challenge
) {
  // HA1 = MD5(username:realm:password)
  String ha1Input = username + ":" + challenge.realm + ":" + password;
  String ha1 = md5(ha1Input);
  
  // HA2 = MD5(method:uri)
  String ha2Input = method + ":" + uri;
  String ha2 = md5(ha2Input);
  
  // Response calculation
  String responseInput;
  
  if (challenge.qop.isEmpty()) {
    // Without qop: response = MD5(HA1:nonce:HA2)
    responseInput = ha1 + ":" + challenge.nonce + ":" + ha2;
  } else {
    // With qop=auth: response = MD5(HA1:nonce:nc:cnonce:qop:HA2)
    String nc = String(nonceCount, HEX);
    while (nc.length() < 8) nc = "0" + nc;
    String cnonce = String((uint32_t)esp_random(), HEX);
    
    responseInput = ha1 + ":" + challenge.nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2;
  }
  
  return md5(responseInput);
}

// Build Authorization header
static String buildAuthHeader(
  const String& username,
  const String& password,
  const String& method,
  const String& uri,
  const AuthChallenge& challenge
) {
  String response = calculateDigestResponse(username, password, method, uri, challenge);
  
  String authHeader = "Authorization: Digest ";
  authHeader += "username=\"" + username + "\", ";
  authHeader += "realm=\"" + challenge.realm + "\", ";
  authHeader += "nonce=\"" + challenge.nonce + "\", ";
  authHeader += "uri=\"" + uri + "\", ";
  authHeader += "response=\"" + response + "\"";
  
  if (!challenge.algorithm.isEmpty()) {
    authHeader += ", algorithm=" + challenge.algorithm;
  }
  
  if (!challenge.qop.isEmpty()) {
    String nc = String(nonceCount, HEX);
    while (nc.length() < 8) nc = "0" + nc;
    String cnonce = String((uint32_t)esp_random(), HEX);
    
    authHeader += ", qop=auth";
    authHeader += ", nc=" + nc;
    authHeader += ", cnonce=\"" + cnonce + "\"";
    nonceCount++;
  }
  
  if (!challenge.opaque.isEmpty()) {
    authHeader += ", opaque=\"" + challenge.opaque + "\"";
  }
  
  authHeader += "\r\n";
  return authHeader;
}

// Load SIP configuration from NVS
bool loadSipConfig(SipConfig &config) {
  Preferences prefs;
  if (!prefs.begin("sip", true)) {
    Serial.println("❌ Failed to open SIP preferences");
    return false;
  }
  
  config.sip_user = prefs.getString("sip_user", "");
  config.sip_password = prefs.getString("sip_password", "");
  config.sip_displayname = prefs.getString("sip_displayname", "Doorbell");
  config.sip_target = prefs.getString("sip_target", "**610");
  config.scrypted_webhook = prefs.getString("scrypted_webhook", "");
  
  prefs.end();
  return true;
}

// Validate that all required SIP fields are present
bool hasSipConfig(const SipConfig &config) {
  return !config.sip_user.isEmpty() && 
         !config.sip_password.isEmpty() && 
         !config.sip_target.isEmpty();
}

// Initialize SIP client (bind UDP port)
bool initSipClient() {
  if (!sipUdp.begin(LOCAL_SIP_PORT)) {
    Serial.println("❌ Failed to bind UDP port for SIP");
    return false;
  }
  Serial.print("✅ SIP UDP bound to port ");
  Serial.println(LOCAL_SIP_PORT);
  return true;
}

// Build SIP REGISTER message with optional authentication
static String buildRegister(const SipConfig &config, const String& fromTag, const String& callID, uint32_t cseq, bool withAuth = false) {
  IPAddress localIP = WiFi.localIP();
  String uri = "sip:" + String(SIP_DOMAIN);
  
  String msg;
  msg  = "REGISTER " + uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=z9hG4bK-" + fromTag + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += "To: <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">\r\n";
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " REGISTER\r\n";
  msg += "Contact: <sip:" + config.sip_user + "@" + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ">\r\n";
  
  // Add authentication if we have a challenge
  if (withAuth && lastAuthChallenge.valid) {
    msg += buildAuthHeader(config.sip_user, config.sip_password, "REGISTER", uri, lastAuthChallenge);
  }
  
  msg += "Expires: 120\r\n";
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

// Build SIP INVITE message with optional authentication
static String buildInvite(const SipConfig &config, const String& fromTag, const String& callID, uint32_t cseq, bool withAuth = false) {
  IPAddress localIP = WiFi.localIP();
  String target = config.sip_target + "@" + String(SIP_DOMAIN);
  String uri = "sip:" + target;

  String msg;
  msg  = "INVITE " + uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=z9hG4bK-" + fromTag + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += "To: <sip:" + target + ">\r\n";
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " INVITE\r\n";
  msg += "Contact: <sip:" + config.sip_user + "@" + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ">\r\n";
  
  // Add authentication if we have a challenge
  if (withAuth && lastAuthChallenge.valid) {
    msg += buildAuthHeader(config.sip_user, config.sip_password, "INVITE", uri, lastAuthChallenge);
  }
  
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Type: application/sdp\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

// Build SIP CANCEL message
static String buildCancel(const SipConfig &config, const String& fromTag, const String& callID, uint32_t cseq) {
  IPAddress localIP = WiFi.localIP();
  String target = config.sip_target + "@" + String(SIP_DOMAIN);

  String msg;
  msg  = "CANCEL sip:" + target + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=z9hG4bK-" + fromTag + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += "To: <sip:" + target + ">\r\n";
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " CANCEL\r\n";
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

// Send SIP message to FRITZ!Box
static void sipSend(const String& msg) {
  sipUdp.beginPacket(SIP_PROXY, SIP_PORT);
  sipUdp.write((const uint8_t*)msg.c_str(), msg.length());
  sipUdp.endPacket();
}

// Wait for and parse SIP response with timeout
static String waitForSipResponse(unsigned long timeoutMs = 2000) {
  unsigned long start = millis();
  
  while (millis() - start < timeoutMs) {
    int packetSize = sipUdp.parsePacket();
    if (packetSize > 0) {
      char buf[2048];
      int len = sipUdp.read(buf, sizeof(buf) - 1);
      if (len > 0) {
        buf[len] = '\0';
        return String(buf);
      }
    }
    delay(10);
  }
  
  return "";
}

// Check if response is 401 or 407 (authentication required)
static bool isAuthRequired(const String& response) {
  return response.startsWith("SIP/2.0 401") || response.startsWith("SIP/2.0 407");
}

// Check if response is success (2xx)
static bool isSuccess(const String& response) {
  return response.startsWith("SIP/2.0 2");
}

// Handle incoming SIP responses
void handleSipIncoming() {
  int packetSize = sipUdp.parsePacket();
  if (packetSize <= 0) return;

  char buf[2048];
  int len = sipUdp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';
  String resp(buf);

  Serial.println("---- SIP RX ----");
  Serial.println(resp);
}

// Send SIP REGISTER with authentication handling
void sendSipRegister(const SipConfig &config) {
  if (!hasSipConfig(config)) {
    Serial.println("⚠️ SIP config incomplete, skipping REGISTER");
    return;
  }

  String tag = generateTag();
  String callID = generateCallID();
  uint32_t cseq = 1;

  // First attempt without auth
  String regMsg = buildRegister(config, tag, callID, cseq, false);
  Serial.println("---- SIP REGISTER (attempt 1) ----");
  Serial.println(regMsg);
  sipSend(regMsg);
  
  // Wait for response
  String response = waitForSipResponse(2000);
  
  if (response.length() > 0) {
    Serial.println("---- SIP Response ----");
    Serial.println(response);
    
    if (isAuthRequired(response)) {
      Serial.println("🔐 Authentication required, sending with credentials...");
      
      // Parse the challenge
      lastAuthChallenge = parseAuthChallenge(response);
      
      if (lastAuthChallenge.valid) {
        // Increment CSeq and resend with auth
        cseq++;
        String authRegMsg = buildRegister(config, tag, callID, cseq, true);
        Serial.println("---- SIP REGISTER (attempt 2 - with auth) ----");
        Serial.println(authRegMsg);
        sipSend(authRegMsg);
        
        // Wait for final response
        String authResponse = waitForSipResponse(2000);
        if (authResponse.length() > 0) {
          Serial.println("---- SIP Auth Response ----");
          Serial.println(authResponse);
          
          if (isSuccess(authResponse)) {
            Serial.println("✅ SIP registration successful!");
          } else {
            Serial.println("❌ SIP registration failed");
          }
        }
      } else {
        Serial.println("❌ Failed to parse auth challenge");
      }
    } else if (isSuccess(response)) {
      Serial.println("✅ SIP registration successful (no auth required)!");
    } else {
      Serial.println("❌ SIP registration failed");
    }
  }
  
  lastRegisterTime = millis();
}

// Check if it's time to send another REGISTER
void sendRegisterIfNeeded(const SipConfig &config) {
  unsigned long now = millis();
  if (now - lastRegisterTime < REGISTER_INTERVAL_MS) return;
  sendSipRegister(config);
}

// Ring the configured target via SIP INVITE/CANCEL with authentication
bool triggerSipRing(const SipConfig &config) {
  if (!hasSipConfig(config)) {
    Serial.println("⚠️ SIP config incomplete, cannot ring");
    return false;
  }

  String tag = generateTag();
  String callID = generateCallID();
  uint32_t cseq = 1;

  // First INVITE attempt (may need auth)
  String invite = buildInvite(config, tag, callID, cseq, lastAuthChallenge.valid);
  Serial.println("---- SIP INVITE ----");
  Serial.println(invite);
  sipSend(invite);

  // Wait briefly for auth challenge or success
  String response = waitForSipResponse(1000);
  
  if (response.length() > 0 && isAuthRequired(response)) {
    Serial.println("🔐 INVITE needs authentication, resending...");
    
    // Parse challenge and resend
    AuthChallenge inviteChallenge = parseAuthChallenge(response);
    if (inviteChallenge.valid) {
      cseq++;
      String authInvite = buildInvite(config, tag, callID, cseq, true);
      lastAuthChallenge = inviteChallenge;
      
      Serial.println("---- SIP INVITE (with auth) ----");
      Serial.println(authInvite);
      sipSend(authInvite);
    }
  }

  // Ring for 2.5 seconds while processing responses
  const unsigned long ringDurationMs = 2500;
  unsigned long start = millis();
  while (millis() - start < ringDurationMs) {
    handleSipIncoming();
    delay(10);
  }

  // Send CANCEL to stop ringing
  cseq++;
  String cancel = buildCancel(config, tag, callID, cseq);
  Serial.println("---- SIP CANCEL ----");
  Serial.println(cancel);
  sipSend(cancel);

  return true;
}
