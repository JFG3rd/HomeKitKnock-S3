<!--
 Project: HomeKitKnock-S3
 File: docs/README.md
 Purpose: Documentation index and navigation guide
 Last Updated: February 26, 2026
 -->

# ESP32-S3 Doorbell Documentation

DIY HomeKit doorbell using Seeed XIAO ESP32-S3 Sense → Scrypted → Apple HomeKit.
Pure ESP-IDF 5.5.0 firmware (no Arduino), Phases 0–5 complete.

## 🚀 Start Here

1. **[CURRENT_STATUS.md](CURRENT_STATUS.md)** — Current status, what's working, next steps
2. **[esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md)** — Full system architecture
3. **[WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)** — Physical wiring and GPIO assignments

## 📊 Implementation Status

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | ✅ Complete | Pre-migration hygiene |
| Phase 1 | ✅ Complete | IDF base (boot, NVS, WiFi, web) |
| Phase 2 | ✅ Complete | Captive portal, log viewer, config |
| Phase 3 | ✅ Complete | SIP intercom, button, LED, SNTP |
| Phase 4 | ✅ Complete | Video — camera, MJPEG, RTSP |
| Phase 5 | ✅ Complete | Audio — speaker + INMP441 mic, Record & Play |
| Phase 6 | ❌ Pending | HomeKit doorbell integration |
| Phase 7 | ❌ Pending | Full OTA update system |
| Phase 8 | ❌ Pending | Cleanup & resilience |

## 📚 Documentation by Topic

### 🔧 Architecture & Design
- **[esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md)** — Full system architecture and design notes
- **[IDF_ADF_MIGRATION_PLAN.md](IDF_ADF_MIGRATION_PLAN.md)** — Phased migration plan with session summaries
- **[IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)** — What's working, component structure, API reference
- **[GPIO_MAP.md](GPIO_MAP.md)** — Pin assignments and availability
- **[WIRING_DIAGRAM.md](WIRING_DIAGRAM.md)** — Physical wiring map

### 🔊 Audio
- **[AUDIO_INTEGRATION.md](AUDIO_INTEGRATION.md)** — Mic + speaker path, formats, tuning
- Audio uses native ESP-IDF I2S drivers (`driver/i2s_std.h`, `driver/i2s_pdm.h`)
- `aac_encoder_pipe` component (future RTSP AAC) will use ESP-ADF
- INMP441 external mic on GPIO5 (D4) via shared I2S1 bus

### 📹 Video Streaming
- **[SCRYPTED_RTSP_SETUP.md](SCRYPTED_RTSP_SETUP.md)** — Scrypted camera integration
- MJPEG HTTP on port 81, RTSP MJPEG on port 8554

### ☎️ SIP Integration (FRITZ!Box)
- **[SIP_INTEGRATION.md](SIP_INTEGRATION.md)** — SIP configuration guide
- **[ESP32-Fritzbox-SIP-Documentation.md](ESP32-Fritzbox-SIP-Documentation.md)** — Technical SIP details

### 🌐 Web Interface
- **[UPDATING_WEB_INTERFACE.md](UPDATING_WEB_INTERFACE.md)** — How to modify the embedded web UI

### 🔄 OTA Updates
- **[OTA_UPDATE_FILE.md](OTA_UPDATE_FILE.md)** — OTA image creation and naming

### ⚡ Hardware
- **[PROJECT_BOM.md](PROJECT_BOM.md)** — Bill of materials
- **[POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md)** — Power supply options

### 🎨 Diagrams & References
- **[Mermaid SIP Sequence Diagram.md](Mermaid%20SIP%20Sequence%20Diagram.md)** — SIP message flow
- **[Mermaid Class Diagram — ESP32 SIP Client Architecture.md](Mermaid%20Class%20Diagram%20—%20ESP32%20SIP%20Client%20Architecture.md)**

## 🛠️ Quick Build & Flash

```bash
pio run -e seeed_xiao_esp32s3_idf -t upload
pio device monitor -e seeed_xiao_esp32s3_idf
```

## 🔗 External Resources

- [Seeed XIAO ESP32-S3 Sense Wiki](https://wiki.seeedstudio.com/xiao_esp32s3_sense/)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [PlatformIO Documentation](https://docs.platformio.org/)

---

**Project:** HomeKitKnock-S3
**Device:** Seeed XIAO ESP32-S3 Sense
**Framework:** Pure ESP-IDF 5.5.0 (PlatformIO)
**Status:** Phase 5 complete — audio I/O fully working
**Last Updated:** February 26, 2026
