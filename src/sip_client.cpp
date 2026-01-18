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
static const uint16_t SIP_RTP_PORT = 40000;
static WiFiUDP sipUdp;
static bool sipUdpReady = false;
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
  bool answered = false;
  bool ackSent = false;
  bool byeSent = false;
  bool cancelSent = false;
  String callID;
  String fromTag;
  String toTag;
  uint32_t cseq = 0;
  String branch;
  String target;
  String remoteTarget;
  unsigned long inviteStartMs = 0;
  unsigned long answeredMs = 0;
  SipConfig config;
};

static PendingInvite pendingInvite;
static unsigned long lastSipNetWarnMs = 0;
static const unsigned long kSipRingDurationMs = 20000;  // Let phones ring before cancel.
static const unsigned long kSipInCallHoldMs = 20000;    // Hold after answer so phones have time to ring.
static const unsigned long kSipCancelWaitMs = 3000;     // Wait for 487 after CANCEL.

// Guard SIP traffic when Wi-Fi is down or the socket is not ready.
// This prevents DNS/UDP failures from crashing the SIP task.
static bool isSipNetworkReady() {
  if (!sipUdpReady) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastSipNetWarnMs > 10000) {
      logEvent(LOG_WARN, "⚠️ SIP paused: WiFi not connected");
      lastSipNetWarnMs = millis();
    }
    return false;
  }
  IPAddress localIP = WiFi.localIP();
  if (localIP == IPAddress(0, 0, 0, 0)) {
    if (millis() - lastSipNetWarnMs > 10000) {
      logEvent(LOG_WARN, "⚠️ SIP paused: invalid local IP");
      lastSipNetWarnMs = millis();
    }
    return false;
  }
  return true;
}

// Resolve the FRITZ!Box SIP proxy with a gateway IP fallback.
static bool resolveSipProxy(IPAddress &dest) {
  if (WiFi.hostByName(SIP_PROXY, dest)) {
    return true;
  }
  dest = WiFi.gatewayIP();
  return dest != IPAddress(0, 0, 0, 0);
}

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

// Extract the status code from a SIP response line.
static int getSipStatusCode(const String& response) {
  int lineEnd = response.indexOf('\n');
  String statusLine = lineEnd >= 0 ? response.substring(0, lineEnd) : response;
  statusLine.trim();
  if (!statusLine.startsWith("SIP/2.0")) {
    return -1;
  }
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) {
    return -1;
  }
  int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  String codeStr = secondSpace > 0
    ? statusLine.substring(firstSpace + 1, secondSpace)
    : statusLine.substring(firstSpace + 1);
  return codeStr.toInt();
}

static String extractHeaderValue(const String& response, const char *headerName) {
  String needle = String(headerName) + ":";
  int pos = response.indexOf(needle);
  if (pos < 0) {
    return "";
  }
  int lineEnd = response.indexOf("\r\n", pos);
  if (lineEnd < 0) {
    lineEnd = response.indexOf('\n', pos);
  }
  if (lineEnd < 0) {
    lineEnd = response.length();
  }
  String line = response.substring(pos + needle.length(), lineEnd);
  line.trim();
  return line;
}

static String extractViaBranch(const String& response) {
  String viaLine = extractHeaderValue(response, "Via");
  if (viaLine.isEmpty()) {
    viaLine = extractHeaderValue(response, "v");
  }
  if (viaLine.isEmpty()) {
    return "";
  }
  int branchPos = viaLine.indexOf("branch=");
  if (branchPos < 0) {
    return "";
  }
  branchPos += 7;
  int branchEnd = viaLine.indexOf(';', branchPos);
  if (branchEnd < 0) {
    branchEnd = viaLine.length();
  }
  String branch = viaLine.substring(branchPos, branchEnd);
  branch.trim();
  return branch;
}

static bool parseCSeq(const String& response, uint32_t *outCseq, String *outMethod) {
  String cseqLine = extractHeaderValue(response, "CSeq");
  if (cseqLine.isEmpty()) {
    cseqLine = extractHeaderValue(response, "C");
  }
  if (cseqLine.isEmpty()) {
    return false;
  }
  int spacePos = cseqLine.indexOf(' ');
  if (spacePos < 0) {
    return false;
  }
  *outCseq = cseqLine.substring(0, spacePos).toInt();
  *outMethod = cseqLine.substring(spacePos + 1);
  outMethod->trim();
  return true;
}

