<!--
 Project: HomeKitKnock-S3
 File: docs/README.md
 Purpose: Documentation index and navigation guide
 -->

# ESP32-S3 Doorbell Documentation

Welcome to the complete documentation for the ESP32-S3 Doorbell project. This guide will help you understand the current status, implementation details, and known issues.

## 🚀 Start Here

### For First-Time Users
1. **[QUICK_START.md](QUICK_START.md)** — 30-second setup guide
2. **[esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md)** — System overview and architecture

### For Contributors/Troubleshooters
1. **[CURRENT_STATUS.md](CURRENT_STATUS.md)** ⚠️ **READ THIS FIRST** — Current blocker (NVS boot loop) and how to help
2. **[IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)** — Technical status and what's working/blocked

## 📚 Documentation by Topic

### 🔴 Current Blocker
- **[CURRENT_STATUS.md](CURRENT_STATUS.md)** — Detailed explanation of NVS corruption boot loop (error 4363)
  - What's tried
  - Root cause theories
  - How to help resolve

### 🔧 Architecture & Design
- **[esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md)** — Full system architecture
- **[GPIO_MAP.md](GPIO_MAP.md)** — Pin assignments and hardware connections
- **[WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)** — Physical wiring guide
- **[PROJECT_BOM.md](PROJECT_BOM.md)** — Bill of materials and component list

