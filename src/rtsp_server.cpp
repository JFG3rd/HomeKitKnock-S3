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
#include "rtsp_server.h"

#ifdef CAMERA

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_camera.h>
#include "logger.h"

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
#define RTP_PORT_BASE 5000

// RTSP session state
struct RtspSession {
  WiFiClient client;
  WiFiUDP rtpSocket;
  WiFiUDP rtcpSocket;
  uint16_t rtpPort;
  uint16_t rtcpPort;
  uint32_t sessionId;
  uint16_t sequenceNumber;
  uint32_t timestamp;
  uint32_t ssrc;
  IPAddress clientIP;
  bool isPlaying;
  bool rtpSocketInitialized;
  unsigned long lastFrameMs;
  
  // TCP interleaved mode (RTP over RTSP TCP connection)
  bool useTcpInterleaved;
  uint8_t interleavedRtpChannel;
  uint8_t interleavedRtcpChannel;
};

// RTSP server objects
static WiFiServer* rtspServer = nullptr;
static RtspSession* activeSessions[4] = {nullptr};
static const int MAX_SESSIONS = 4;
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
  sdp += "a=control:" + rtspUrl + "/track1\r\n";

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
static void handleSetup(WiFiClient &client, int cseq, String transport) {
  logEvent(LOG_DEBUG, "SETUP Transport: " + transport);
  
  // Check if client wants TCP interleaved mode (RTP/AVP/TCP with interleaved=)
  bool useTcp = transport.indexOf("RTP/AVP/TCP") >= 0;
  uint8_t rtpChannel = 0;
  uint8_t rtcpChannel = 1;
  uint16_t clientRtpPort = 0;
  uint16_t clientRtcpPort = 0;
  
  if (useTcp) {
    // TCP interleaved mode
    if (!parseInterleaved(transport, rtpChannel, rtcpChannel)) {
      // Default to channels 0-1 if not specified
      rtpChannel = 0;
      rtcpChannel = 1;
    }
  } else {
    // UDP mode
    if (!parseClientPorts(transport, clientRtpPort, clientRtcpPort)) {
      sendRtspResponse(client, cseq, "461 Unsupported Transport");
      return;
    }
  }

  int slot = findFreeSessionSlot();
  if (slot < 0) {
    sendRtspResponse(client, cseq, "453 Not Enough Bandwidth");
    return;
  }

  RtspSession* session = new RtspSession();
  session->client = client;
  session->rtpPort = clientRtpPort;
  session->rtcpPort = clientRtcpPort;
  session->sessionId = generateSessionId();
  session->sequenceNumber = 0;
  session->timestamp = 0;
  session->ssrc = generateSSRC();
  session->clientIP = client.remoteIP();
  session->isPlaying = false;
  session->rtpSocketInitialized = false;
  session->lastFrameMs = 0;
  session->useTcpInterleaved = useTcp;
  session->interleavedRtpChannel = rtpChannel;
  session->interleavedRtcpChannel = rtcpChannel;

  activeSessions[slot] = session;

  // Build Transport response
  String headers;
  if (useTcp) {
    headers = "Transport: RTP/AVP/TCP;unicast;interleaved=" + String(rtpChannel) + "-" + String(rtcpChannel) + "\r\n";
    logEvent(LOG_INFO, "🔌 RTSP SETUP: TCP interleaved mode, channels " + String(rtpChannel) + "-" + String(rtcpChannel));
  } else {
    headers = "Transport: RTP/AVP;unicast;client_port=" + String(clientRtpPort) + "-" + String(clientRtcpPort) + "\r\n";
    logEvent(LOG_INFO, "🔌 RTSP SETUP: UDP mode, ports " + String(clientRtpPort) + "-" + String(clientRtcpPort));
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
  const size_t MAX_RTP_PAYLOAD = 1400 - JPEG_HEADER_SIZE;
  
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
    
    session->client.write(interleavedHeader, 4);
    session->client.write(rtpPacket, rtpPacketLen);
    
    session->sequenceNumber++;
    offset += chunkSize;
    fragmentOffset += chunkSize;
  }
  
  // Increment timestamp for next frame (90kHz clock, ~33ms = 30fps)
  session->timestamp += 3000;
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
  const size_t MAX_RTP_PAYLOAD = 1400 - JPEG_HEADER_SIZE;
  
  size_t offset = 0;
  uint32_t fragmentOffset = 0;
  
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
    
    // Send via UDP
    session->rtpSocket.beginPacket(session->clientIP, session->rtpPort);
    session->rtpSocket.write(rtpPacket, 20 + chunkSize);
    session->rtpSocket.endPacket();
    
    session->sequenceNumber++;
    offset += chunkSize;
    fragmentOffset += chunkSize;
  }
  
  // Increment timestamp
  session->timestamp += 3000;
}

// Send RTP JPEG - dispatches to TCP or UDP based on session mode
static void sendRtpJpeg(RtspSession* session, camera_fb_t* fb) {
  if (session->useTcpInterleaved) {
    sendRtpJpegTcp(session, fb);
  } else {
    sendRtpJpegUdp(session, fb);
  }
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
  Serial.println("🛑 RTSP server stopped");
}

bool isRtspServerRunning() {
  return rtspRunning;
}

String getRtspUrl() {
  IPAddress ip = WiFi.localIP();
  return "rtsp://" + ip.toString() + ":" + String(RTSP_PORT) + "/mjpeg/1";
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
        handleSetup(newClient, cseq, transport);
        // Continue reading for PLAY command on same connection
      } else if (method == "PLAY") {
        handlePlay(newClient, cseq, request);
        sessionStarted = true; // Exit blocking loop, start sending frames
      } else if (method == "TEARDOWN") {
        // Find session by client
        int sessionSlot = -1;
        for (int i = 0; i < MAX_SESSIONS; i++) {
          if (activeSessions[i] && activeSessions[i]->client == newClient) {
            sessionSlot = i;
            break;
          }
        }
        handleTeardown(newClient, cseq, request, sessionSlot);
        return; // Session torn down, exit
      } else {
        sendRtspResponse(newClient, cseq, "501 Not Implemented");
        newClient.stop();
        return;
      }
    }
    
    // If no session started, client disconnected during handshake
    if (!sessionStarted) {
      logEvent(LOG_INFO, "📴 RTSP client disconnected (no session created)");
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
          handleTeardown(session->client, cseq, request, i);
          return;
        }
      }
    }
  }

  // Stream frames to active playing sessions
  for (int i = 0; i < MAX_SESSIONS; i++) {
    RtspSession* session = activeSessions[i];
    if (session && session->isPlaying) {
      // Check if client is still connected
      if (!session->client.connected()) {
        logEvent(LOG_INFO, "📴 RTSP client disconnected: Session " + String(session->sessionId, HEX));
        session->rtpSocket.stop();
        session->rtcpSocket.stop();
        delete session;
        activeSessions[i] = nullptr;
        continue;
      }
      
      // Send frame at reasonable rate (~15-20 fps)
      unsigned long now = millis();
      if (now - session->lastFrameMs >= 50) { // 20 fps max
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
          sendRtpJpeg(session, fb);
          esp_camera_fb_return(fb);
          session->lastFrameMs = now;
        }
      }
    }
  }
}

#else

// Dummy implementations when CAMERA is not defined
bool startRtspServer() { return false; }
void stopRtspServer() {}
bool isRtspServerRunning() { return false; }
String getRtspUrl() { return ""; }
void handleRtspClient() {}

#endif // CAMERA
