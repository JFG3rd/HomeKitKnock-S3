/*
 * Project: HomeKitKnock-S3
 * File: src/rtsp_server.cpp
 * Author: Jesse Greene
 * 
 * Custom RTSP server implementation that shares the ESP32 camera
 * initialized by cameraAPI.cpp. Implements RTSP protocol manually
 * to stream MJPEG frames over RTP.
 */

#include "camera_pins.h"
#include "config.h"
#include "rtsp_server.h"

#ifdef CAMERA

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "logger.h"
#include "audio.h"

// JPEG marker constants
#define JPEG_MARKER_SOI   0xFFD8  // Start of Image
#define JPEG_MARKER_APP0  0xFFE0  // APP0 (JFIF)
#define JPEG_MARKER_DQT   0xFFDB  // Define Quantization Table
#define JPEG_MARKER_SOF0  0xFFC0  // Start of Frame (baseline)
#define JPEG_MARKER_DHT   0xFFC4  // Define Huffman Table
#define JPEG_MARKER_SOS   0xFFDA  // Start of Scan
#define JPEG_MARKER_EOI   0xFFD9  // End of Image

// Parse JPEG to find scan data offset (after SOS marker)
static size_t findJpegScanData(const uint8_t* jpeg, size_t len, uint8_t &type, uint8_t &q) {
  size_t i = 0;
  type = 0;  // Default: YUV 4:2:0
  q = 255;   // Default Q (will use dynamic tables)
  
  // Check for JPEG SOI marker
  if (len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    return 0;
  }
  
  i = 2;
  
  // Parse JPEG markers until we find SOS
  while (i < len - 1) {
    if (jpeg[i] != 0xFF) {
      return 0; // Invalid JPEG
    }
    
    uint8_t marker = jpeg[i + 1];
    i += 2;
    
    // SOF0 marker - read to determine chroma subsampling
    if (marker == 0xC0) {
      if (i + 9 > len) return 0;
      uint16_t sofLen = (jpeg[i] << 8) | jpeg[i + 1];
      // Byte 5 = number of components (should be 3 for YUV)
      // Bytes 6-8 = Y component (ID, H/V sampling, Q table)
      // Check Y component H/V sampling factor at offset i+6
      if (i + 6 < len) {
        uint8_t ySampling = jpeg[i + 6];
        // High nibble = H sampling, low nibble = V sampling
        // 0x22 = 2x2 (4:2:0), 0x21 = 2x1 (4:2:2), 0x11 = 1x1 (4:4:4)
        if (ySampling == 0x21) {
          type = 1; // 4:2:2
        } else if (ySampling == 0x22) {
          type = 0; // 4:2:0
        }
      }
    }
    
    // SOS marker - scan data starts after this marker's length field
    if (marker == 0xDA) {
      if (i + 2 > len) return 0;
      uint16_t sosLen = (jpeg[i] << 8) | jpeg[i + 1];
      return i + sosLen;  // Return offset to scan data
    }
    
    // Skip marker data (except standalone markers like SOI/EOI/RST)
    // RST markers are 0xD0-0xD7, SOI is 0xD8, EOI is 0xD9
    if (marker != 0xD8 && marker != 0xD9 && (marker < 0xD0 || marker > 0xD7)) {
      if (i + 2 > len) return 0;
      uint16_t markerLen = (jpeg[i] << 8) | jpeg[i + 1];
      i += markerLen;
    }
  }
  
  return 0; // SOS not found
}

// RTSP server configuration
#define RTSP_PORT 8554

// RTSP session state
struct RtspSession {
  WiFiClient client;
  WiFiUDP rtpSocket;
  // RTCP sockets/ports are tracked for future sender reports (not emitted yet).
  WiFiUDP rtcpSocket;
  WiFiUDP audioRtpSocket;
  WiFiUDP audioRtcpSocket;
  uint16_t rtpPort;
  uint16_t rtcpPort;
  uint16_t audioRtpPort;
  uint16_t audioRtcpPort;
  uint32_t sessionId;
  uint16_t sequenceNumber;
  uint32_t timestamp;
  uint32_t ssrc;
  uint16_t audioSequenceNumber;
  uint32_t audioTimestamp;
  uint32_t audioSsrc;
  IPAddress clientIP;
  bool isPlaying;
  bool rtpSocketInitialized;
  bool audioSocketInitialized;
  bool audioSetup;
  unsigned long lastFrameMs;
  unsigned long lastAudioMs;
  unsigned long lastActivityMs;  // Track last client activity for timeout detection
  
  // TCP interleaved mode (RTP over RTSP TCP connection)
  bool useTcpInterleaved;
  uint8_t interleavedRtpChannel;
  uint8_t interleavedRtcpChannel;
  bool audioUseTcpInterleaved;
  uint8_t interleavedAudioRtpChannel;
  uint8_t interleavedAudioRtcpChannel;
};

// RTSP server objects
static WiFiServer* rtspServer = nullptr;
static RtspSession* activeSessions[4] = {nullptr};
static const int MAX_SESSIONS = 4;
static TaskHandle_t rtspTaskHandle = nullptr;
static bool rtspTaskRunning = false;
static bool rtspAllowUdp = false;
static uint16_t lastFrameWidth = 0;
static uint16_t lastFrameHeight = 0;

static void rtspTask(void *pvParameters);
bool rtspRunning = false;

// Generate random SSRC identifier
static uint32_t generateSSRC() {
  return esp_random();
}

// Generate unique session ID
static uint32_t generateSessionId() {
  return (millis() & 0xFFFFFF) | (esp_random() & 0xFF000000);
}

