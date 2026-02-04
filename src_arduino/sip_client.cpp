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
#include "audio.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// FRITZ!Box SIP settings
static const char* SIP_DOMAIN = "fritz.box";
static const char* SIP_PROXY = "fritz.box";
static const uint16_t SIP_PORT = 5060;

// Local SIP client settings
static const uint16_t LOCAL_SIP_PORT = 5062;
static const uint16_t SIP_RTP_PORT = 40000;
static WiFiUDP sipUdp;
static bool sipUdpReady = false;
static WiFiUDP sipRtpUdp;
static bool sipRtpReady = false;
static unsigned long lastRegisterTime = 0;
static unsigned long lastRegisterAttemptMs = 0;
static unsigned long lastRegisterOkMs = 0;
static bool lastRegisterSuccessful = false;
static const unsigned long REGISTER_INTERVAL_MS = 60UL * 1000; // 60 seconds
static const uint32_t kSipAudioSampleRate = 8000;
static const size_t kSipRtpSamplesPerPacket = 160;  // 20ms at 8kHz
static const size_t kSipMicSamplesPerPacket =
  (AUDIO_SAMPLE_RATE / kSipAudioSampleRate) * kSipRtpSamplesPerPacket;
static const unsigned long kSipRtpFrameMs = 20;
static const uint32_t kSipMicReadTimeoutMs = 25;
static const unsigned long kSipMaxCallMs = 60000;
static const uint8_t kSipDefaultDtmfPayload = 101;

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

struct SipMediaInfo {
  IPAddress remoteIp;
  uint16_t remotePort = 0;
  bool hasPcmu = false;
  bool hasPcma = false;
  uint8_t preferredAudioPayload = 0xFF;
  uint8_t dtmfPayload = kSipDefaultDtmfPayload;
  bool remoteSends = true;
  bool remoteReceives = true;
};

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
  unsigned long cancelStartMs = 0;
  bool mediaReady = false;
  SipMediaInfo media;
  SipConfig config;
};

struct SipCallSession {
  bool active = false;
  bool inbound = false;
  bool awaitingAck = false;
  bool acked = false;
  bool byeSent = false;
  String callID;
  String localTag;
  String remoteTag;
  String remoteContact;
  String remoteUri;
  String requestUri;
  uint32_t localCseq = 1;
  uint32_t remoteCseq = 0;
  IPAddress sipRemoteIp;
  uint16_t sipRemotePort = 0;
  IPAddress rtpRemoteIp;
  uint16_t rtpRemotePort = 0;
  uint8_t audioPayload = 0;
  uint8_t dtmfPayload = kSipDefaultDtmfPayload;
  bool remoteSends = true;
  bool remoteReceives = true;
  bool localSends = true;
  bool localReceives = true;
  unsigned long startMs = 0;
  unsigned long lastRtpSendMs = 0;
  unsigned long lastRtpRecvMs = 0;
  uint16_t rtpSeq = 0;
  uint32_t rtpTimestamp = 0;
  uint32_t rtpSsrc = 0;
  uint8_t lastDtmfEvent = 0xFF;
  unsigned long lastDtmfEndMs = 0;
  SipConfig config;
};

static PendingInvite pendingInvite;
static SipCallSession sipCall;
static SipRingTickCallback ringTickCallback = nullptr;
static SipDtmfCallback dtmfCallback = nullptr;
static unsigned long lastSipNetWarnMs = 0;
static TaskHandle_t sipRtpTxTaskHandle = nullptr;
static bool sipRtpTxTaskRunning = false;
static const unsigned long kSipRingDurationMs = 30000;  // Let phones ring before cancel.
static const unsigned long kSipInCallHoldMs = 60000;    // Hold after answer for intercom audio.
static const unsigned long kSipCancelWaitMs = 3000;     // Wait for 487 after CANCEL.

static String buildCancel(const SipConfig &config,
                          const String& fromTag,
                          const String& callID,
                          const String& branch,
                          uint32_t cseq);
static String buildAck(const SipConfig &config,
                       const String& fromTag,
                       const String& toTag,
                       const String& callID,
                       const String& requestUri,
                       const String& target,
                       uint32_t cseq);
static String buildBye(const SipConfig &config,
                       const String& fromTag,
                       const String& toTag,
                       const String& callID,
                       const String& requestUri,
                       const String& target,
                       uint32_t cseq);
static bool sipSend(const String& msg);
static bool sendSipRtpFrame();

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

void setSipRingTickCallback(SipRingTickCallback callback) {
  ringTickCallback = callback;
}

void setSipDtmfCallback(SipDtmfCallback callback) {
  dtmfCallback = callback;
}

static void serviceRingTick() {
  if (ringTickCallback) {
    ringTickCallback();
  }
}

static void resetSipCall() {
  sipCall = SipCallSession();
}