### 📹 Video Streaming
- **[SCRYPTED_RTSP_SETUP.md](SCRYPTED_RTSP_SETUP.md)** — Scrypted camera integration
- **[esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md#video-streaming)** — RTSP/MJPEG details

### 🎵 Audio Streaming
- **[AUDIO_INTEGRATION.md](AUDIO_INTEGRATION.md)** — Audio capture and streaming
- **[EMBEDDED_ASSETS.md](EMBEDDED_ASSETS.md)** — Related: Web UI assets (mentions audio files)

### ☎️ SIP Integration (FRITZ!Box)
- **[SIP_INTEGRATION.md](SIP_INTEGRATION.md)** — SIP configuration guide
- **[ESP32-Fritzbox-SIP-Documentation.md](ESP32-Fritzbox-SIP-Documentation.md)** — Technical SIP details
- **[SIP-Fritzbox JSON Spec.md](SIP-Fritzbox%20JSON%20Spec.md)** — API specification

### 🌐 Web Interface
- **[EMBEDDED_ASSETS.md](EMBEDDED_ASSETS.md)** — Web assets now embedded in firmware (eliminated LittleFS)
- **[UPDATING_WEB_INTERFACE.md](UPDATING_WEB_INTERFACE.md)** — How to modify web UI

### 🔄 Updates
- **[OTA_UPDATE_FILE.md](OTA_UPDATE_FILE.md)** — Over-the-air update mechanism

### 🔍 Debugging
- **[TR064_DEBUGGING.md](TR064_DEBUGGING.md)** — TR-064 protocol debugging tips

### 🎨 Diagrams & References
- **[Mermaid Class Diagram — ESP32 SIP Client Architecture.md](Mermaid%20Class%20Diagram%20—%20ESP32%20SIP%20Client%20Architecture.md)** — SIP client architecture
- **[Mermaid Flowchart — Digest Authentication Logic.md](Mermaid%20Flowchart%20—%20Digest%20Authentication%20Logic.md)** — SIP auth flow
- **[Mermaid SIP Sequence Diagram.md](Mermaid%20SIP%20Sequence%20Diagram.md)** — SIP message sequence
- **[Mermaid Timing Diagram — SIP Message Timing & Retransmissions.md](Mermaid%20Timing%20Diagram%20—%20SIP%20Message%20Timing%20&%20Retransmissions.md)** — Timing details

### ⚡ Hardware Design
- **[POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md)** — Power supply options and calculations

## 📊 Implementation Status

| Feature | Status | Notes |
|---------|--------|-------|
| **Video Streaming (RTSP)** | ✅ Implemented | Cannot test (NVS blocker) |
| **Audio Streaming (AAC)** | ✅ Implemented | AAC codec needs ESP-ADF integration |
| **SIP Integration** | ✅ Implemented | Cannot test (NVS blocker) |
| **Web Interface** | ✅ Implemented | Embedded in firmware (PROGMEM) |
| **WiFi AP Mode** | ❌ Blocked | NVS error 4363 prevents startup |
| **OTA Updates** | ✅ Implemented | Cannot test (no WiFi) |
| **Doorbell Button** | ✅ Implemented | Cannot test (no SIP) |

## 🔴 Known Issues

### Critical: NVS Boot Loop (Error 4363)
- **Impact:** Device cannot boot past WiFi initialization
- **Status:** Investigated, mitigation implemented (graceful feature disabling), root cause unclear
- **Resolution:** Needs ESP-IDF or WiFi driver expert to investigate
- **See:** [CURRENT_STATUS.md](CURRENT_STATUS.md)

### Medium: AAC Audio Codec
- **Impact:** Audio streams not properly encoded as AAC
- **Status:** Needs ESP-ADF integration for proper AAC-LC encoding
- **See:** [AUDIO_INTEGRATION.md](AUDIO_INTEGRATION.md)

## 🛠️ Recent Changes

### January 29, 2026: Embedded Web Assets
- ✅ Eliminated LittleFS filesystem dependency
- ✅ All HTML/CSS/JS now embedded as gzip-compressed PROGMEM
- ✅ Reduced boot time by ~500ms
- ✅ Reduced flash usage by 71% (110KB → 32KB)
- 📖 See: [EMBEDDED_ASSETS.md](EMBEDDED_ASSETS.md)

### January 29, 2026: NVS Graceful Degradation
- ✅ Added NVS accessibility check at startup
- ✅ Features automatically disabled if NVS not accessible
- ✅ Eliminates Preferences error spam
- ⚠️ Still doesn't resolve WiFi initialization failure

## 🤝 How to Help

### If You Know ESP-IDF/WiFi
1. Read [CURRENT_STATUS.md](CURRENT_STATUS.md) for full context
2. Investigate why WiFi driver fails with error 4363 after app's NVS operations
3. Suggest fixes for NVS initialization order or configuration

### If You Know Arduino/ESP32
1. Test reproduction case with minimal WiFi + Preferences sketch
2. Share known workarounds for NVS corruption on ESP32-S3
3. Verify partition table compatibility

### If You're a Community Member
1. Share if you've encountered error 4363 with ESP32-S3
2. Suggest alternate approaches for WiFi initialization
3. Point to related issues or solutions

## 📖 Document Relationships

```
QUICK_START.md
    ↓
esp32-s3-doorbell-architecture.md (full overview)
    ├── GPIO_MAP.md (hardware details)
    ├── WIRING_DIAGRAM.md (physical wiring)
    └── PROJECT_BOM.md (components)

CURRENT_STATUS.md (⚠️ BLOCKER - READ THIS)
    ├── IMPLEMENTATION_SUMMARY.md (technical details)
    ├── EMBEDDED_ASSETS.md (web UI in firmware)
    └── AUDIO_INTEGRATION.md (audio codec planning)

SCRYPTED_RTSP_SETUP.md (video streaming integration)
SIP_INTEGRATION.md (FRITZ!Box integration)
    └── ESP32-Fritzbox-SIP-Documentation.md (technical details)

[Various debugging & reference docs]
```

## 🔗 External Resources

### ESP32-S3 Documentation
- [Seeed XIAO ESP32-S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_sense/)
- [ESP32-S3 Technical Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)

### Libraries & Frameworks
- [PlatformIO Documentation](https://docs.platformio.org/)
- [Arduino-ESP32 GitHub](https://github.com/espressif/arduino-esp32)
- [ESP-ADF (Audio Development Framework)](https://github.com/espressif/esp-adf)

### Protocols
- [RTSP RFC 2326](https://tools.ietf.org/html/rfc2326)
- [SIP RFC 3261](https://tools.ietf.org/html/rfc3261)
- [TR-064 Protocol](https://avm.de/fileadmin/user_upload/documents/fritzbox/fritz-nas-03.89.pdf)

## 📝 License

See [LICENSE](../LICENSE) for details.

---

**Project:** ESP32-S3 Doorbell  
**Purpose:** HomeKit doorbell via Scrypted + FRITZ!Box IP phone  
**Status:** Phase 1 complete, NVS boot loop blocking Phase 2  
**Last Updated:** January 29, 2026