// Find free session slot
static int findFreeSessionSlot() {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (activeSessions[i] == nullptr) {
      return i;
    }
  }
  return -1;
}

// Parse RTSP request line
static String getRequestMethod(String request) {
  int spaceIndex = request.indexOf(' ');
  if (spaceIndex > 0) {
    return request.substring(0, spaceIndex);
  }
  return "";
}

// Extract CSeq from RTSP headers
static int getCSeq(String request) {
  int cseqIndex = request.indexOf("CSeq:");
  if (cseqIndex >= 0) {
    int lineEnd = request.indexOf('\r', cseqIndex);
    String cseqLine = request.substring(cseqIndex + 5, lineEnd);
    cseqLine.trim();
    return cseqLine.toInt();
  }
  return 1;
}

// Extract Transport header
static String getTransport(String request) {
  int transportIndex = request.indexOf("Transport:");
  if (transportIndex >= 0) {
    int lineEnd = request.indexOf('\r', transportIndex);
    String transport = request.substring(transportIndex + 10, lineEnd);
    transport.trim();
    return transport;
  }
  return "";
}

static bool getSessionIdFromRequest(String request, uint32_t &sessionId) {
  int sessionIndex = request.indexOf("Session:");
  if (sessionIndex < 0) {
    return false;
  }
  int lineEnd = request.indexOf('\r', sessionIndex);
  if (lineEnd < 0) {
    lineEnd = request.length();
  }
  String sessionLine = request.substring(sessionIndex + 8, lineEnd);
  sessionLine.trim();
  int semicolonIndex = sessionLine.indexOf(';');
  if (semicolonIndex > 0) {
    sessionLine = sessionLine.substring(0, semicolonIndex);
  }
  sessionId = strtoul(sessionLine.c_str(), NULL, 16);
  return sessionId != 0;
}

// Extract client RTP ports from Transport header
static bool parseClientPorts(String transport, uint16_t &rtpPort, uint16_t &rtcpPort) {
  int clientPortIndex = transport.indexOf("client_port=");
  if (clientPortIndex >= 0) {
    int dashIndex = transport.indexOf('-', clientPortIndex);
    if (dashIndex > 0) {
      String rtpStr = transport.substring(clientPortIndex + 12, dashIndex);
      int semicolonIndex = transport.indexOf(';', dashIndex);
      String rtcpStr;
      if (semicolonIndex > 0) {
        rtcpStr = transport.substring(dashIndex + 1, semicolonIndex);
      } else {
        rtcpStr = transport.substring(dashIndex + 1);
      }
      rtpPort = rtpStr.toInt();
      rtcpPort = rtcpStr.toInt();
      return true;
    }
  }
  return false;
}

// Extract interleaved channels from Transport header (e.g., "interleaved=0-1")
static bool parseInterleaved(String transport, uint8_t &rtpChannel, uint8_t &rtcpChannel) {
  int interleavedIndex = transport.indexOf("interleaved=");
  if (interleavedIndex >= 0) {
    int dashIndex = transport.indexOf('-', interleavedIndex);
    if (dashIndex > 0) {
      String rtpStr = transport.substring(interleavedIndex + 12, dashIndex);
      int semicolonIndex = transport.indexOf(';', dashIndex);
      String rtcpStr;
      if (semicolonIndex > 0) {
        rtcpStr = transport.substring(dashIndex + 1, semicolonIndex);
      } else {
        rtcpStr = transport.substring(dashIndex + 1);
      }
      rtpChannel = rtpStr.toInt();
      rtcpChannel = rtcpStr.toInt();
      return true;
    }
  }
  return false;
}

// Find an active RTSP session slot by client instance.
static int findSessionSlotByClient(const WiFiClient &client) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (activeSessions[i] && activeSessions[i]->client == client) {
      return i;
    }
  }
  return -1;
}

// Find an active RTSP session slot by session ID.
static int findSessionSlotById(uint32_t sessionId) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (activeSessions[i] && activeSessions[i]->sessionId == sessionId) {
      return i;
    }
  }
  return -1;
}

// Send RTSP response
static void sendRtspResponse(WiFiClient &client, int cseq, String status, String extraHeaders = "") {
  String response = "RTSP/1.0 " + status + "\r\n";
  response += "CSeq: " + String(cseq) + "\r\n";
  if (extraHeaders.length() > 0) {
    response += extraHeaders;
  }
  response += "\r\n";
  client.print(response);
}

// Handle DESCRIBE request - return SDP
static void handleDescribe(WiFiClient &client, int cseq) {
  String rtspUrl = "rtsp://" + WiFi.localIP().toString() + ":" + String(RTSP_PORT) + "/mjpeg/1";
  
  String sdp = "v=0\r\n";
  sdp += "o=- 0 0 IN IP4 " + WiFi.localIP().toString() + "\r\n";
  sdp += "s=ESP32-S3 Camera\r\n";
  sdp += "c=IN IP4 0.0.0.0\r\n";
  sdp += "t=0 0\r\n";
  sdp += "a=control:" + rtspUrl + "\r\n";
  sdp += "m=video 0 RTP/AVP 26\r\n";
  sdp += "a=rtpmap:26 JPEG/90000\r\n";
  if (lastFrameWidth > 0 && lastFrameHeight > 0) {
    sdp += "a=framesize:26 " + String(lastFrameWidth) + "-" + String(lastFrameHeight) + "\r\n";
  }
  sdp += "a=control:" + rtspUrl + "/track1\r\n";
  // Only advertise audio when the mic is enabled so clients don't SETUP a dead track.
  if (isMicEnabled()) {
    sdp += "m=audio 0 RTP/AVP 0\r\n";
    sdp += "a=rtpmap:0 PCMU/8000\r\n";
    sdp += "a=control:" + rtspUrl + "/track2\r\n";
  }

  String headers = "Content-Base: " + rtspUrl + "/\r\n";
  headers += "Content-Type: application/sdp\r\n";
  headers += "Content-Length: " + String(sdp.length()) + "\r\n";
  
  String response = "RTSP/1.0 200 OK\r\n";
  response += "CSeq: " + String(cseq) + "\r\n";
  response += headers + "\r\n";
  response += sdp;
  
  client.print(response);
}