static void sipRtpTxTask(void *param) {
  const TickType_t interval = pdMS_TO_TICKS(kSipRtpFrameMs);
  TickType_t lastWake = xTaskGetTickCount();
  while (sipCall.active && sipCall.acked) {
    if (sendSipRtpFrame()) {
      sipCall.lastRtpSendMs = millis();
    }
    vTaskDelayUntil(&lastWake, interval);
  }
  sipRtpTxTaskRunning = false;
  sipRtpTxTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void ensureSipRtpTxTask() {
  if (sipRtpTxTaskRunning || !sipCall.active || !sipCall.acked) {
    return;
  }
  sipRtpTxTaskRunning = true;
  BaseType_t result = xTaskCreatePinnedToCore(
      sipRtpTxTask,
      "sip_rtp_tx",
      4096,
      nullptr,
      1,
      &sipRtpTxTaskHandle,
      STREAM_TASK_CORE);
  if (result != pdPASS) {
    sipRtpTxTaskRunning = false;
    sipRtpTxTaskHandle = nullptr;
    logEvent(LOG_ERROR, "❌ Failed to start SIP RTP TX task");
  }
}

static void startOutboundSipCall() {
  if (sipCall.active) {
    return;
  }
  if (!pendingInvite.mediaReady || pendingInvite.media.remotePort == 0) {
    logEvent(LOG_WARN, "⚠️ SIP media missing SDP details; skipping RTP");
    return;
  }
  sipCall.active = true;
  sipCall.inbound = false;
  sipCall.awaitingAck = false;
  sipCall.acked = true;
  sipCall.callID = pendingInvite.callID;
  sipCall.localTag = pendingInvite.fromTag;
  sipCall.remoteTag = pendingInvite.toTag;
  sipCall.remoteContact = pendingInvite.remoteTarget;
  sipCall.remoteUri = "sip:" + pendingInvite.target;
  sipCall.requestUri = "sip:" + pendingInvite.target;
  if (sipCall.remoteContact.isEmpty()) {
    sipCall.remoteContact = sipCall.remoteUri;
  }
  sipCall.rtpRemoteIp = pendingInvite.media.remoteIp;
  sipCall.rtpRemotePort = pendingInvite.media.remotePort;
  sipCall.remoteSends = pendingInvite.media.remoteSends;
  sipCall.remoteReceives = pendingInvite.media.remoteReceives;
  if (pendingInvite.media.preferredAudioPayload != 0xFF) {
    sipCall.audioPayload = pendingInvite.media.preferredAudioPayload;
  } else {
    sipCall.audioPayload = pendingInvite.media.hasPcmu ? 0 : (pendingInvite.media.hasPcma ? 8 : 0);
  }
  sipCall.dtmfPayload = pendingInvite.media.dtmfPayload;
  sipCall.startMs = millis();
  sipCall.lastRtpSendMs = sipCall.startMs;
  sipCall.rtpSeq = static_cast<uint16_t>(esp_random());
  sipCall.rtpTimestamp = static_cast<uint32_t>(esp_random());
  sipCall.rtpSsrc = static_cast<uint32_t>(esp_random());
  sipCall.config = pendingInvite.config;
  sipCall.localCseq = pendingInvite.cseq + 1;
  ensureSipRtpTxTask();
}

bool isSipRingActive() {
  return pendingInvite.active;
}

void processSipRing() {
  if (!pendingInvite.active) {
    return;
  }

  unsigned long now = millis();
  serviceRingTick();

  if (pendingInvite.answered) {
    if (!pendingInvite.ackSent) {
      String requestUri = pendingInvite.remoteTarget.isEmpty()
        ? pendingInvite.target
        : pendingInvite.remoteTarget;
      String ack = buildAck(pendingInvite.config,
                            pendingInvite.fromTag,
                            pendingInvite.toTag,
                            pendingInvite.callID,
                            requestUri,
                            pendingInvite.target,
                            pendingInvite.cseq);
      logEvent(LOG_DEBUG, "---- SIP ACK ----");
      logEvent(LOG_DEBUG, ack);
      pendingInvite.ackSent = sipSend(ack);
      pendingInvite.answeredMs = now;
      if (!pendingInvite.ackSent) {
        pendingInvite.active = false;
        resetSipCall();
        return;
      }
      startOutboundSipCall();
    } else if (!pendingInvite.byeSent &&
               (now - pendingInvite.answeredMs > kSipInCallHoldMs)) {
      String requestUri = pendingInvite.remoteTarget.isEmpty()
        ? pendingInvite.target
        : pendingInvite.remoteTarget;
      String bye = buildBye(pendingInvite.config,
                            pendingInvite.fromTag,
                            pendingInvite.toTag,
                            pendingInvite.callID,
                            requestUri,
                            pendingInvite.target,
                            pendingInvite.cseq + 1);
      logEvent(LOG_DEBUG, "---- SIP BYE ----");
      logEvent(LOG_DEBUG, bye);
      pendingInvite.byeSent = sipSend(bye);
      pendingInvite.active = false;
      resetSipCall();
    }
    return;
  }

  if (now - pendingInvite.inviteStartMs >= kSipRingDurationMs) {
    if (pendingInvite.canCancel && !pendingInvite.cancelSent) {
      String cancel = buildCancel(pendingInvite.config,
                                  pendingInvite.fromTag,
                                  pendingInvite.callID,
                                  pendingInvite.branch,
                                  pendingInvite.cseq);
      logEvent(LOG_DEBUG, "---- SIP CANCEL ----");
      logEvent(LOG_DEBUG, cancel);
      pendingInvite.cancelSent = sipSend(cancel);
      pendingInvite.cancelStartMs = now;
      if (!pendingInvite.cancelSent) {
        pendingInvite.active = false;
        resetSipCall();
      }
    } else if (!pendingInvite.canCancel && !pendingInvite.cancelSent) {
      logEvent(LOG_INFO, "ℹ️ Skipping CANCEL (no provisional response or call answered)");
      pendingInvite.active = false;
      resetSipCall();
      return;
    }
  }

  if (pendingInvite.cancelSent && pendingInvite.cancelStartMs > 0 &&
      (now - pendingInvite.cancelStartMs > kSipCancelWaitMs)) {
    pendingInvite.active = false;
    resetSipCall();
  }
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

static String extractRequestUri(const String& request) {
  int lineEnd = request.indexOf('\n');
  String line = lineEnd >= 0 ? request.substring(0, lineEnd) : request;
  line.trim();
  int firstSpace = line.indexOf(' ');
  if (firstSpace < 0) {
    return "";
  }
  int secondSpace = line.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) {
    return "";
  }
  return line.substring(firstSpace + 1, secondSpace);
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

static String extractHeaderTag(const String& headerValue) {
  int tagPos = headerValue.indexOf("tag=");
  if (tagPos < 0) {
    return "";
  }
  tagPos += 4;
  int tagEnd = headerValue.indexOf(';', tagPos);
  if (tagEnd < 0) {
    tagEnd = headerValue.length();
  }
  return headerValue.substring(tagPos, tagEnd);
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

static String extractSipUriFromHeader(const String& headerValue) {
  String line = headerValue;
  int sipPos = line.indexOf("sip:");
  if (sipPos < 0) {
    return "";
  }
  int endPos = line.indexOf('>', sipPos);
  if (endPos < 0) {
    endPos = line.indexOf(';', sipPos);
  }
  if (endPos < 0) {
    endPos = line.length();
  }
  String uri = line.substring(sipPos, endPos);
  uri.trim();
  return uri;
}

static String stripSipPrefix(const String& uri) {
  if (uri.startsWith("sip:")) {
    return uri.substring(4);
  }
  return uri;
}

static String extractSdpBody(const String& message) {
  int bodyPos = message.indexOf("\r\n\r\n");
  if (bodyPos < 0) {
    return "";
  }
  return message.substring(bodyPos + 4);
}

static SipMediaInfo parseSdpMedia(const String& sdp, const IPAddress& fallbackIp) {
  SipMediaInfo info;
  info.remoteIp = fallbackIp;
  int lineStart = 0;
  while (lineStart < sdp.length()) {
    int lineEnd = sdp.indexOf('\n', lineStart);
    if (lineEnd < 0) {
      lineEnd = sdp.length();
    }
    String line = sdp.substring(lineStart, lineEnd);
    line.replace("\r", "");
    line.trim();
    lineStart = lineEnd + 1;

    if (line.startsWith("c=")) {
      int ipPos = line.indexOf("IN IP4");
      if (ipPos >= 0) {
        String ipStr = line.substring(ipPos + 6);
        ipStr.trim();
        IPAddress ip;
        if (ip.fromString(ipStr)) {
          info.remoteIp = ip;
        }
      }
    } else if (line.startsWith("m=audio")) {
      int firstSpace = line.indexOf(' ');
      int secondSpace = line.indexOf(' ', firstSpace + 1);
      if (firstSpace > 0 && secondSpace > firstSpace) {
        String portStr = line.substring(firstSpace + 1, secondSpace);
        info.remotePort = static_cast<uint16_t>(portStr.toInt());
        int payloadPos = line.indexOf(' ', secondSpace + 1);
        if (payloadPos > 0) {
          String payloads = line.substring(payloadPos + 1);
          payloads.trim();
          int start = 0;
          while (start < payloads.length()) {
            int end = payloads.indexOf(' ', start);
            if (end < 0) {
              end = payloads.length();
            }
            String token = payloads.substring(start, end);
            token.trim();
            if (token == "0") {
              info.hasPcmu = true;
              if (info.preferredAudioPayload == 0xFF) {
                info.preferredAudioPayload = 0;
              }
            } else if (token == "8") {
              info.hasPcma = true;
              if (info.preferredAudioPayload == 0xFF) {
                info.preferredAudioPayload = 8;
              }
            }
            start = end + 1;
          }
        }
      }
    } else if (line.startsWith("a=rtpmap:")) {
      int colon = line.indexOf(':');
      int space = line.indexOf(' ', colon + 1);
      if (colon > 0 && space > colon) {
        String ptStr = line.substring(colon + 1, space);
        uint8_t payloadType = static_cast<uint8_t>(ptStr.toInt());
        String codec = line.substring(space + 1);
        codec.trim();
        codec.toLowerCase();
        if (codec.startsWith("pcmu/8000")) {
          info.hasPcmu = true;
        } else if (codec.startsWith("pcma/8000")) {
          info.hasPcma = true;
        } else if (codec.startsWith("telephone-event/8000")) {
          info.dtmfPayload = payloadType;
        }
      }
    } else if (line == "a=sendonly") {
      info.remoteSends = true;
      info.remoteReceives = false;
    } else if (line == "a=recvonly") {
      info.remoteSends = false;
      info.remoteReceives = true;
    } else if (line == "a=inactive") {
      info.remoteSends = false;
      info.remoteReceives = false;
    } else if (line == "a=sendrecv") {
      info.remoteSends = true;
      info.remoteReceives = true;
    }
  }
  return info;
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

static String getSdpDirection() {
  bool canSend = isMicEnabled() && !isMicMuted();
  bool canReceive = isAudioOutEnabled() && !isAudioOutMuted();
  if (canSend && canReceive) {
    return "sendrecv";
  }
  if (canSend) {
    return "sendonly";
  }
  if (canReceive) {
    return "recvonly";
  }
  return "inactive";
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
    // Silently fail if NVS not ready - this is expected on first boot
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
  if (!sipRtpUdp.begin(SIP_RTP_PORT)) {
    logEvent(LOG_ERROR, "❌ Failed to bind RTP port " + String(SIP_RTP_PORT));
  } else {
    sipRtpReady = true;
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
  String requestUri = "sip:" + target;
  String authUri = "sip:" + String(SIP_DOMAIN);
  String sdp;
  // Offer G.711 audio so FRITZ!Box treats INVITE as a proper intercom call.
  if (localIP != IPAddress(0, 0, 0, 0)) {
    String direction = getSdpDirection();
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
    sdp += "a=ptime:20\r\n";
    sdp += "a=" + direction + "\r\n";
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

static String buildSdpBody(const IPAddress& localIP) {
  if (localIP == IPAddress(0, 0, 0, 0)) {
    return "";
  }
  String direction = getSdpDirection();
  String sdp;
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
  sdp += "a=ptime:20\r\n";
  sdp += "a=" + direction + "\r\n";
  return sdp;
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

static String buildResponse(const String& request,
                            const String& status,
                            const String& toTag,
                            const String& extraHeaders,
                            const String& contentType,
                            const String& body) {
  String via = extractHeaderValue(request, "Via");
  String from = extractHeaderValue(request, "From");
  String to = extractHeaderValue(request, "To");
  String callID = extractHeaderValue(request, "Call-ID");
  String cseq = extractHeaderValue(request, "CSeq");
  if (via.isEmpty() || from.isEmpty() || to.isEmpty() || callID.isEmpty() || cseq.isEmpty()) {
    return "";
  }
  if (!toTag.isEmpty() && to.indexOf("tag=") < 0) {
    to += ";tag=" + toTag;
  }
  String msg = "SIP/2.0 " + status + "\r\n";
  msg += "Via: " + via + "\r\n";
  msg += "From: " + from + "\r\n";
  msg += "To: " + to + "\r\n";
  msg += "Call-ID: " + callID + "\r\n";
  msg += "CSeq: " + cseq + "\r\n";
  if (!extraHeaders.isEmpty()) {
    msg += extraHeaders;
  }
  if (!contentType.isEmpty() && !body.isEmpty()) {
    msg += "Content-Type: " + contentType + "\r\n";
  }
  msg += "Content-Length: " + String(body.length()) + "\r\n";
  msg += "\r\n";
  if (!body.isEmpty()) {
    msg += body;
  }
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
static String __attribute__((unused)) waitForMatchingSipResponse(const String& callID, uint32_t cseq, const char* method, unsigned long timeoutMs = 2000) {
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

static bool __attribute__((unused)) isProvisional(const String& response) {
  return response.startsWith("SIP/2.0 1");
}

static uint8_t linear2ulaw(int16_t pcm) {
  static const int16_t seg_end[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
  int16_t mask;
  int16_t seg;
  uint8_t uval;

  if (pcm < 0) {
    pcm = -pcm;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }

  if (pcm > 0x3FFF) {
    pcm = 0x3FFF;
  }

  pcm += 0x84;
  for (seg = 0; seg < 8; seg++) {
    if (pcm <= seg_end[seg]) {
      break;
    }
  }

  if (seg >= 8) {
    return static_cast<uint8_t>(0x7F ^ mask);
  }

  uval = (seg << 4) | ((pcm >> (seg + 3)) & 0x0F);
  return static_cast<uint8_t>(uval ^ mask);
}

static uint8_t linear2alaw(int32_t pcm) {
  static const int16_t seg_end[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
  int16_t mask;
  int16_t seg;
  uint8_t aval;

  if (pcm >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    pcm = -pcm - 1;
  }

  if (pcm > 0x7FFF) {
    pcm = 0x7FFF;
  }

  for (seg = 0; seg < 8; seg++) {
    if (pcm <= seg_end[seg]) {
      break;
    }
  }

  if (seg >= 8) {
    return static_cast<uint8_t>(0x7F ^ mask);
  }

  aval = static_cast<uint8_t>(seg << 4);
  if (seg < 2) {
    aval |= (pcm >> 4) & 0x0F;
  } else {
    aval |= (pcm >> (seg + 3)) & 0x0F;
  }
  return static_cast<uint8_t>(aval ^ mask);
}

static int16_t ulaw2linear(uint8_t uval) {
  uval = ~uval;
  int t = ((uval & 0x0F) << 3) + 0x84;
  t <<= (uval & 0x70) >> 4;
  return (uval & 0x80) ? (0x84 - t) : (t - 0x84);
}

static int16_t alaw2linear(uint8_t aval) {
  aval ^= 0x55;
  int t = (aval & 0x0F) << 4;
  int seg = (aval & 0x70) >> 4;
  switch (seg) {
    case 0:
      t += 8;
      break;
    case 1:
      t += 0x108;
      break;
    default:
      t += 0x108;
      t <<= seg - 1;
      break;
  }
  return (aval & 0x80) ? t : -t;
}

static void downsampleTo8k(const int16_t *in, size_t inSamples, int16_t *out, size_t outSamples) {
  if (AUDIO_SAMPLE_RATE == kSipAudioSampleRate) {
    size_t samplesToCopy = min(inSamples, outSamples);
    memcpy(out, in, samplesToCopy * sizeof(int16_t));
    if (samplesToCopy < outSamples) {
      memset(out + samplesToCopy, 0, (outSamples - samplesToCopy) * sizeof(int16_t));
    }
    return;
  }
  size_t step = AUDIO_SAMPLE_RATE / kSipAudioSampleRate;
  if (step < 1) {
    step = 1;
  }
  for (size_t i = 0; i < outSamples; i++) {
    size_t start = i * step;
    if (start >= inSamples) {
      out[i] = 0;
      continue;
    }
    int32_t sum = 0;
    size_t count = 0;
    for (size_t j = 0; j < step && (start + j) < inSamples; j++) {
      sum += in[start + j];
      count++;
    }
    out[i] = count > 0 ? static_cast<int16_t>(sum / static_cast<int32_t>(count)) : 0;
  }
}

static void upsampleToOutput(const int16_t *in, size_t inSamples, int16_t *out, size_t outSamples) {
  if (AUDIO_SAMPLE_RATE == kSipAudioSampleRate) {
    size_t samplesToCopy = min(inSamples, outSamples);
    memcpy(out, in, samplesToCopy * sizeof(int16_t));
    if (samplesToCopy < outSamples) {
      memset(out + samplesToCopy, 0, (outSamples - samplesToCopy) * sizeof(int16_t));
    }
    return;
  }
  size_t step = AUDIO_SAMPLE_RATE / kSipAudioSampleRate;
  if (step < 1) {
    step = 1;
  }
  size_t outIdx = 0;
  for (size_t i = 0; i < inSamples && outIdx < outSamples; i++) {
    for (size_t j = 0; j < step && outIdx < outSamples; j++) {
      out[outIdx++] = in[i];
    }
  }
  while (outIdx < outSamples) {
    out[outIdx++] = 0;
  }
}

static void encodeG711(const int16_t *pcm, size_t samples, uint8_t *encoded, uint8_t payloadType) {
  for (size_t i = 0; i < samples; i++) {
    encoded[i] = (payloadType == 8) ? linear2alaw(pcm[i]) : linear2ulaw(pcm[i]);
  }
}

static void decodeG711(const uint8_t *encoded, size_t samples, int16_t *pcm, uint8_t payloadType) {
  for (size_t i = 0; i < samples; i++) {
    pcm[i] = (payloadType == 8) ? alaw2linear(encoded[i]) : ulaw2linear(encoded[i]);
  }
}

static char dtmfEventToChar(uint8_t event) {
  if (event <= 9) {
    return static_cast<char>('0' + event);
  }
  if (event == 10) {
    return '*';
  }
  if (event == 11) {
    return '#';
  }
  if (event >= 12 && event <= 15) {
    return static_cast<char>('A' + (event - 12));
  }
  return '\0';
}

static uint8_t g711SilenceByte(uint8_t payloadType) {
  return (payloadType == 8) ? 0xD5 : 0xFF;
}

static void handleSipDtmfEvent(uint8_t event, bool end) {
  if (!end) {
    return;
  }
  unsigned long now = millis();
  if (event == sipCall.lastDtmfEvent && (now - sipCall.lastDtmfEndMs) < 250) {
    return;
  }
  char digit = dtmfEventToChar(event);
  if (digit != '\0' && dtmfCallback) {
    dtmfCallback(digit);
  }
  sipCall.lastDtmfEvent = event;
  sipCall.lastDtmfEndMs = now;
}

static void handleSipRtpPacket(const uint8_t *data, size_t len, const IPAddress &remoteIp) {
  if (!sipCall.active || !sipCall.acked) {
    return;
  }
  if (!sipCall.remoteSends) {
    return;
  }
  if (sipCall.rtpRemoteIp != IPAddress(0, 0, 0, 0) && remoteIp != sipCall.rtpRemoteIp) {
    return;
  }
  if (len < 12) {
    return;
  }

  uint8_t vpxcc = data[0];
  if ((vpxcc >> 6) != 2) {
    return;
  }
  bool hasExtension = (vpxcc & 0x10) != 0;
  uint8_t csrcCount = vpxcc & 0x0F;
  size_t headerLen = 12 + (csrcCount * 4);
  if (len < headerLen) {
    return;
  }
  if (hasExtension) {
    if (len < headerLen + 4) {
      return;
    }
    uint16_t extLenWords = (data[headerLen + 2] << 8) | data[headerLen + 3];
    headerLen += 4 + (extLenWords * 4);
    if (len < headerLen) {
      return;
    }
  }

  uint8_t payloadType = data[1] & 0x7F;
  const uint8_t *payload = data + headerLen;
  size_t payloadLen = len - headerLen;
  if (payloadLen == 0) {
    return;
  }

  if (payloadType == sipCall.dtmfPayload) {
    if (payloadLen >= 4) {
      uint8_t event = payload[0];
      bool end = (payload[1] & 0x80) != 0;
      handleSipDtmfEvent(event, end);
    }
    return;
  }

  if (payloadType != 0 && payloadType != 8) {
    return;
  }
  if (!isAudioOutEnabled() || isAudioOutMuted()) {
    return;
  }

  size_t samples = min(payloadLen, kSipRtpSamplesPerPacket);
  int16_t pcm[kSipRtpSamplesPerPacket];
  decodeG711(payload, samples, pcm, payloadType);

  const size_t outSamples = (AUDIO_SAMPLE_RATE / kSipAudioSampleRate) * kSipRtpSamplesPerPacket;
  int16_t outBuffer[outSamples];
  upsampleToOutput(pcm, samples, outBuffer, outSamples);
  playAudioSamples(outBuffer, outSamples, 5);
  sipCall.lastRtpRecvMs = millis();
}

static bool sendSipRtpFrame() {
  if (!sipCall.active || !sipCall.acked) {
    return false;
  }
  if (!sipRtpReady || sipCall.rtpRemotePort == 0 || sipCall.rtpRemoteIp == IPAddress(0, 0, 0, 0)) {
    return false;
  }
  if (!sipCall.remoteReceives) {
    return false;
  }
  if (!isMicEnabled()) {
    return false;
  }

  int16_t micBuffer[kSipMicSamplesPerPacket];
  int16_t audioBuffer[kSipRtpSamplesPerPacket];
  uint8_t encoded[kSipRtpSamplesPerPacket];

  bool hasMic = !isMicMuted() && captureMicSamples(micBuffer, kSipMicSamplesPerPacket, kSipMicReadTimeoutMs);
  if (hasMic) {
    downsampleTo8k(micBuffer, kSipMicSamplesPerPacket, audioBuffer, kSipRtpSamplesPerPacket);
    encodeG711(audioBuffer, kSipRtpSamplesPerPacket, encoded, sipCall.audioPayload);
  } else {
    memset(encoded, g711SilenceByte(sipCall.audioPayload), sizeof(encoded));
  }

  if (sipCall.rtpSsrc == 0) {
    sipCall.rtpSsrc = static_cast<uint32_t>(esp_random());
  }

  uint8_t packet[12 + kSipRtpSamplesPerPacket];
  packet[0] = 0x80;
  packet[1] = sipCall.audioPayload;
  packet[2] = (sipCall.rtpSeq >> 8) & 0xFF;
  packet[3] = sipCall.rtpSeq & 0xFF;
  packet[4] = (sipCall.rtpTimestamp >> 24) & 0xFF;
  packet[5] = (sipCall.rtpTimestamp >> 16) & 0xFF;
  packet[6] = (sipCall.rtpTimestamp >> 8) & 0xFF;
  packet[7] = sipCall.rtpTimestamp & 0xFF;
  packet[8] = (sipCall.rtpSsrc >> 24) & 0xFF;
  packet[9] = (sipCall.rtpSsrc >> 16) & 0xFF;
  packet[10] = (sipCall.rtpSsrc >> 8) & 0xFF;
  packet[11] = sipCall.rtpSsrc & 0xFF;
  memcpy(packet + 12, encoded, kSipRtpSamplesPerPacket);

  if (!sipRtpUdp.beginPacket(sipCall.rtpRemoteIp, sipCall.rtpRemotePort)) {
    return false;
  }
  size_t written = sipRtpUdp.write(packet, sizeof(packet));
  if (written != sizeof(packet)) {
    sipRtpUdp.endPacket();
    return false;
  }
  if (!sipRtpUdp.endPacket()) {
    return false;
  }

  sipCall.rtpSeq++;
  sipCall.rtpTimestamp += kSipRtpSamplesPerPacket;
  return true;
}

static bool sendSipBye() {
  if (!sipCall.active || sipCall.byeSent) {
    return false;
  }
  String requestUri = sipCall.remoteContact.isEmpty() ? sipCall.requestUri : sipCall.remoteContact;
  String toTarget = stripSipPrefix(sipCall.remoteUri.isEmpty() ? sipCall.requestUri : sipCall.remoteUri);
  if (requestUri.isEmpty() || toTarget.isEmpty()) {
    return false;
  }
  uint32_t cseq = sipCall.localCseq++;
  String bye = buildBye(sipCall.config,
                        sipCall.localTag,
                        sipCall.remoteTag,
                        sipCall.callID,
                        requestUri,
                        toTarget,
                        cseq);
  logEvent(LOG_DEBUG, "---- SIP BYE ----");
  logEvent(LOG_DEBUG, bye);
  bool sent = false;
  if (sipCall.sipRemoteIp != IPAddress(0, 0, 0, 0) && sipCall.sipRemotePort != 0) {
    sent = sipSendTo(sipCall.sipRemoteIp, sipCall.sipRemotePort, bye);
  } else {
    sent = sipSend(bye);
  }
  sipCall.byeSent = sent;
  return sent;
}

void processSipMedia() {
  // RTP receive loop for active SIP calls; TX runs in a dedicated task.
  if (!sipCall.active || !sipCall.acked) {
    return;
  }
  ensureSipRtpTxTask();
  unsigned long now = millis();
  if (sipCall.inbound && kSipMaxCallMs > 0 &&
      (now - sipCall.startMs) > kSipMaxCallMs) {
    sendSipBye();
    resetSipCall();
    return;
  }

  if (!sipRtpReady) {
    return;
  }
  for (int i = 0; i < 4; i++) {
    int packetSize = sipRtpUdp.parsePacket();
    if (packetSize <= 0) {
      break;
    }
    if (packetSize > 0) {
      uint8_t buffer[512];
      int len = sipRtpUdp.read(buffer, sizeof(buffer));
      if (len > 0) {
        handleSipRtpPacket(buffer, static_cast<size_t>(len), sipRtpUdp.remoteIP());
      }
    }
  }
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
    if (method.equalsIgnoreCase("OPTIONS")) {
      String headers = "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS\r\n";
      String ok = buildResponse(resp, "200 OK", "", headers, "", "");
      if (!ok.isEmpty()) {
        logEvent(LOG_DEBUG, "---- SIP TX ----");
        logEvent(LOG_DEBUG, ok);
        sipSendResponse(ok);
      }
      return;
    }
    if (method.equalsIgnoreCase("INVITE")) {
      // Incoming call: answer immediately with SDP so FRITZ!Box treats us as an intercom.
      if (pendingInvite.active || sipCall.active) {
        String busy = buildResponse(resp, "486 Busy Here", "", "", "", "");
        if (!busy.isEmpty()) {
          logEvent(LOG_DEBUG, "---- SIP TX ----");
          logEvent(LOG_DEBUG, busy);
          sipSendResponse(busy);
        }
        return;
      }

      String callId = extractHeaderValue(resp, "Call-ID");
      callId.trim();
      if (callId.isEmpty()) {
        return;
      }

      uint32_t cseq = 0;
      String cseqMethod;
      parseCSeq(resp, &cseq, &cseqMethod);

      String fromLine = extractHeaderValue(resp, "From");
      String fromTag = extractHeaderTag(fromLine);
      String requestUri = extractRequestUri(resp);
      String remoteContact = extractContactUri(resp);
      String remoteUri = extractSipUriFromHeader(fromLine);
      if (remoteContact.isEmpty()) {
        remoteContact = remoteUri;
      }

      String sdp = extractSdpBody(resp);
      SipMediaInfo media = parseSdpMedia(sdp, sipUdp.remoteIP());

      sipCall.active = true;
      sipCall.inbound = true;
      sipCall.awaitingAck = true;
      sipCall.acked = false;
      sipCall.callID = callId;
      sipCall.localTag = generateTag();
      sipCall.remoteTag = fromTag;
      sipCall.remoteContact = remoteContact;
      sipCall.remoteUri = remoteUri;
      sipCall.requestUri = requestUri;
      sipCall.remoteCseq = cseq;
      sipCall.sipRemoteIp = sipUdp.remoteIP();
      sipCall.sipRemotePort = sipUdp.remotePort();
      sipCall.rtpRemoteIp = media.remoteIp;
      sipCall.rtpRemotePort = media.remotePort;
      sipCall.remoteSends = media.remoteSends;
      sipCall.remoteReceives = media.remoteReceives;
      if (media.preferredAudioPayload != 0xFF) {
        sipCall.audioPayload = media.preferredAudioPayload;
      } else {
        sipCall.audioPayload = media.hasPcmu ? 0 : (media.hasPcma ? 8 : 0);
      }
      sipCall.dtmfPayload = media.dtmfPayload;
      sipCall.startMs = millis();
      sipCall.rtpSeq = static_cast<uint16_t>(esp_random());
      sipCall.rtpTimestamp = static_cast<uint32_t>(esp_random());
      sipCall.rtpSsrc = static_cast<uint32_t>(esp_random());
      sipCall.localCseq = 1;
      loadSipConfig(sipCall.config);
      logEvent(LOG_INFO, "📞 SIP inbound INVITE received");

      String trying = buildResponse(resp, "100 Trying", "", "", "", "");
      if (!trying.isEmpty()) {
        logEvent(LOG_DEBUG, "---- SIP TX ----");
        logEvent(LOG_DEBUG, trying);
        sipSendResponse(trying);
      }

      String sdpBody = buildSdpBody(WiFi.localIP());
      String contactHeader;
      IPAddress localIp = WiFi.localIP();
      if (localIp != IPAddress(0, 0, 0, 0) && !sipCall.config.sip_user.isEmpty()) {
        contactHeader = "Contact: <sip:" + sipCall.config.sip_user + "@" +
                        localIp.toString() + ":" + String(LOCAL_SIP_PORT) + ">\r\n";
      }
      String ok = buildResponse(resp, "200 OK", sipCall.localTag, contactHeader, "application/sdp", sdpBody);
      if (!ok.isEmpty()) {
        logEvent(LOG_DEBUG, "---- SIP TX ----");
        logEvent(LOG_DEBUG, ok);
        sipSendResponse(ok);
      }
      return;
    }
    if (method.equalsIgnoreCase("ACK")) {
      String callId = extractHeaderValue(resp, "Call-ID");
      callId.trim();
      if (sipCall.active && sipCall.inbound && callId.equalsIgnoreCase(sipCall.callID)) {
        sipCall.acked = true;
        sipCall.awaitingAck = false;
        sipCall.startMs = millis();
        ensureSipRtpTxTask();
      }
      return;
    }
    if (method.equalsIgnoreCase("BYE") || method.equalsIgnoreCase("CANCEL")) {
      String ok = buildOkResponse(resp);
      if (!ok.isEmpty()) {
        logEvent(LOG_DEBUG, "---- SIP TX ----");
        logEvent(LOG_DEBUG, ok);
        sipSendResponse(ok);
      }
      pendingInvite.active = false;
      pendingInvite.answered = false;
      resetSipCall();
      return;
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
    String sdp = extractSdpBody(resp);
    if (!sdp.isEmpty()) {
      pendingInvite.media = parseSdpMedia(sdp, sipUdp.remoteIP());
      pendingInvite.mediaReady = pendingInvite.media.remotePort > 0;
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

  lastRegisterAttemptMs = millis();
  lastRegisterSuccessful = false;

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
            lastRegisterSuccessful = true;
            lastRegisterOkMs = millis();
          } else {
            logEvent(LOG_ERROR, "❌ SIP registration failed");
          }
        }
      } else {
        logEvent(LOG_ERROR, "❌ Failed to parse auth challenge");
      }
    } else if (isSuccess(response)) {
      logEvent(LOG_INFO, "✅ SIP registration successful (no auth required)!");
      lastRegisterSuccessful = true;
      lastRegisterOkMs = millis();
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
  if (sipCall.active) {
    return;
  }
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
  if (pendingInvite.active) {
    logEvent(LOG_INFO, "ℹ️ SIP ring already active");
    return false;
  }
  if (sipCall.active) {
    logEvent(LOG_INFO, "ℹ️ SIP call already active");
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
  pendingInvite.cancelStartMs = 0;
  pendingInvite.mediaReady = false;
  pendingInvite.media = SipMediaInfo();
  pendingInvite.config = config;

  return true;
}

bool isSipRegistrationOk() {
  if (!lastRegisterSuccessful) {
    return false;
  }
  if (lastRegisterAttemptMs == 0) {
    return false;
  }
  unsigned long now = millis();
  return (now - lastRegisterOkMs) <= (REGISTER_INTERVAL_MS * 2);
}

unsigned long getSipLastRegisterOkMs() {
  return lastRegisterOkMs;
}

unsigned long getSipLastRegisterAttemptMs() {
  return lastRegisterAttemptMs;
}
