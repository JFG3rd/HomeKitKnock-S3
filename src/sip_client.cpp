/*
 * Project: HomeKitKnock-S3
 * File: src/sip_client.cpp
 * Author: Jesse Greene
 */

#include "sip_client.h"
#include <Preferences.h>
#include <WiFi.h>
#include <MD5Builder.h>
#include "logger.h"

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
  bool isProxy = false;
  bool valid = false;
};

static AuthChallenge lastAuthChallenge;
static uint32_t nonceCount = 1;

struct PendingInvite {
  bool active = false;
  bool authSent = false;
  bool canCancel = false;
  String callID;
  String fromTag;
  uint32_t cseq = 0;
  String branch;
  SipConfig config;
};

static PendingInvite pendingInvite;

// Helper functions for generating unique SIP identifiers
static String generateTag() {
  return String((uint32_t)esp_random(), HEX);
}

static String generateBranch() {
  return "z9hG4bK-" + String((uint32_t)esp_random(), HEX);
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
    if (authPos != -1) {
      challenge.isProxy = true;
    }
  } else {
    challenge.isProxy = false;
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
  
  // Parse qop (optional, may be quoted or unquoted)
  int qopStart = authLine.indexOf("qop=\"");
  if (qopStart != -1) {
    qopStart += 5;
    int qopEnd = authLine.indexOf("\"", qopStart);
    challenge.qop = authLine.substring(qopStart, qopEnd);
  } else {
    qopStart = authLine.indexOf("qop=");
    if (qopStart != -1) {
      qopStart += 4;
      int qopEnd = authLine.indexOf(",", qopStart);
      if (qopEnd == -1) qopEnd = authLine.indexOf("\r", qopStart);
      challenge.qop = authLine.substring(qopStart, qopEnd);
      challenge.qop.trim();
    }
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
  const AuthChallenge& challenge,
  String* outNc,
  String* outCnonce
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
    if (outNc) *outNc = nc;
    if (outCnonce) *outCnonce = cnonce;
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
  String nc;
  String cnonce;
  String response = calculateDigestResponse(username, password, method, uri, challenge, &nc, &cnonce);
  
  String headerName = challenge.isProxy ? "Proxy-Authorization" : "Authorization";
  String authHeader = headerName + ": Digest ";
  authHeader += "username=\"" + username + "\", ";
  authHeader += "realm=\"" + challenge.realm + "\", ";
  authHeader += "nonce=\"" + challenge.nonce + "\", ";
  authHeader += "uri=\"" + uri + "\", ";
  authHeader += "response=\"" + response + "\"";
  
  if (!challenge.algorithm.isEmpty()) {
    authHeader += ", algorithm=" + challenge.algorithm;
  }
  
  if (!challenge.qop.isEmpty()) {
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
    logEvent(LOG_ERROR, "❌ Failed to open SIP preferences");
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
    logEvent(LOG_ERROR, "❌ Failed to bind UDP port for SIP");
    return false;
  }
  logEvent(LOG_INFO, "✅ SIP UDP bound to port " + String(LOCAL_SIP_PORT));
  return true;
}

// Build SIP REGISTER message with optional authentication
static String buildRegister(const SipConfig &config, const String& fromTag, const String& callID, const String& branch, uint32_t cseq, bool withAuth = false) {
  IPAddress localIP = WiFi.localIP();
  String uri = "sip:" + String(SIP_DOMAIN);
  
  String msg;
  msg  = "REGISTER " + uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + "\r\n";
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
static String buildInvite(const SipConfig &config, const String& fromTag, const String& callID, const String& branch, uint32_t cseq, bool withAuth = false) {
  IPAddress localIP = WiFi.localIP();
  String target = config.sip_target + "@" + String(SIP_DOMAIN);
  String uri = "sip:" + target;

  String msg;
  msg  = "INVITE " + uri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + "\r\n";
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
static String buildCancel(const SipConfig &config, const String& fromTag, const String& callID, const String& branch, uint32_t cseq) {
  IPAddress localIP = WiFi.localIP();
  String target = config.sip_target + "@" + String(SIP_DOMAIN);

  String msg;
  msg  = "CANCEL sip:" + target + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + "\r\n";
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

// Check if SIP response matches a Call-ID and CSeq method
static bool responseMatches(const String& response, const String& callID, uint32_t cseq, const char* method) {
  int callIdPos = response.indexOf("Call-ID:");
  if (callIdPos == -1) return false;
  int callIdEnd = response.indexOf("\r\n", callIdPos);
  if (callIdEnd == -1) return false;
  String respCallId = response.substring(callIdPos + 8, callIdEnd);
  respCallId.trim();
  if (!respCallId.equalsIgnoreCase(callID)) return false;

  int cseqPos = response.indexOf("CSeq:");
  if (cseqPos == -1) return false;
  int cseqEnd = response.indexOf("\r\n", cseqPos);
  if (cseqEnd == -1) return false;
  String respCseqLine = response.substring(cseqPos + 5, cseqEnd);
  respCseqLine.trim();

  int spacePos = respCseqLine.indexOf(' ');
  if (spacePos == -1) return false;
  uint32_t respCseq = respCseqLine.substring(0, spacePos).toInt();
  String respMethod = respCseqLine.substring(spacePos + 1);
  respMethod.trim();

  return respCseq == cseq && respMethod.equalsIgnoreCase(method);
}

// Wait for a matching SIP response for a specific Call-ID/CSeq/method.
static String waitForMatchingSipResponse(const String& callID, uint32_t cseq, const char* method, unsigned long timeoutMs = 2000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    int packetSize = sipUdp.parsePacket();
    if (packetSize > 0) {
      char buf[2048];
      int len = sipUdp.read(buf, sizeof(buf) - 1);
      if (len > 0) {
        buf[len] = '\0';
        String resp(buf);
        if (responseMatches(resp, callID, cseq, method)) {
          return resp;
        }
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

static bool isProvisional(const String& response) {
  return response.startsWith("SIP/2.0 1");
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

  logEvent(LOG_DEBUG, "---- SIP RX ----");
  logEvent(LOG_DEBUG, resp);

  if (!pendingInvite.active) {
    return;
  }

  if (!responseMatches(resp, pendingInvite.callID, pendingInvite.cseq, "INVITE")) {
    return;
  }

  if (isAuthRequired(resp) && !pendingInvite.authSent) {
    logEvent(LOG_WARN, "🔐 INVITE needs authentication, resending...");
    AuthChallenge inviteChallenge = parseAuthChallenge(resp);
    if (inviteChallenge.valid) {
      lastAuthChallenge = inviteChallenge;
      pendingInvite.cseq++;
      pendingInvite.branch = generateBranch();
      String authInvite = buildInvite(pendingInvite.config, pendingInvite.fromTag, pendingInvite.callID, pendingInvite.branch, pendingInvite.cseq, true);
      pendingInvite.authSent = true;
      logEvent(LOG_DEBUG, "---- SIP INVITE (with auth) ----");
      logEvent(LOG_DEBUG, authInvite);
      sipSend(authInvite);
    } else {
      logEvent(LOG_ERROR, "❌ Failed to parse INVITE auth challenge");
    }
    return;
  }

  if (isProvisional(resp) || isSuccess(resp)) {
    pendingInvite.canCancel = true;
  }
}

// Send SIP REGISTER with authentication handling
void sendSipRegister(const SipConfig &config) {
  if (!hasSipConfig(config)) {
    logEvent(LOG_WARN, "⚠️ SIP config incomplete, skipping REGISTER");
    return;
  }

  String tag = generateTag();
  String callID = generateCallID();
  uint32_t cseq = 1;
  String branch = generateBranch();

  // First attempt without auth
  String regMsg = buildRegister(config, tag, callID, branch, cseq, false);
  logEvent(LOG_DEBUG, "---- SIP REGISTER (attempt 1) ----");
  logEvent(LOG_DEBUG, regMsg);
  sipSend(regMsg);
  
  // Wait for response
  String response = waitForSipResponse(2000);
  
  if (response.length() > 0) {
    logEvent(LOG_DEBUG, "---- SIP Response ----");
    logEvent(LOG_DEBUG, response);
    
    if (isAuthRequired(response)) {
      logEvent(LOG_WARN, "🔐 Authentication required, sending with credentials...");
      
      // Parse the challenge
      lastAuthChallenge = parseAuthChallenge(response);
      
      if (lastAuthChallenge.valid) {
        // Increment CSeq and resend with auth
        cseq++;
        branch = generateBranch();
        String authRegMsg = buildRegister(config, tag, callID, branch, cseq, true);
        logEvent(LOG_DEBUG, "---- SIP REGISTER (attempt 2 - with auth) ----");
        logEvent(LOG_DEBUG, authRegMsg);
        sipSend(authRegMsg);
        
        // Wait for final response
        String authResponse = waitForSipResponse(2000);
        if (authResponse.length() > 0) {
          logEvent(LOG_DEBUG, "---- SIP Auth Response ----");
          logEvent(LOG_DEBUG, authResponse);
          
          if (isSuccess(authResponse)) {
            logEvent(LOG_INFO, "✅ SIP registration successful!");
          } else {
            logEvent(LOG_ERROR, "❌ SIP registration failed");
          }
        }
      } else {
        logEvent(LOG_ERROR, "❌ Failed to parse auth challenge");
      }
    } else if (isSuccess(response)) {
      logEvent(LOG_INFO, "✅ SIP registration successful (no auth required)!");
    } else {
      logEvent(LOG_ERROR, "❌ SIP registration failed");
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
    logEvent(LOG_WARN, "⚠️ SIP config incomplete, cannot ring");
    return false;
  }

  String tag = generateTag();
  String callID = generateCallID();
  uint32_t cseq = 1;
  String branch = generateBranch();

  // Drain any stale packets before starting the INVITE transaction.
  while (sipUdp.parsePacket() > 0) {
    char drainBuf[32];
    sipUdp.read(drainBuf, sizeof(drainBuf));
  }

  // First INVITE attempt (may need auth)
  String invite = buildInvite(config, tag, callID, branch, cseq, false);
  logEvent(LOG_DEBUG, "---- SIP INVITE ----");
  logEvent(LOG_DEBUG, invite);
  sipSend(invite);

  pendingInvite.active = true;
  pendingInvite.authSent = false;
  pendingInvite.canCancel = false;
  pendingInvite.callID = callID;
  pendingInvite.fromTag = tag;
  pendingInvite.cseq = cseq;
  pendingInvite.branch = branch;
  pendingInvite.config = config;

  // Ring for 2.5 seconds while processing responses
  const unsigned long ringDurationMs = 2500;
  unsigned long start = millis();
  while (millis() - start < ringDurationMs) {
    handleSipIncoming();
    delay(10);
  }

  // Send CANCEL to stop ringing (must match last INVITE branch + CSeq).
  if (pendingInvite.canCancel) {
    String cancel = buildCancel(config, pendingInvite.fromTag, pendingInvite.callID, pendingInvite.branch, pendingInvite.cseq);
    logEvent(LOG_DEBUG, "---- SIP CANCEL ----");
    logEvent(LOG_DEBUG, cancel);
    sipSend(cancel);
  } else {
    logEvent(LOG_WARN, "⚠️ Skipping CANCEL (no provisional response received)");
  }
  pendingInvite.active = false;

  return true;
}