// Handle SETUP request - create RTP/RTCP session
static void handleSetup(WiFiClient &client, int cseq, String request, String transport) {
  String track = request.indexOf("track2") >= 0 ? "audio" : "video";
  logEvent(LOG_INFO, "RTSP SETUP " + track + " transport=" + transport);

  bool isAudioTrack = request.indexOf("track2") >= 0;
  if (isAudioTrack && !isMicEnabled()) {
    sendRtspResponse(client, cseq, "404 Not Found");
    return;
  }
  bool useTcp = transport.indexOf("RTP/AVP/TCP") >= 0;
  uint8_t rtpChannel = 0;
  uint8_t rtcpChannel = 1;
  uint16_t clientRtpPort = 0;
  uint16_t clientRtcpPort = 0;

  if (useTcp) {
    if (!parseInterleaved(transport, rtpChannel, rtcpChannel)) {
      rtpChannel = isAudioTrack ? 2 : 0;
      rtcpChannel = isAudioTrack ? 3 : 1;
    }
  } else {
    if (!rtspAllowUdp) {
      logEvent(LOG_WARN, "⚠️ RTSP UDP requested while disabled");
      sendRtspResponse(client, cseq, "461 Unsupported Transport");
      return;
    }
    if (!parseClientPorts(transport, clientRtpPort, clientRtcpPort)) {
      sendRtspResponse(client, cseq, "461 Unsupported Transport");
      return;
    }
  }

  RtspSession* session = nullptr;
  int slot = -1;
  uint32_t requestedSessionId = 0;
  if (getSessionIdFromRequest(request, requestedSessionId)) {
    slot = findSessionSlotById(requestedSessionId);
    if (slot < 0) {
      sendRtspResponse(client, cseq, "454 Session Not Found");
      return;
    }
    session = activeSessions[slot];
  } else {
    slot = findFreeSessionSlot();
    if (slot < 0) {
      sendRtspResponse(client, cseq, "453 Not Enough Bandwidth");
      return;
    }
    session = new RtspSession();
    session->client = client;
    session->rtpPort = 0;
    session->rtcpPort = 0;
    session->audioRtpPort = 0;
    session->audioRtcpPort = 0;
    session->sessionId = generateSessionId();
    session->sequenceNumber = 0;
    session->timestamp = 0;
    session->ssrc = generateSSRC();
    session->audioSequenceNumber = 0;
    session->audioTimestamp = 0;
    session->audioSsrc = generateSSRC();
    session->clientIP = client.remoteIP();
    session->isPlaying = false;
    session->rtpSocketInitialized = false;
    session->audioSocketInitialized = false;
    session->audioSetup = false;
    session->lastFrameMs = 0;
    session->lastAudioMs = 0;
    session->lastActivityMs = millis();
    session->useTcpInterleaved = false;
    session->interleavedRtpChannel = 0;
    session->interleavedRtcpChannel = 1;
    session->audioUseTcpInterleaved = false;
    session->interleavedAudioRtpChannel = 2;
    session->interleavedAudioRtcpChannel = 3;

    activeSessions[slot] = session;
  }

  if (isAudioTrack) {
    session->audioRtpPort = clientRtpPort;
    session->audioRtcpPort = clientRtcpPort;
    session->audioUseTcpInterleaved = useTcp;
    session->interleavedAudioRtpChannel = rtpChannel;
    session->interleavedAudioRtcpChannel = rtcpChannel;
    session->audioSetup = true;
  } else {
    session->rtpPort = clientRtpPort;
    session->rtcpPort = clientRtcpPort;
    session->useTcpInterleaved = useTcp;
    session->interleavedRtpChannel = rtpChannel;
    session->interleavedRtcpChannel = rtcpChannel;
  }
  session->lastActivityMs = millis();

  String headers;
  if (useTcp) {
    headers = "Transport: RTP/AVP/TCP;unicast;interleaved=" + String(rtpChannel) + "-" + String(rtcpChannel) + "\r\n";
    logEvent(LOG_INFO, "🔌 RTSP SETUP: TCP " + String(isAudioTrack ? "audio" : "video") + " channels " + String(rtpChannel) + "-" + String(rtcpChannel));
  } else {
    headers = "Transport: RTP/AVP;unicast;client_port=" + String(clientRtpPort) + "-" + String(clientRtcpPort) + "\r\n";
    logEvent(LOG_INFO, "🔌 RTSP SETUP: UDP " + String(isAudioTrack ? "audio" : "video") + " ports " + String(clientRtpPort) + "-" + String(clientRtcpPort));
  }
  headers += "Session: " + String(session->sessionId, HEX) + ";timeout=60\r\n";

  sendRtspResponse(client, cseq, "200 OK", headers);
  logEvent(LOG_INFO, "🔌 RTSP Session " + String(session->sessionId, HEX) + " for " + client.remoteIP().toString());
}

