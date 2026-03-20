<!--
 Project: HomeKitKnock-S3
 File: docs/IDF_ADF_MIGRATION_PLAN.md
 Purpose: Development history — phased migration from Arduino to pure ESP-IDF
 Author: Jesse Greene
 Last Updated: March 2026
-->

# ESP-IDF Migration — Development History

This document tracks the phased migration from Arduino to pure ESP-IDF. It serves as the development journal with session summaries, bug fixes, and architectural decisions.

For the current system overview, see [esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md).

---

## Phase Summary

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | Done | Pre-migration hygiene (flash config, partitions) |
| Phase 1 | Done | IDF base (boot, NVS, WiFi, embedded web server) |
| Phase 2 | Done | Captive portal, log viewer, config services |
| Phase 3 | Done | SIP intercom, doorbell button, LED, SNTP |
| Phase 4 | Done | Camera, MJPEG streaming, RTSP server |
| Phase 5 | Done | Audio: speaker + INMP441 mic, shared I2S bus, SIP two-way audio |
| Phase 6 | Done | Scrypted integration, RTSP AAC audio, camera overlays |
| Phase 7 | Done | OTA updates, device authentication, security |
| Phase 8 | Planned | Cleanup and resilience hardening |

---

## Current Architecture

### Component Structure (21 components)
```
src_idf/
├── main/main.c                          # Boot sequence, main loop
└── components/
    ├── nvs_manager/                     # NVS abstraction
    ├── wifi_manager/                    # WiFi STA/AP/APSTA modes
    ├── web_server/                      # HTTP server + REST API
    ├── dns_server/                      # Captive portal DNS
    ├── log_buffer/                      # Web log viewer backend
    ├── config_manager/                  # Typed settings storage
    ├── sip_client/                      # SIP state machine + RTP
    ├── button/                          # Doorbell button (GPIO4)
    ├── status_led/                      # PWM LED patterns (GPIO2)
    ├── relay_controller/                # Gong (GPIO3) + door opener (GPIO1)
    ├── camera/                          # OV2640 driver + NVS config
    ├── mjpeg_server/                    # MJPEG HTTP streaming port 81
    ├── rtsp_server/                     # RTSP server port 8554 (MJPEG + AAC)
    ├── audio_output/                    # MAX98357A speaker, gong, volume
    ├── audio_capture/                   # INMP441 mic capture via shared bus
    ├── i2s_shared_bus/                  # Full-duplex I2S_NUM_1 shared channel
    ├── aac_encoder_pipe/                # AAC-LC encoder for RTSP audio
    ├── timestamp_overlay/               # JPEG timestamp + camera name overlay
    ├── embedded_web_assets/             # Gzip web asset registry
    ├── adf_codec/                       # ESP-ADF codec wrapper
    └── adf_pipeline/                    # ESP-ADF pipeline wrapper
```

### Boot Sequence
```
1. NVS Manager Init (with auto-recovery)
2. Log Buffer + Status LED + Button + Audio Output Init
3. WiFi Manager Init (esp_netif + event loop)
4. WiFi Start (STA if credentials, else AP+DNS)
5. Web Server + SIP Client (via WiFi event callback)
6. Audio Capture Init (deferred, if mic enabled)
7. Camera + MJPEG/RTSP Server (deferred, if camera enabled)
```

### NVS Namespaces
| Namespace | Keys | Purpose |
|-----------|------|---------|
| `wifi` | `ssid`, `password` | WiFi credentials |
| `sip` | `user`, `password`, `displayname`, `target`, `enabled`, `verbose` | SIP configuration |
| `camera` | `http_cam_en`, `rtsp_en`, `rtsp_udp`, `framesize`, `quality`, `brightness`, `contrast`, `saturation`, `awb`, `hmirror`, `vflip` | Camera feature toggles + sensor config |
| `camera` | `mic_en`, `mic_mute`, `mic_sens`, `aac_rate`, `aac_bitr`, `mic_source` | Mic/audio capture config |
| `camera` | `aud_out_en`, `aud_muted`, `aud_volume`, `hw_diag` | Speaker output + diagnostic mode |
| `camera` | `ts_overlay`, `cam_name_ovl`, `cam_name` | Timestamp and camera name overlay settings |
| `relay` | `gong_ms`, `door_ms` | Relay pulse durations |
| `rtsp` | `user`, `pass`, `scr_low_lat`, `scr_low_buf`, `scr_source`, `scr_enc_q`, `scr_bitrate`, `scr_ff_verb` | RTSP credentials + Scrypted streaming prefs |
| `system` | `timezone` | Timezone string (POSIX TZ format) |
| `auth` | `username`, `pass_hash` | Device credentials (SHA-256 hash) |