static String normalizeSipUri(const String& uri) {
  String normalized = uri;
  normalized.trim();
  if (normalized.startsWith("<") && normalized.endsWith(">")) {
    normalized = normalized.substring(1, normalized.length() - 1);
  }
  if (normalized.startsWith("sip:")) {
    return normalized;
  }
  return "sip:" + normalized;
}

// Pull the tag parameter from a To header (needed for ACK/BYE).
static String extractToTag(const String& response) {
  String toLine = extractHeaderValue(response, "To");
  if (toLine.isEmpty()) {
    toLine = extractHeaderValue(response, "t");
  }
  int tagPos = toLine.indexOf("tag=");
  if (tagPos < 0) {
    return "";
  }
  tagPos += 4;
  int tagEnd = toLine.indexOf(';', tagPos);
  if (tagEnd < 0) {
    tagEnd = toLine.length();
  }
  return toLine.substring(tagPos, tagEnd);
}

static String extractContactUri(const String& response) {
  String contactLine = extractHeaderValue(response, "Contact");
  if (contactLine.isEmpty()) {
    contactLine = extractHeaderValue(response, "m");
  }
  if (contactLine.isEmpty()) {
    return "";
  }
  int sipPos = contactLine.indexOf("sip:");
  if (sipPos < 0) {
    return "";
  }
  int endPos = contactLine.indexOf('>', sipPos);
  if (endPos < 0) {
    endPos = contactLine.indexOf(';', sipPos);
  }
  if (endPos < 0) {
    endPos = contactLine.length();
  }
  String uri = contactLine.substring(sipPos, endPos);
  uri.trim();
  return uri;
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

static String buildToHeader(const String& target, const String& toTag) {
  String header = "To: <sip:" + target + ">";
  if (!toTag.isEmpty()) {
    header += ";tag=" + toTag;
  }
  header += "\r\n";
  return header;
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
  config.sip_target = prefs.getString("sip_target", "**11");
  
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
  sipUdpReady = true;
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
  String requestUri = "sip:" + target;
  String authUri = "sip:" + String(SIP_DOMAIN);
  String sdp;
  // Offer minimal audio so FRITZ!Box treats INVITE as a proper call setup.
  // We declare recvonly G.711 even though RTP streaming is not implemented yet.
  if (localIP != IPAddress(0, 0, 0, 0)) {
    sdp  = "v=0\r\n";
    sdp += "o=- 0 0 IN IP4 " + localIP.toString() + "\r\n";
    sdp += "s=ESP32 Doorbell\r\n";
    sdp += "c=IN IP4 " + localIP.toString() + "\r\n";
    sdp += "t=0 0\r\n";
    sdp += "m=audio " + String(SIP_RTP_PORT) + " RTP/AVP 0 8 101\r\n";
    sdp += "a=rtpmap:0 PCMU/8000\r\n";
    sdp += "a=rtpmap:8 PCMA/8000\r\n";
    sdp += "a=rtpmap:101 telephone-event/8000\r\n";
    sdp += "a=fmtp:101 0-15\r\n";
    sdp += "a=recvonly\r\n";
  }

  String msg;
  msg  = "INVITE " + requestUri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += buildToHeader(target, "");
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " INVITE\r\n";
  msg += "Contact: <sip:" + config.sip_user + "@" + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ">\r\n";
  
  // Add authentication if we have a challenge
  if (withAuth && lastAuthChallenge.valid) {
    // FRITZ!Box expects the digest URI to be the registrar, not the target.
    msg += buildAuthHeader(config.sip_user, config.sip_password, "INVITE", authUri, lastAuthChallenge);
  }
  
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  if (sdp.length() > 0) {
    msg += "Content-Type: application/sdp\r\n";
  }
  msg += "Content-Length: " + String(sdp.length()) + "\r\n";
  msg += "\r\n";
  if (sdp.length() > 0) {
    msg += sdp;
  }
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
  msg += buildToHeader(target, pendingInvite.toTag);
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " CANCEL\r\n";
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

static String buildAck(const SipConfig &config,
                       const String& fromTag,
                       const String& toTag,
                       const String& callID,
                       const String& requestUri,
                       const String& toHeaderTarget,
                       uint32_t cseq) {
  IPAddress localIP = WiFi.localIP();
  String normalizedUri = normalizeSipUri(requestUri);
  String msg;
  msg  = "ACK " + normalizedUri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + generateBranch() + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += buildToHeader(toHeaderTarget, toTag);
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " ACK\r\n";
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

static String buildInviteNon2xxAck(const SipConfig &config,
                                   const String& fromTag,
                                   const String& toTag,
                                   const String& callID,
                                   const String& branch,
                                   uint32_t cseq,
                                   const String& requestUri,
                                   const String& toHeaderTarget) {
  if (branch.isEmpty()) {
    return "";
  }
  IPAddress localIP = WiFi.localIP();
  String normalizedUri = normalizeSipUri(requestUri);
  String msg;
  msg  = "ACK " + normalizedUri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += buildToHeader(toHeaderTarget, toTag);
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " ACK\r\n";
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

static String buildBye(const SipConfig &config,
                       const String& fromTag,
                       const String& toTag,
                       const String& callID,
                       const String& requestUri,
                       const String& toHeaderTarget,
                       uint32_t cseq) {
  IPAddress localIP = WiFi.localIP();
  String normalizedUri = normalizeSipUri(requestUri);
  String msg;
  msg  = "BYE " + normalizedUri + " SIP/2.0\r\n";
  msg += "Via: SIP/2.0/UDP " + localIP.toString() + ":" + String(LOCAL_SIP_PORT) + ";branch=" + generateBranch() + "\r\n";
  msg += "Max-Forwards: 70\r\n";
  msg += "From: \"" + config.sip_displayname + "\" <sip:" + config.sip_user + "@" + String(SIP_DOMAIN) + ">;tag=" + fromTag + "\r\n";
  msg += buildToHeader(toHeaderTarget, toTag);
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + String(cseq) + " BYE\r\n";
  msg += "User-Agent: ESP32-Doorbell/1.0\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
}

static bool sipSendTo(const IPAddress& dest, uint16_t port, const String& msg) {
  if (!sipUdp.beginPacket(dest, port)) {
    logEvent(LOG_WARN, "⚠️ SIP send failed: UDP beginPacket() failed");
    return false;
  }

  size_t written = sipUdp.write(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());
  if (written != msg.length()) {
    logEvent(LOG_WARN, "⚠️ SIP send incomplete: " + String(written) + "/" + String(msg.length()));
  }

  if (!sipUdp.endPacket()) {
    logEvent(LOG_WARN, "⚠️ SIP send failed: UDP endPacket() failed");
    return false;
  }

  return true;
}

static bool sipSendResponse(const String& msg) {
  IPAddress dest = sipUdp.remoteIP();
  uint16_t port = sipUdp.remotePort();
  if (dest == IPAddress(0, 0, 0, 0) || port == 0) {
    logEvent(LOG_WARN, "⚠️ SIP send failed: invalid remote endpoint");
    return false;
  }
  return sipSendTo(dest, port, msg);
}

// Send SIP message to FRITZ!Box
static bool sipSend(const String& msg) {
  if (!isSipNetworkReady()) {
    return false;
  }

  IPAddress dest;
  if (!resolveSipProxy(dest)) {
    logEvent(LOG_WARN, "⚠️ SIP send failed: cannot resolve proxy or gateway");
    return false;
  }

  return sipSendTo(dest, SIP_PORT, msg);
}

static String buildOkResponse(const String& request) {
  String via = extractHeaderValue(request, "Via");
  String from = extractHeaderValue(request, "From");
  String to = extractHeaderValue(request, "To");
  String callID = extractHeaderValue(request, "Call-ID");
  String cseq = extractHeaderValue(request, "CSeq");
  if (via.isEmpty() || from.isEmpty() || to.isEmpty() || callID.isEmpty() || cseq.isEmpty()) {
    return "";
  }
  String msg = "SIP/2.0 200 OK\r\n";
  msg += "Via: " + via + "\r\n";
  msg += "From: " + from + "\r\n";
  msg += "To: " + to + "\r\n";
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + cseq + "\r\n";
  msg += "Content-Length: 0\r\n";
  msg += "\r\n";
  return msg;
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
  if (!isSipNetworkReady()) {
    return;
  }

  int packetSize = sipUdp.parsePacket();
  if (packetSize <= 0) return;

  char buf[2048];
  int len = sipUdp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';
  String resp(buf);

  logEvent(LOG_DEBUG, "---- SIP RX ----");
  logEvent(LOG_DEBUG, resp);

  int lineEnd = resp.indexOf('\n');
  String firstLine = lineEnd >= 0 ? resp.substring(0, lineEnd) : resp;
  firstLine.trim();
  if (!firstLine.startsWith("SIP/2.0")) {
    int spacePos = firstLine.indexOf(' ');
    String method = spacePos > 0 ? firstLine.substring(0, spacePos) : firstLine;
    method.trim();
    if (method.equalsIgnoreCase("BYE") || method.equalsIgnoreCase("CANCEL")) {
      String ok = buildOkResponse(resp);
      if (!ok.isEmpty()) {
        logEvent(LOG_DEBUG, "---- SIP TX ----");
        logEvent(LOG_DEBUG, ok);
        sipSendResponse(ok);
      }
      if (method.equalsIgnoreCase("BYE")) {
        pendingInvite.active = false;
        pendingInvite.answered = false;
      }
    }
    return;
  }

  if (!pendingInvite.active) {
    return;
  }

  String respCallId = extractHeaderValue(resp, "Call-ID");
  respCallId.trim();
  if (respCallId.isEmpty() || !respCallId.equalsIgnoreCase(pendingInvite.callID)) {
    return;
  }

  uint32_t respCseq = 0;
  String respMethod;
  if (!parseCSeq(resp, &respCseq, &respMethod)) {
    return;
  }
  if (!respMethod.equalsIgnoreCase("INVITE")) {
    return;
  }

  int statusCode = getSipStatusCode(resp);
  bool isCurrent = respCseq == pendingInvite.cseq;
  String responseToTag = extractToTag(resp);

  if (isCurrent && statusCode >= 100 && statusCode < 300) {
    String contactUri = extractContactUri(resp);
    if (!contactUri.isEmpty()) {
      pendingInvite.remoteTarget = contactUri;
    }
    if (!responseToTag.isEmpty()) {
      pendingInvite.toTag = responseToTag;
    }
  }

  if (statusCode == 401 || statusCode == 407) {
    String branch = extractViaBranch(resp);
    String ack = buildInviteNon2xxAck(pendingInvite.config,
                                      pendingInvite.fromTag,
                                      responseToTag,
                                      pendingInvite.callID,
                                      branch,
                                      respCseq,
                                      pendingInvite.target,
                                      pendingInvite.target);
    if (!ack.isEmpty()) {
      logEvent(LOG_DEBUG, "---- SIP ACK (auth) ----");
      logEvent(LOG_DEBUG, ack);
      sipSend(ack);
    }
    if (!isCurrent || pendingInvite.authSent) {
      return;
    }
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
      if (!sipSend(authInvite)) {
        pendingInvite.active = false;
      }
    } else {
      logEvent(LOG_ERROR, "❌ Failed to parse INVITE auth challenge");
    }
    return;
  }

  if (statusCode >= 100 && statusCode < 200) {
    if (!isCurrent) {
      return;
    }
    pendingInvite.canCancel = true;
    return;
  }

  if (statusCode >= 200 && statusCode < 300) {
    if (!isCurrent) {
      return;
    }
    pendingInvite.canCancel = false;
    pendingInvite.answered = true;
    return;
  }

  if (statusCode >= 300) {
    String branch = extractViaBranch(resp);
    String ack = buildInviteNon2xxAck(pendingInvite.config,
                                      pendingInvite.fromTag,
                                      responseToTag,
                                      pendingInvite.callID,
                                      branch,
                                      respCseq,
                                      pendingInvite.target,
                                      pendingInvite.target);
    if (!ack.isEmpty()) {
      logEvent(LOG_DEBUG, "---- SIP ACK (final) ----");
      logEvent(LOG_DEBUG, ack);
      sipSend(ack);
    }
    if (isCurrent) {
      logEvent(LOG_WARN, "⚠️ SIP INVITE failed with status " + String(statusCode));
      pendingInvite.active = false;
    }
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
  if (!sipSend(regMsg)) {
    return;
  }
  
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
        if (!sipSend(authRegMsg)) {
          return;
        }
        
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
  if (!isSipNetworkReady()) {
    return;
  }
  sendSipRegister(config);
}

// Ring the configured target via SIP INVITE/CANCEL with authentication
bool triggerSipRing(const SipConfig &config) {
  if (!hasSipConfig(config)) {
    logEvent(LOG_WARN, "⚠️ SIP config incomplete, cannot ring");
    return false;
  }
  if (!isSipNetworkReady()) {
    logEvent(LOG_WARN, "⚠️ SIP ring skipped: network not ready");
    return false;
  }
  if (config.sip_target.isEmpty()) {
    logEvent(LOG_WARN, "⚠️ SIP target is empty, cannot ring");
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

  // First INVITE attempt (may require 401 + digest).
  String invite = buildInvite(config, tag, callID, branch, cseq, false);
  logEvent(LOG_DEBUG, "---- SIP INVITE ----");
  logEvent(LOG_DEBUG, invite);
  if (!sipSend(invite)) {
    pendingInvite.active = false;
    return false;
  }

  pendingInvite.active = true;
  pendingInvite.authSent = false;
  pendingInvite.canCancel = false;
  pendingInvite.answered = false;
  pendingInvite.ackSent = false;
  pendingInvite.byeSent = false;
  pendingInvite.cancelSent = false;
  pendingInvite.callID = callID;
  pendingInvite.fromTag = tag;
  pendingInvite.toTag = "";
  pendingInvite.cseq = cseq;
  pendingInvite.branch = branch;
  pendingInvite.target = config.sip_target + "@" + String(SIP_DOMAIN);
  pendingInvite.remoteTarget = "";
  pendingInvite.inviteStartMs = millis();
  pendingInvite.answeredMs = 0;
  pendingInvite.config = config;

  // Ring window: wait for 100/180/183 or 200 OK while still allowing auth retry.
  while (millis() - pendingInvite.inviteStartMs < kSipRingDurationMs) {
    handleSipIncoming();
    if (!pendingInvite.active) {
      break;
    }
    if (pendingInvite.answered && !pendingInvite.ackSent) {
      String requestUri = pendingInvite.remoteTarget.isEmpty()
        ? pendingInvite.target
        : pendingInvite.remoteTarget;
      String ack = buildAck(config,
                            pendingInvite.fromTag,
                            pendingInvite.toTag,
                            pendingInvite.callID,
                            requestUri,
                            pendingInvite.target,
                            pendingInvite.cseq);
      logEvent(LOG_DEBUG, "---- SIP ACK ----");
      logEvent(LOG_DEBUG, ack);
      pendingInvite.ackSent = sipSend(ack);
      pendingInvite.answeredMs = millis();
    }
    if (pendingInvite.ackSent && !pendingInvite.byeSent &&
        (millis() - pendingInvite.answeredMs > kSipInCallHoldMs)) {
      String requestUri = pendingInvite.remoteTarget.isEmpty()
        ? pendingInvite.target
        : pendingInvite.remoteTarget;
      String bye = buildBye(config,
                            pendingInvite.fromTag,
                            pendingInvite.toTag,
                            pendingInvite.callID,
                            requestUri,
                            pendingInvite.target,
                            pendingInvite.cseq + 1);
      logEvent(LOG_DEBUG, "---- SIP BYE ----");
      logEvent(LOG_DEBUG, bye);
      pendingInvite.byeSent = sipSend(bye);
      break;
    }
    delay(10);
  }

  // Send CANCEL to stop ringing (must match last INVITE branch + CSeq).
  if (pendingInvite.active && !pendingInvite.answered && pendingInvite.canCancel) {
    String cancel = buildCancel(config, pendingInvite.fromTag, pendingInvite.callID, pendingInvite.branch, pendingInvite.cseq);
    logEvent(LOG_DEBUG, "---- SIP CANCEL ----");
    logEvent(LOG_DEBUG, cancel);
    sipSend(cancel);
    pendingInvite.cancelSent = true;
    unsigned long cancelStart = millis();
    while (millis() - cancelStart < kSipCancelWaitMs) {
      handleSipIncoming();
      if (!pendingInvite.active) {
        break;
      }
      delay(10);
    }
  } else {
    logEvent(LOG_INFO, "ℹ️ Skipping CANCEL (no provisional response or call answered)");
  }
  pendingInvite.active = false;

  return true;
}