// Handle PLAY request - start streaming
static void handlePlay(WiFiClient &client, int cseq, String request) {
  // Extract session ID from request
  int sessionIndex = request.indexOf("Session:");
  if (sessionIndex < 0) {
    sendRtspResponse(client, cseq, "454 Session Not Found");
    return;
  }
  
  int lineEnd = request.indexOf('\r', sessionIndex);
  String sessionLine = request.substring(sessionIndex + 8, lineEnd);
  sessionLine.trim();
  uint32_t requestedSessionId = strtoul(sessionLine.c_str(), NULL, 16);

  // Find matching session
  RtspSession* session = nullptr;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (activeSessions[i] && activeSessions[i]->sessionId == requestedSessionId) {
      session = activeSessions[i];
      break;
    }
  }

  if (!session) {
    sendRtspResponse(client, cseq, "454 Session Not Found");
    return;
  }

  session->isPlaying = true;
  session->lastFrameMs = millis();
  session->lastAudioMs = session->lastFrameMs;
  session->lastActivityMs = millis();  // Update activity timestamp

  String headers = "Session: " + String(session->sessionId, HEX) + "\r\n";
  sendRtspResponse(client, cseq, "200 OK", headers);
  
  logEvent(LOG_INFO, "▶️ RTSP PLAY: Session " + String(session->sessionId, HEX));
}

// Handle TEARDOWN request - close session
static void handleTeardown(WiFiClient &client, int cseq, String request, int slot) {
  if (slot >= 0 && slot < MAX_SESSIONS && activeSessions[slot]) {
    RtspSession* session = activeSessions[slot];
    
    String headers = "Session: " + String(session->sessionId, HEX) + "\r\n";
    sendRtspResponse(client, cseq, "200 OK", headers);
    
    logEvent(LOG_INFO, "🛑 RTSP TEARDOWN: Session " + String(session->sessionId, HEX));
    
    session->rtpSocket.stop();
    session->rtcpSocket.stop();
    session->audioRtpSocket.stop();
    session->audioRtcpSocket.stop();
    session->client.stop();
    delete session;
    activeSessions[slot] = nullptr;
  } else {
    sendRtspResponse(client, cseq, "454 Session Not Found");
  }
}

// Send RTP JPEG packet over TCP interleaved mode (RFC 2326 section 10.12)
// Format: $ + channel (1 byte) + length (2 bytes big-endian) + RTP packet
// Implements RFC 2435 - RTP Payload Format for JPEG-compressed Video
static void sendRtpJpegTcp(RtspSession* session, camera_fb_t* fb) {
  if (!session || !fb || !session->isPlaying || !session->client.connected()) {
    return;
  }

  // Parse JPEG to extract scan data (RFC 2435 sends only entropy-coded segment)
  uint8_t jpegType = 0;
  uint8_t jpegQ = 0;  // Will be set by parser
  size_t scanDataOffset = findJpegScanData(fb->buf, fb->len, jpegType, jpegQ);
  
  // Force Q=80 to avoid needing Q-table header (required for Q >= 128)
  jpegQ = 80;
  
  if (scanDataOffset == 0 || scanDataOffset >= fb->len) {
    logEvent(LOG_ERROR, "❌ JPEG parse failed: offset=" + String(scanDataOffset) + " len=" + String(fb->len));
    return;
  }
  
  // Calculate scan data length (exclude EOI marker at end)
  size_t scanDataLen = fb->len - scanDataOffset;
  if (scanDataLen >= 2 && fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
    scanDataLen -= 2;  // Remove EOI marker
  }
  
  const uint8_t* scanData = fb->buf + scanDataOffset;
  uint16_t width = fb->width;
  uint16_t height = fb->height;
  
  // RFC 2435: Fragment if needed (max ~1400 bytes per packet)
  const size_t JPEG_HEADER_SIZE = 8;
  const size_t MAX_RTP_PAYLOAD = 1200 - JPEG_HEADER_SIZE;
  
  size_t offset = 0;
  uint32_t fragmentOffset = 0;
  
  while (offset < scanDataLen) {
    size_t chunkSize = min((size_t)MAX_RTP_PAYLOAD, scanDataLen - offset);
    bool isLastFragment = (offset + chunkSize >= scanDataLen);
    
    // Build RTP header (12 bytes) + JPEG header (8 bytes) + scan data
    uint8_t rtpPacket[12 + JPEG_HEADER_SIZE + MAX_RTP_PAYLOAD];
    
    // === RTP Header (12 bytes) ===
    rtpPacket[0] = 0x80;  // V=2, P=0, X=0, CC=0
    rtpPacket[1] = isLastFragment ? 0x9A : 0x1A;  // M bit + PT=26 (JPEG)
    
    // Sequence number (big-endian)
    rtpPacket[2] = (session->sequenceNumber >> 8) & 0xFF;
    rtpPacket[3] = session->sequenceNumber & 0xFF;
    
    // Timestamp (big-endian, 90kHz)
    rtpPacket[4] = (session->timestamp >> 24) & 0xFF;
    rtpPacket[5] = (session->timestamp >> 16) & 0xFF;
    rtpPacket[6] = (session->timestamp >> 8) & 0xFF;
    rtpPacket[7] = session->timestamp & 0xFF;
    
    // SSRC (big-endian)
    rtpPacket[8] = (session->ssrc >> 24) & 0xFF;
    rtpPacket[9] = (session->ssrc >> 16) & 0xFF;
    rtpPacket[10] = (session->ssrc >> 8) & 0xFF;
    rtpPacket[11] = session->ssrc & 0xFF;
    
    // === JPEG/RTP Header (8 bytes, RFC 2435 Section 3.1) ===
    rtpPacket[12] = 0;  // Type-Specific (0)
    
    // Fragment Offset (3 bytes, big-endian) - byte offset in scan data
    rtpPacket[13] = (fragmentOffset >> 16) & 0xFF;
    rtpPacket[14] = (fragmentOffset >> 8) & 0xFF;
    rtpPacket[15] = fragmentOffset & 0xFF;
    
    rtpPacket[16] = jpegType;  // Type (0 = YUV 4:2:0, 1 = YUV 4:2:2)
    rtpPacket[17] = jpegQ;     // Q (255 = dynamic tables, 0-127 = fixed)
    rtpPacket[18] = width / 8;  // Width in 8-pixel blocks
    rtpPacket[19] = height / 8; // Height in 8-pixel blocks
    
    // Copy scan data chunk
    memcpy(rtpPacket + 20, scanData + offset, chunkSize);
    
    // Send as TCP interleaved frame: $<channel><length><rtp_packet>
    size_t rtpPacketLen = 20 + chunkSize;
    uint8_t interleavedHeader[4];
    interleavedHeader[0] = '$';
    interleavedHeader[1] = session->interleavedRtpChannel;
    interleavedHeader[2] = (rtpPacketLen >> 8) & 0xFF;
    interleavedHeader[3] = rtpPacketLen & 0xFF;
    
    // Check write() return value to detect broken connections
    size_t headerWritten = session->client.write(interleavedHeader, 4);
    size_t payloadWritten = session->client.write(rtpPacket, rtpPacketLen);
    
    if (headerWritten != 4 || payloadWritten != rtpPacketLen) {
      logEvent(LOG_WARN, "⚠️ TCP write failed: header=" + String(headerWritten) + "/4, payload=" + String(payloadWritten) + "/" + String(rtpPacketLen));
      return;  // Stop sending this frame
    }
    
    session->sequenceNumber++;
    offset += chunkSize;
    fragmentOffset += chunkSize;
  }
  
  // Timestamp increment handled by scheduler to match actual frame cadence.
}