### GPIO Assignments
| GPIO | Function | Configuration |
|------|----------|---------------|
| GPIO1 | Door Opener Relay | Active-high — triggered by DTMF "123" from FRITZ!fon |
| GPIO2 | Status LED | PWM (LEDC), 8-bit, 5kHz |
| GPIO3 | Gong Relay | Active-high — 150ms startup delay, configurable pulse |
| GPIO4 | Doorbell Button | Active-low, internal pull-up, 50ms debounce |
| GPIO5 | INMP441 SD (mic data in) | I2S_NUM_1 RX — GPIO12 forbidden (camera Y7) |
| GPIO7 | Shared I2S BCLK | MAX98357A + INMP441 (shared via i2s_shared_bus) |
| GPIO8 | Shared I2S WS | MAX98357A + INMP441 (shared via i2s_shared_bus) |
| GPIO9 | I2S_NUM_1 TX data | MAX98357A DIN (speaker data out) |
| GPIO41/42 | PDM Mic (onboard) | Integrated on XIAO Sense; INMP441 is active source |

### API Endpoints
| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Redirect (AP→setup, STA→index) |
| `/api/wifi` | POST/DELETE | Save/clear WiFi credentials |
| `/api/status` | GET | System status JSON |
| `/api/ota` | POST | Firmware update |
| `/api/logs` | GET/DELETE | Log viewer API |
| `/api/features` | GET | Feature toggle states |
| `/api/sip` | GET/POST | SIP configuration |
| `/api/sip/ring` | POST | Trigger SIP ring |
| `/api/sip/verbose` | GET/POST | Toggle verbose logging |
| `/api/audio/gong` | POST | Play test gong |
| `/api/mic/test` | POST | Record 2s + play back |
| `/api/auth/status` | GET | Auth configuration state |
| `/api/auth/setup` | POST | Create initial credentials |
| `/api/auth/change` | POST | Change credentials |
| `/capture` | GET | JPEG snapshot |
| `/status` | GET | Combined system + camera + audio status |
| `/control` | GET | Camera/mic settings `?var=X&val=Y` |
| `/saveFeatures` | POST | Save all feature toggles and settings |
| `/ring/sip` | GET | Trigger SIP ring |
| `/ring/homekit` | GET | Trigger HomeKit test gong |
| `/restart` | GET | Styled restart page |

### Memory Usage (March 2026)
| Resource | Used | Total | Percentage |
|----------|------|-------|------------|
| RAM | 156,832 bytes | 327,680 bytes | 47.9% |
| Flash | 1,270,725 bytes | 3,932,160 bytes | 32.3% |

---

## Phase Details

### Phase 0 — Pre-migration hygiene ✅
- Aligned flash config to real hardware (8 MB)
- Regenerated partitions
- Enabled required mbedTLS options
- Full flash erase

### Phase 1 — IDF base ✅
- Minimal IDF app: logging, PSRAM init, WiFi STA/AP
- Raw NVS API
- Embedded web assets via `esp_http_server`

### Phase 2 — Networking and config ✅
- WiFi credential persistence
- HTTP-based OTA firmware update
- WiFi scanning in APSTA mode
- Captive portal with DNS server
- Web log viewer (`/logs.html`)
- System status API

### Phase 3 — SIP intercom ✅
- SIP client ported to ESP-IDF (lwIP sockets)
- REGISTER with MD5 digest auth
- INVITE/CANCEL/ACK/BYE
- FRITZ!Box door intercom integration
- Physical button (GPIO4) triggers ring
- Status LED (GPIO2) with multiple patterns
- SNTP time sync

### Phase 4 — Video path ✅
- OV2640 camera with PSRAM buffers
- MJPEG HTTP streaming (port 81, max clients configurable)
- RTSP MJPEG server (port 8554)
- Camera config runtime apply + NVS persistence
- Feature toggles for HTTP and RTSP streaming

### Phase 5 — Audio path ✅

**Architecture decision**: Audio uses native ESP-IDF I2S drivers, not ESP-ADF. The `aac_encoder_pipe` uses ESP-ADF codec libraries for AAC encoding only.

#### Shared I2S Bus
GPIO7 (BCLK) and GPIO8 (WS) are physically shared between MAX98357A and INMP441. The `i2s_shared_bus` component creates a full-duplex I2S_NUM_1 channel pair (TX + RX) that both audio components share.

**Key insights discovered during implementation:**
- TX channel must be enabled first — it's the BCLK master; INMP441 can't clock without it
- ESP-IDF returns stereo-interleaved DMA data even in MONO mode; must de-interleave to get L channel
- INMP441 requires 32-bit slot width (64 BCLK per LRCLK frame)
- GPIO5 is the only safe pin for INMP441 SD — GPIO12 is camera Y7 data

#### Implemented
- MAX98357A speaker: gong PCM from flash, synthesized fallback tones, volume 0-100%
- INMP441 capture: shared bus, stereo DMA de-interleave, sensitivity scaling
- SIP bidirectional audio: G.711 PCMU/PCMA RTP — verified on hardware
- AAC-LC encoder pipeline: lazy init, 10ms timeouts
- Record & Play diagnostic tool
- Full-duplex (mic + speaker simultaneously)

### Phase 6 — Scrypted + HomeKit ✅
- RTSP AAC audio: MPEG4-GENERIC/16000/1 (RFC 3640), advertised in SDP
- RTSP server: TCP interleaved + optional UDP transport
- Scrypted FFmpeg Camera plugin integration tested
- FFmpeg Output Prefix documented for MJPEG→H.264 conversion
- Camera overlays: timestamp (top-right) + camera name (bottom-left), toggled in setup
- Font rendering: 39-glyph 8x12 bitmap font (0-9, :/-SPACE, A-Z)
- Settings persistence fix: NVS writes now work even when camera hardware is unavailable
- Latency optimization documented (bufsize/keyframe tuning)

### Phase 7 — OTA + Security ✅
- First-setup flow: create username + password on first boot
- SHA-256 password hashing stored in NVS
- HTTP Basic Auth on all protected endpoints
- Time-limited OTA upload window (5 minutes)
- OTA page with progress UI
- Factory reset via double long-press (5s + 5s)
- Confirmation modals for destructive actions in web UI

### Phase 8 — Cleanup and Resilience (Planned)
- Remove Arduino build artifacts and unused scripts
- Add watchdog tuning
- Auto-reconnect SIP on network loss
- Brownout handling

---

## Session Summaries

### February 6, 2026 — Phase 3 Completion
- SIP client fully ported to ESP-IDF
- FRITZ!Box integration working (register, invite, digest auth)
- Physical button + status LED + SNTP verified
- **Bug**: SIP config was cached at boot, not reloaded per ring
- **Bug**: Wrong target number (`**11` vs `**12`)
- **Hardware**: XIAO ESP32-S3 Sense + FRITZ!Box 6591 Cable

### February 8-10, 2026 — Phase 4 (Camera + Streaming)
- Camera component with OV2640, VGA JPEG, PSRAM buffers
- MJPEG server on port 81 (raw lwIP sockets)
- RTSP MJPEG server on port 8554
- Camera config card in setup page
- **Bug**: esp_camera not found — needed EXTRA_COMPONENT_DIRS
- **Bug**: PSRAM in wrong sdkconfig file
- **Bug**: Socket exhaustion — increased MAX_SOCKETS to 16
- **Memory**: RAM 24.6%, Flash 26.5%

### February 22, 2026 — Phase 4 Complete + Audio Fixes
- RTSP server implemented
- Audio output camera-gate bug fixed (speaker init moved to unconditional boot)
- NVS default for audio_out_enabled changed to 1
- Deferred TX channel to avoid I2S GPIO conflicts

### February 26, 2026 — Phase 5 (INMP441 Mic Working)
- **Critical GPIO12 conflict**: INMP441 SD was on GPIO12 (OV2640 Y7 data). Moved to GPIO5.
- BCLK not generated: TX channel must be enabled before RX capture
- Stereo DMA de-interleave: extract L channel from `[L, R, L, R, ...]` buffer
- Record & Play verified — clear audible playback
- **Memory**: RAM 28.5%, Flash 31.3%

### March 1, 2026 — Full Doorbell Workflow Verified
7 bugs fixed in one session:
1. Main loop 50ms→10ms (RTP timing)
2. SIP INFO DTMF handler (door opener "123" sequence)
3. Main task stack overflow (large buffers → static)
4. BYE/CANCEL Call-ID validation (stale retransmissions)
5. DMA circular-buffer replay (flush silence after call)
6. DTMF 3-digit sequence buffer with 500ms timeout
7. SIP Stack Spec corrections

**Verified workflow**: Button → gong + relay → FRITZ!fon rings → two-way audio → door opener → clean hangup

### March 8-10, 2026 — Phase 7 (OTA + Auth)
- Secure OTA with 5-minute time-limited window
- Device credential system (username + SHA-256 hash)
- First-setup flow on first boot
- AUTH_GUARD macro for protected endpoints
- Factory reset via double long-press

### March 19-20, 2026 — Phase 6 (Scrypted + Overlays)
- RTSP AAC audio: M-bit fix, RFC 3640 compliance
- Scrypted FFmpeg Camera integration tested
- Streaming latency fix documented (Scrypted-side `-bufsize`/`-g` settings)
- Timestamp overlay (top-right) with 8x12 bitmap font
- Camera name overlay (bottom-left) with A-Z glyph extension
- Settings persistence fix: removed `!camera_ready` gate on NVS writes in `camera_set_control()`
- Camera-not-ready status JSON now includes sensor settings from NVS
- Buffer sizes increased (cam_json 384→512, response 768→1024)

---

## Commands Reference

### Build and Upload
```bash
pio run -t upload -e seeed_xiao_esp32s3_idf
```

### Monitor Serial
```bash
pio device monitor -e seeed_xiao_esp32s3_idf
```

### Re-embed Web Assets
```bash
python3 tools/embed_web_assets.py data include
```

### Erase NVS
```bash
~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbmodem21201 \
    erase_region 0x9000 0x5000
```

### Full Flash Erase
```bash
pio run -t erase -e seeed_xiao_esp32s3_idf
```