// Send RTP JPEG packet over UDP (legacy mode)
// Implements RFC 2435 - RTP Payload Format for JPEG-compressed Video
static void sendRtpJpegUdp(RtspSession* session, camera_fb_t* fb) {
  if (!session || !fb || !session->isPlaying) {
    return;
  }

  // Initialize UDP socket if not already
  if (!session->rtpSocketInitialized) {
    session->rtpSocket.begin(0); // Random local port
    session->rtpSocketInitialized = true;
  }

  // Parse JPEG to extract scan data
  uint8_t jpegType = 0;
  uint8_t jpegQ = 0;  // Will be set by parser
  size_t scanDataOffset = findJpegScanData(fb->buf, fb->len, jpegType, jpegQ);
  
  // Force Q=80 to avoid needing Q-table header (required for Q >= 128)
  jpegQ = 80;
  
  if (scanDataOffset == 0 || scanDataOffset >= fb->len) {
    return;
  }
  
  // Calculate scan data length (exclude EOI)
  size_t scanDataLen = fb->len - scanDataOffset;
  if (scanDataLen >= 2 && fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
    scanDataLen -= 2;
  }
  
  const uint8_t* scanData = fb->buf + scanDataOffset;
  uint16_t width = fb->width;
  uint16_t height = fb->height;
  
  const size_t JPEG_HEADER_SIZE = 8;
  const size_t MAX_RTP_PAYLOAD = 1200 - JPEG_HEADER_SIZE;  // Balance: larger = fewer packets, smaller = less buffering
  
  size_t offset = 0;
  uint32_t fragmentOffset = 0;
  int packetCount = 0;
  
  while (offset < scanDataLen) {
    size_t chunkSize = min((size_t)MAX_RTP_PAYLOAD, scanDataLen - offset);
    bool isLastFragment = (offset + chunkSize >= scanDataLen);
    
    // Build RTP header (12 bytes) + JPEG header (8 bytes) + scan data
    uint8_t rtpPacket[12 + JPEG_HEADER_SIZE + MAX_RTP_PAYLOAD];
    
    // === RTP Header ===
    rtpPacket[0] = 0x80;
    rtpPacket[1] = isLastFragment ? 0x9A : 0x1A;
    
    // Sequence number
    rtpPacket[2] = (session->sequenceNumber >> 8) & 0xFF;
    rtpPacket[3] = session->sequenceNumber & 0xFF;
    
    // Timestamp
    rtpPacket[4] = (session->timestamp >> 24) & 0xFF;
    rtpPacket[5] = (session->timestamp >> 16) & 0xFF;
    rtpPacket[6] = (session->timestamp >> 8) & 0xFF;
    rtpPacket[7] = session->timestamp & 0xFF;
    
    // SSRC
    rtpPacket[8] = (session->ssrc >> 24) & 0xFF;
    rtpPacket[9] = (session->ssrc >> 16) & 0xFF;
    rtpPacket[10] = (session->ssrc >> 8) & 0xFF;
    rtpPacket[11] = session->ssrc & 0xFF;
    
    // === JPEG/RTP Header ===
    rtpPacket[12] = 0;
    rtpPacket[13] = (fragmentOffset >> 16) & 0xFF;
    rtpPacket[14] = (fragmentOffset >> 8) & 0xFF;
    rtpPacket[15] = fragmentOffset & 0xFF;
    rtpPacket[16] = jpegType;
    rtpPacket[17] = jpegQ;
    rtpPacket[18] = width / 8;
    rtpPacket[19] = height / 8;
    
    // Copy scan data
    memcpy(rtpPacket + 20, scanData + offset, chunkSize);
    
    // Send via UDP with error checking
    size_t packetLen = 20 + chunkSize;
    if (!session->rtpSocket.beginPacket(session->clientIP, session->rtpPort)) {
      logEvent(LOG_WARN, "⚠️ RTSP UDP beginPacket failed");
      return;
    }
    
    size_t written = session->rtpSocket.write(rtpPacket, packetLen);
    if (written != packetLen) {
      logEvent(LOG_WARN, "⚠️ RTSP UDP write incomplete: " + String(written) + "/" + String(packetLen));
      session->rtpSocket.endPacket();  // Clean up failed packet
      return;
    }
    
    int result = session->rtpSocket.endPacket();
    if (result == 0) {
      logEvent(LOG_WARN, "⚠️ RTSP UDP endPacket failed");
      return;
    }
    
    session->sequenceNumber++;
    offset += chunkSize;
    fragmentOffset += chunkSize;
    packetCount++;
    
    // Delay between packets to prevent UDP buffer overflow
    // Must pace transmission for WiFi UDP stack
    if (offset < scanDataLen) {
      delayMicroseconds(1500);  // 1.5ms between packets
    }
  }
  
  // Timestamp increment handled by scheduler to match actual frame cadence.
}

// Send RTP JPEG - dispatches to TCP or UDP based on session mode
static void sendRtpJpeg(RtspSession* session, camera_fb_t* fb) {
  if (session->useTcpInterleaved) {
    sendRtpJpegTcp(session, fb);
  } else {
    sendRtpJpegUdp(session, fb);
  }
}

// Mic capture runs at AUDIO_SAMPLE_RATE; RTSP advertises 8kHz PCMU.
// Downsample to 8kHz and packetize 20ms frames (160 samples).
static const uint32_t kAudioRtpSampleRate = 8000;
static const size_t kAudioSamplesPerPacket = 160;  // 20ms at 8kHz
static const size_t kMicSamplesPerPacket = (AUDIO_SAMPLE_RATE / kAudioRtpSampleRate) * kAudioSamplesPerPacket;

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

static void encodeUlaw(const int16_t *pcm, size_t samples, uint8_t *ulaw) {
  for (size_t i = 0; i < samples; i++) {
    ulaw[i] = linear2ulaw(pcm[i]);
  }
}

static bool sendInterleaved(WiFiClient &client, uint8_t channel, const uint8_t *payload, size_t payloadLen) {
  uint8_t interleavedHeader[4];
  interleavedHeader[0] = '$';
  interleavedHeader[1] = channel;
  interleavedHeader[2] = (payloadLen >> 8) & 0xFF;
  interleavedHeader[3] = payloadLen & 0xFF;

  size_t headerWritten = client.write(interleavedHeader, 4);
  size_t payloadWritten = client.write(payload, payloadLen);
  return headerWritten == 4 && payloadWritten == payloadLen;
}

static void downsampleTo8k(const int16_t *in, size_t inSamples, int16_t *out, size_t outSamples) {
  if (AUDIO_SAMPLE_RATE == kAudioRtpSampleRate) {
    size_t samplesToCopy = min(inSamples, outSamples);
    memcpy(out, in, samplesToCopy * sizeof(int16_t));
    if (samplesToCopy < outSamples) {
      memset(out + samplesToCopy, 0, (outSamples - samplesToCopy) * sizeof(int16_t));
    }
    return;
  }
  size_t step = AUDIO_SAMPLE_RATE / kAudioRtpSampleRate;
  if (step < 1) {
    step = 1;
  }
  for (size_t i = 0; i < outSamples; i++) {
    size_t idx = i * step;
    if (idx < inSamples) {
      out[i] = in[idx];
    } else {
      out[i] = 0;
    }
  }
}

static void sendRtpAudio(RtspSession* session) {
  if (!session || !session->isPlaying || !session->audioSetup) {
    return;
  }
  if (!isMicEnabled()) {
    return;
  }

  int16_t micBuffer[kMicSamplesPerPacket];
  int16_t audioBuffer[kAudioSamplesPerPacket];
  uint8_t ulawBuffer[kAudioSamplesPerPacket];

  bool hasMic = !isMicMuted() && captureMicSamples(micBuffer, kMicSamplesPerPacket, 10);
  if (hasMic) {
    downsampleTo8k(micBuffer, kMicSamplesPerPacket, audioBuffer, kAudioSamplesPerPacket);
    encodeUlaw(audioBuffer, kAudioSamplesPerPacket, ulawBuffer);
  } else {
    // G.711 mu-law "silence" is 0xFF; use it when muted or capture fails.
    memset(ulawBuffer, 0xFF, sizeof(ulawBuffer));
  }

  uint8_t rtpPacket[12 + kAudioSamplesPerPacket];
  rtpPacket[0] = 0x80;
  rtpPacket[1] = 0x00;  // PT=0 (PCMU)
  rtpPacket[2] = (session->audioSequenceNumber >> 8) & 0xFF;
  rtpPacket[3] = session->audioSequenceNumber & 0xFF;
  rtpPacket[4] = (session->audioTimestamp >> 24) & 0xFF;
  rtpPacket[5] = (session->audioTimestamp >> 16) & 0xFF;
  rtpPacket[6] = (session->audioTimestamp >> 8) & 0xFF;
  rtpPacket[7] = session->audioTimestamp & 0xFF;
  rtpPacket[8] = (session->audioSsrc >> 24) & 0xFF;
  rtpPacket[9] = (session->audioSsrc >> 16) & 0xFF;
  rtpPacket[10] = (session->audioSsrc >> 8) & 0xFF;
  rtpPacket[11] = session->audioSsrc & 0xFF;
  memcpy(rtpPacket + 12, ulawBuffer, kAudioSamplesPerPacket);

  size_t packetLen = 12 + kAudioSamplesPerPacket;
  if (session->audioUseTcpInterleaved) {
    if (!sendInterleaved(session->client, session->interleavedAudioRtpChannel, rtpPacket, packetLen)) {
      logEvent(LOG_WARN, "⚠️ RTSP audio TCP write failed");
      return;
    }
  } else {
    if (session->audioRtpPort == 0) {
      return;
    }
    if (!session->audioSocketInitialized) {
      session->audioRtpSocket.begin(0);
      session->audioSocketInitialized = true;
    }
    if (!session->audioRtpSocket.beginPacket(session->clientIP, session->audioRtpPort)) {
      logEvent(LOG_WARN, "⚠️ RTSP audio UDP beginPacket failed");
      return;
    }
    size_t written = session->audioRtpSocket.write(rtpPacket, packetLen);
    if (written != packetLen) {
      logEvent(LOG_WARN, "⚠️ RTSP audio UDP write incomplete: " + String(written) + "/" + String(packetLen));
      session->audioRtpSocket.endPacket();
      return;
    }
    int result = session->audioRtpSocket.endPacket();
    if (result == 0) {
      logEvent(LOG_WARN, "⚠️ RTSP audio UDP endPacket failed");
      return;
    }
  }

  session->audioSequenceNumber++;
  session->audioTimestamp += kAudioSamplesPerPacket;
}

bool startRtspServer() {
  if (rtspRunning) {
    logEvent(LOG_WARN, "⚠️ RTSP server already running");
    return true;
  }

  logEvent(LOG_INFO, "🎥 Starting RTSP server...");
  
  // Create and start RTSP server (must be done after WiFi is connected)
  if (!rtspServer) {
    rtspServer = new WiFiServer(RTSP_PORT);
    if (!rtspServer) {
      logEvent(LOG_ERROR, "❌ Failed to allocate RTSP server");
      return false;
    }
  }
  
  rtspServer->begin();
  rtspRunning = true;
  if (!rtspTaskRunning) {
    if (xTaskCreatePinnedToCore(
          rtspTask,
          "rtsp_task",
          8192,
          NULL,
          1,
          &rtspTaskHandle,
          STREAM_TASK_CORE) != pdPASS) {
      logEvent(LOG_ERROR, "❌ Failed to create RTSP task");
      rtspTaskHandle = nullptr;
      rtspTaskRunning = false;
    } else {
      rtspTaskRunning = true;
    }
  }
  
  String rtspUrl = getRtspUrl();
  logEvent(LOG_INFO, "✅ RTSP server started");
  logEvent(LOG_INFO, "📡 RTSP URL: " + rtspUrl);
  
  return true;
}

void stopRtspServer() {
  if (!rtspRunning) {
    return;
  }
  
  // Close all active sessions
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (activeSessions[i]) {
      activeSessions[i]->rtpSocket.stop();
      activeSessions[i]->rtcpSocket.stop();
      activeSessions[i]->audioRtpSocket.stop();
      activeSessions[i]->audioRtcpSocket.stop();
      activeSessions[i]->client.stop();
      delete activeSessions[i];
      activeSessions[i] = nullptr;
    }
  }
  
  if (rtspServer) {
    rtspServer->stop();
    delete rtspServer;
    rtspServer = nullptr;
  }
  
  rtspRunning = false;
  if (rtspTaskHandle) {
    TaskHandle_t handle = rtspTaskHandle;
    rtspTaskHandle = nullptr;
    rtspTaskRunning = false;
    vTaskDelete(handle);
  }
  Serial.println("🛑 RTSP server stopped");
}

bool isRtspServerRunning() {
  return rtspRunning;
}

bool isRtspTaskRunning() {
  return rtspTaskRunning;
}

String getRtspUrl() {
  IPAddress ip = WiFi.localIP();
  return "rtsp://" + ip.toString() + ":" + String(RTSP_PORT) + "/mjpeg/1";
}

int getRtspActiveSessionCount() {
  int count = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (activeSessions[i] && activeSessions[i]->isPlaying) {
      count++;
    }
  }
  return count;
}

void setRtspAllowUdp(bool allow) {
  rtspAllowUdp = allow;
}

// Call this in loop() to handle RTSP client connections  
void handleRtspClient() {
  if (!rtspRunning || !rtspServer) {
    return;
  }

  // Check for new RTSP control connections
  WiFiClient newClient = rtspServer->available();
  if (newClient) {
    logEvent(LOG_INFO, "🔌 RTSP client connected from " + newClient.remoteIP().toString());
    
    // Handle this client's requests in a blocking manner until PLAY or disconnect
    // RTSP protocol requires persistent TCP connection for handshake
    bool sessionStarted = false;
    int setupSlot = -1;
    while (newClient.connected() && !sessionStarted) {
      // Read one RTSP request
      unsigned long timeout = millis() + 10000;
      String request = "";
      while (newClient.connected() && millis() < timeout) {
        if (newClient.available()) {
          char c = newClient.read();
          request += c;
          
          // End of RTSP request (double CRLF)
          if (request.endsWith("\r\n\r\n")) {
            break;
          }
        }
        yield(); // Prevent watchdog
      }
      
      if (request.length() == 0) {
        // No data received, client likely disconnected
        break;
      }
      
      String method = getRequestMethod(request);
      int cseq = getCSeq(request);
      
      logEvent(LOG_DEBUG, "RTSP " + method + " (CSeq=" + String(cseq) + ")");
      
      if (method == "OPTIONS") {
        String headers = "Public: DESCRIBE, SETUP, PLAY, TEARDOWN\r\n";
        sendRtspResponse(newClient, cseq, "200 OK", headers);
      } else if (method == "DESCRIBE") {
        handleDescribe(newClient, cseq);
      } else if (method == "SETUP") {
        String transport = getTransport(request);
        handleSetup(newClient, cseq, request, transport);
        setupSlot = findSessionSlotByClient(newClient);
        // Continue reading for PLAY command on same connection
      } else if (method == "PLAY") {
        handlePlay(newClient, cseq, request);
        sessionStarted = true; // Exit blocking loop, start sending frames
      } else if (method == "TEARDOWN") {
        // Find session by client
        int sessionSlot = findSessionSlotByClient(newClient);
        handleTeardown(newClient, cseq, request, sessionSlot);
        return; // Session torn down, exit
      } else {
        sendRtspResponse(newClient, cseq, "501 Not Implemented");
        newClient.stop();
        if (setupSlot >= 0 && activeSessions[setupSlot]) {
          delete activeSessions[setupSlot];
          activeSessions[setupSlot] = nullptr;
        }
        return;
      }
    }
    
    // If no session started, client disconnected during handshake
    if (!sessionStarted) {
      if (setupSlot >= 0 && activeSessions[setupSlot]) {
        logEvent(LOG_INFO, "📴 RTSP client disconnected during SETUP; cleaning session");
        delete activeSessions[setupSlot];
        activeSessions[setupSlot] = nullptr;
      } else {
        logEvent(LOG_INFO, "📴 RTSP client disconnected (no session created)");
      }
      newClient.stop();
    }
  }
  
  // Handle TEARDOWN requests on active sessions (non-blocking check)
  for (int i = 0; i < MAX_SESSIONS; i++) {
    RtspSession* session = activeSessions[i];
    if (session && session->isPlaying && session->client.connected()) {
      // Check for incoming TEARDOWN without blocking
      if (session->client.available()) {
        String request = "";
        unsigned long readStart = millis();
        while (session->client.available() && millis() - readStart < 100) {
          char c = session->client.read();
          request += c;
          if (request.endsWith("\r\n\r\n")) {
            break;
          }
        }
        
        if (request.indexOf("TEARDOWN") >= 0) {
          int cseq = getCSeq(request);
          session->lastActivityMs = millis();  // Update activity before teardown
          handleTeardown(session->client, cseq, request, i);
          logEvent(LOG_DEBUG, "💾 Free heap after TEARDOWN: " + String(ESP.getFreeHeap()) + " bytes");
          return;
        } else if (request.length() > 0) {
          // Any other request = activity
          session->lastActivityMs = millis();
        }
      }
    }
  }

  // Stream frames to active playing sessions
  unsigned long now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    RtspSession* session = activeSessions[i];
    if (session && session->isPlaying) {
      // Check if RTSP control connection is still alive
      bool clientDisconnected = !session->client.connected();
      
      // Check for session timeout (60 seconds of inactivity)
      const unsigned long SESSION_TIMEOUT = 60000;  // 60 seconds as per Session: timeout=60
      if (now - session->lastActivityMs > SESSION_TIMEOUT) {
        logEvent(LOG_INFO, "⏱️ RTSP session timeout: Session " + String(session->sessionId, HEX));
        clientDisconnected = true;
      }
      
      if (clientDisconnected) {
        logEvent(LOG_INFO, "📴 RTSP client disconnected: Session " + String(session->sessionId, HEX));
        session->rtpSocket.stop();
        session->rtcpSocket.stop();
        session->audioRtpSocket.stop();
        session->audioRtcpSocket.stop();
        session->client.stop();
        delete session;
        activeSessions[i] = nullptr;
        
        // Log free heap to track memory usage
        logEvent(LOG_DEBUG, "💾 Free heap: " + String(ESP.getFreeHeap()) + " bytes");
        continue;
      }
      
      // Send frame at 15 fps to match Scrypted settings and reduce UDP pressure
      if (now - session->lastFrameMs >= 67) { // 15 fps (1000ms / 15 = 67ms)
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
          lastFrameWidth = fb->width;
          lastFrameHeight = fb->height;
          sendRtpJpeg(session, fb);
          esp_camera_fb_return(fb);
          if (session->lastFrameMs > 0) {
            unsigned long deltaMs = now - session->lastFrameMs;
            uint32_t increment = deltaMs * 90;
            if (increment == 0) {
              increment = 1;
            }
            session->timestamp += increment;
          }
          session->lastFrameMs = now;
          session->lastActivityMs = now;  // Update activity timestamp on successful frame send
        } else {
          // Camera frame acquisition failed - log it
          logEvent(LOG_ERROR, "❌ Camera frame acquisition failed");
        }
      }

      // Send audio at 20ms cadence when enabled.
      const unsigned long AUDIO_INTERVAL_MS = 20;
      if (session->audioSetup && isMicEnabled()) {
        if (now - session->lastAudioMs >= AUDIO_INTERVAL_MS) {
          sendRtpAudio(session);
          session->lastAudioMs = now;
          session->lastActivityMs = now;
        }
      }
    }
  }
}

static void rtspTask(void *pvParameters) {
  while (rtspRunning) {
    handleRtspClient();
    vTaskDelay(1);
  }
  rtspTaskRunning = false;
  rtspTaskHandle = nullptr;
  vTaskDelete(NULL);
}

#else

// Dummy implementations when CAMERA is not defined
bool startRtspServer() { return false; }
void stopRtspServer() {}
bool isRtspServerRunning() { return false; }
bool isRtspTaskRunning() { return false; }
String getRtspUrl() { return ""; }
void setRtspAllowUdp(bool allow) { (void)allow; }
void handleRtspClient() {}

#endif // CAMERA
