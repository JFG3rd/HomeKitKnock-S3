<!--
 Project: HomeKitKnock-S3
 File: docs/IMPLEMENTATION_SUMMARY.md
 Author: Jesse Greene
 Last Updated: February 22, 2026
 -->

# ESP32-S3 Doorbell - Implementation Summary

## Project Status: ESP-IDF Migration Phase 5 In Progress 🔧

Phases 0-4 complete. Phase 5 (Audio Path) is in progress: speaker (MAX98357A) works end-to-end with gong playback, volume control, and test tone. INMP441 mic integration via shared I2S bus is the current work.

---

## What's Working

### Audio Output (Speaker) ✅ (Phase 5 partial)
- **MAX98357A I2S DAC**: I2S_NUM_1, GPIO7 (BCLK), GPIO8 (WS), GPIO9 (DOUT)
- **Shared I2S Bus**: `i2s_shared_bus` component manages full-duplex I2S_NUM_1 — BCLK/WS shared between speaker TX and INMP441 RX
- **Gong Playback**: Embedded PCM from flash (`gong_data.c`) with synthesized 880/660 Hz fallback
- **Deferred TX Channel**: Created at first playback (not at boot) to avoid GPIO conflicts
- **Volume Control**: Software scaling 0–100%, NVS persistence, slider always interactive
- **Test Tone**: 440 Hz 2s tone via setup page "🔔 Test Gong" button
- **Audio out enabled**: NVS default is 1 (enabled) — gong is a core feature
- **Unconditional Init**: `audio_output_init()` runs at boot, independent of camera enable flag

### Camera & MJPEG Streaming ✅ (Phase 4)
- **OV2640 Camera**: VGA JPEG, 2 frame buffers in PSRAM (8MB OPI)
- **MJPEG HTTP Stream**: Port 81, raw lwIP sockets, max 2 concurrent clients, TCP_NODELAY
- **JPEG Snapshot**: `/capture` endpoint returns single frame
- **RTSP Server**: Port 8554, MJPEG-over-RTP (`src_idf/components/rtsp_server/`)
- **Camera Config**: Framesize, quality, brightness, contrast — runtime apply + NVS persistence
- **Mic/Audio Config**: mic_enabled, mic_muted, sensitivity, AAC sample rate, bitrate — NVS persistence
- **Feature Toggle**: HTTP camera enable/disable via setup page, gated camera initialization
- **Combined Status**: `/status` endpoint returns system + camera + audio fields
- **Setup Page**: Camera Config card with sliders, Apply button, settings load on page open
- **Deferred Init**: Camera initializes after WiFi connects (if feature enabled)

### SIP Intercom ✅ (Phase 3)
- **SIP Registration**: Automatic registration with Fritz!Box
- **SIP INVITE**: Ring phones via `**11`, `**12`, etc. (Fritz!Box door intercom numbers)
- **Digest Authentication**: 401 challenge/response flow
- **Call Management**: CANCEL, ACK, BYE handling
- **Verbose Logging**: Single-line SIP packet display for debugging
- **NVS Persistence**: SIP credentials stored securely
- **Web Configuration**: SIP settings via `/sip.html`

### Physical I/O ✅ (Phase 3)
- **Doorbell Button**: GPIO4, active-low with internal pull-up, 50ms debounce
- **Status LED**: GPIO2, PWM-controlled with multiple patterns:
  - Ringing: breathing animation (6 seconds)
  - AP mode: fast double-blink
  - WiFi connecting: 2Hz blink
  - SIP error: slow pulse
  - SIP registered: steady low glow

### Time Synchronization ✅ (Phase 3)
- **SNTP**: Automatic time sync via pool.ntp.org
- **Timezone**: German CET/CEST with DST
- **Log Timestamps**: Real date/time when synced, uptime otherwise

### WiFi Provisioning ✅ (Phase 2)
- **Captive Portal**: Auto-popup on iOS/Android/Windows when connecting to AP
- **DNS Server**: Redirects all queries to 192.168.4.1
- **WiFi Scanning**: APSTA mode with cached, deduplicated results
- **Credential Storage**: NVS persistence across reboots
- **Auto-Restart**: Device reboots after saving credentials
- **Styled Restart Page**: Progress bar with auto-reconnect

### Web Interface ✅
- **Embedded Assets**: 12 gzip-compressed HTML/CSS files (~35KB)
- **REST API**: WiFi config, status, OTA, logs, SIP config, audio test
- **Log Viewer**: Filter by category, auto-refresh, download, real timestamps
- **Root Redirect**: AP mode → setup, STA mode → dashboard

### System Infrastructure ✅
- **NVS Manager**: Raw C API with auto-recovery
- **WiFi Manager**: STA/AP/APSTA modes with events
- **DNS Server**: Captive portal support
- **Log Buffer**: Ring buffer with ESP-IDF logging hook, Unix timestamps
- **Web Server**: esp_http_server with wildcard routing

---

## Architecture

### Component Structure
```
src_idf/
├── main/main.c                          # Entry point, boot sequence, main loop
├── components/
│   ├── nvs_manager/                     # NVS abstraction
│   ├── wifi_manager/                    # WiFi state machine
│   ├── web_server/                      # HTTP server + API
│   ├── dns_server/                      # Captive portal DNS
│   ├── log_buffer/                      # Web log viewer backend
│   ├── config_manager/                  # Settings storage
│   ├── sip_client/                      # SIP state machine + RTP
│   ├── button/                          # Doorbell button handler
│   ├── status_led/                      # PWM LED patterns
│   ├── camera/                          # OV2640 driver + NVS config (Phase 4)
│   ├── mjpeg_server/                    # MJPEG HTTP stream port 81 (Phase 4)
│   ├── rtsp_server/                     # RTSP MJPEG server port 8554 (Phase 4)
│   ├── audio_output/                    # MAX98357A speaker, gong, volume (Phase 5)
│   ├── audio_capture/                   # INMP441 mic capture via shared bus (Phase 5)
│   ├── i2s_shared_bus/                  # Full-duplex I2S_NUM_1 shared channel mgr (Phase 5)
│   └── aac_encoder_pipe/                # AAC-LC encoder for RTSP audio (Phase 5)
├── generated/embedded_web_assets.*      # Gzip assets (auto-generated)
```

### Boot Sequence
```
[1/5] NVS Manager Init (with corruption recovery)
[2/5] Log Buffer Init (hooks into ESP-IDF logging)
      Status LED Init (PWM for visual feedback)
      Button Init (GPIO4 with debounce callback)
      Audio Output Init (unconditional — speaker is core feature, TX channel deferred)
[3/5] WiFi Manager Init (esp_netif + events)
[4/5] WiFi Start (STA if credentials, else AP+DNS)
[5/5] Web Server + SIP Client (via WiFi event callback)
[6/6] Audio Capture Init (deferred to WiFi got-IP, if mic enabled — independent of camera)
[7/7] Camera Init + MJPEG/RTSP Server (deferred, after WiFi got-IP, if camera enabled)
```

### Main Loop
- Button polling (debounced, triggers SIP ring + gong)
- Status LED updates (pattern based on system state)
- SIP processing (registration, call handling, RTP media)
- SNTP initialization (when WiFi connected)
- Deferred audio capture init (when WiFi connected + mic enabled, regardless of camera)
- Deferred camera init (when WiFi connected + camera enabled)
- MJPEG/RTSP client count tracking for LED state

### NVS Namespaces
| Namespace | Keys | Purpose |
|-----------|------|---------|
| `wifi` | `ssid`, `password` | WiFi credentials |
| `sip` | `user`, `password`, `displayname`, `target`, `enabled`, `verbose` | SIP configuration |
| `camera` | `http_cam_en`, `rtsp_en`, `framesize`, `quality`, `brightness`, `contrast` | Camera feature toggle + sensor config |
| `camera` | `mic_en`, `mic_mute`, `mic_sens`, `aac_rate`, `aac_bitr`, `mic_source` | Mic/audio capture config |
| `camera` | `aud_out_en`, `aud_volume`, `hw_diag` | Speaker output + diagnostic mode |
| `system` | `timezone` | Timezone string (POSIX TZ format) |
| `ota` | `username`, `pass_hash` | OTA credentials (Phase 7) |

### API Endpoints
| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Redirect based on WiFi state |
| `/api/wifi` | POST | Save WiFi credentials |
| `/api/wifi` | DELETE | Clear WiFi credentials |
| `/api/status` | GET | System status JSON (detailed) |
| `/api/ota` | POST | Firmware update |
| `/api/logs` | GET | Get logs with filter |
| `/api/logs` | DELETE | Clear log buffer |
| `/api/features` | GET | Feature toggle states |
| `/api/sip` | GET | Get SIP config (without password) |
| `/api/sip` | POST | Save SIP config |
| `/api/sip/ring` | POST | Trigger SIP ring |
| `/api/sip/verbose` | GET/POST | Verbose logging toggle |
| `/api/audio/gong` | POST | Play test gong on speaker |
| `/api/mic/test` | POST | Record 2s + play back (mic test) |
| `/capture` | GET | JPEG snapshot (Phase 4) |
| `/cameraStreamInfo` | GET | Camera/streaming status (Phase 4) |
| `/control` | GET | Camera/mic settings `?var=X&val=Y` (Phase 4) |
| `/status` | GET | Combined system + camera + audio JSON (Phase 4) |
| `/saveFeatures` | POST | Save feature toggles |
| `/ring/sip` | GET | Legacy SIP ring trigger |
| `/saveWiFi` | POST | Legacy credential save |
| `/saveSIP` | POST | Legacy SIP config save |
| `/scanWifi` | GET | Start WiFi scan |
| `/wifiScanResults` | GET | Get scan results (deduplicated) |
| `/deviceStatus` | GET | Basic device status |
| `/sipDebug` | GET | SIP debug info |
| `/restart` | GET | Restart with progress UI |

### Web Pages
| Page | Purpose |
|------|---------|
| `index.html` | Main dashboard |
| `wifi-setup.html` | WiFi provisioning |
| `logs.html` | Log viewer with filters |
| `ota.html` | Firmware update |
| `setup.html` | Device setup — audio/mic config, camera config, feature toggles |
| `sip.html` | SIP configuration |
| `live.html` | Live A/V viewer |

---

## GPIO Assignments

| GPIO | Function | Notes |
|------|----------|-------|
| GPIO4 | Doorbell Button | Active-low, internal pull-up, 50ms debounce |
| GPIO2 | Status LED | PWM, 330Ω resistor to LED |
| GPIO1 | Door Opener Relay | Active-high (future) |
| GPIO3 | Gong Relay | Active-high (future) |
| GPIO5/6 | I2C | Reserved for sensors |
| GPIO7 | Shared I2S BCLK | MAX98357A BCLK + INMP441 SCK (shared clock line) |
| GPIO8 | Shared I2S WS | MAX98357A LRC + INMP441 WS (shared word select) |
| GPIO9 | I2S_NUM_1 TX | MAX98357A DIN (speaker data out) |
| GPIO12 | I2S_NUM_1 RX | INMP441 SD/DOUT (mic data in) |
| GPIO41/42 | PDM Mic (onboard) | Onboard hardware — NOT connected in required wiring; INMP441 is the active mic |

---

## Key Files

### ESP-IDF Components
| File | Purpose |
|------|---------|
| `src_idf/main/main.c` | Boot sequence, main loop |
| `src_idf/components/nvs_manager/*` | NVS operations |
| `src_idf/components/wifi_manager/*` | WiFi state machine |
| `src_idf/components/web_server/*` | HTTP server + handlers |
| `src_idf/components/dns_server/*` | Captive portal DNS |
| `src_idf/components/log_buffer/*` | Log ring buffer with timestamps |
| `src_idf/components/sip_client/*` | SIP client + RTP |
| `src_idf/components/button/*` | Doorbell button handler |
| `src_idf/components/status_led/*` | PWM LED patterns |
| `src_idf/components/camera/*` | OV2640 driver + NVS config (Phase 4) |
| `src_idf/components/mjpeg_server/*` | MJPEG HTTP streaming (Phase 4) |
| `src_idf/components/rtsp_server/*` | RTSP server port 8554 (Phase 4) |
| `src_idf/components/audio_output/*` | MAX98357A speaker output (Phase 5) |
| `src_idf/components/audio_capture/*` | INMP441 mic capture (Phase 5) |
| `src_idf/components/i2s_shared_bus/*` | Shared full-duplex I2S_NUM_1 bus (Phase 5) |
| `src_idf/components/aac_encoder_pipe/*` | AAC-LC encoder for RTSP audio (Phase 5) |

### Web Assets
| File | Purpose |
|------|---------|
| `data/index.html` | Main dashboard |
| `data/wifi-setup.html` | WiFi provisioning |
| `data/logs.html` | Log viewer |
| `data/sip.html` | SIP configuration |
| `data/setup.html` | Device setup (audio/mic, camera, features) |
| `data/style.css` | Unified styling |

---

## Migration Progress

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | ✅ Complete | Pre-migration hygiene |
| Phase 1 | ✅ Complete | IDF base (boot, NVS, WiFi, web) |
| Phase 2 | ✅ Complete | Config services, captive portal, logs |
| Phase 3 | ✅ Complete | SIP intercom, button, LED, SNTP |
| Phase 4 | ✅ Complete | Video path — camera, MJPEG, RTSP |
| Phase 5 | 🔧 In Progress | Audio path — speaker working; mic (INMP441 shared bus) in progress |
| Phase 6 | ❌ Pending | HomeKit doorbell |
| Phase 7 | ❌ Pending | OTA update system (credentials, time-limited window) |
| Phase 8 | ❌ Pending | Cleanup & resilience |

---

## Features Detail

### SIP Integration
- **Fritz!Box IP Phone**: Register as LAN/WLAN IP phone
- **Door Intercom Numbers**: `**11`, `**12`, etc. route to Call Groups
- **Authentication**: MD5 digest auth with nonce
- **Call Flow**: INVITE → 401 → Auth INVITE → 100 → 183 → 200 → ACK
- **Verbose Mode**: Full SIP packets on single line with `|` separators

### Physical Button
- Debounced with 50ms threshold
- Triggers status LED breathing animation
- Initiates SIP ring request (deferred to main loop)
- Plays gong sound on speaker (async, fire-and-forget)

### Status LED Patterns
Priority (highest to lowest):
1. **Ringing**: Breathing (fade in/out over 1.4s, 6s duration)
2. **AP Mode**: Double-blink every 1s
3. **WiFi Connecting**: 2Hz blink
4. **SIP Error**: Slow pulse (2s period)
5. **SIP OK**: Steady low glow (24/255 duty)
6. **RTSP Active**: Short tick every 2s

### Log Timestamps
- **SNTP Synced**: Full date/time (2026-02-06 15:30:45)
- **Pre-Sync**: Uptime (0:05:23)
- **Timezone**: German CET/CEST with automatic DST

### Audio Diagnostic Mode
- Toggle via "🧪 Hardware Diagnostic Mode" checkbox on setup page (NVS `hw_diag`)
- **OFF** (default): Audio components log at ERROR level only — quiet serial output
- **ON**: All audio TAGs (`audio_output`, `audio_capture`, `aac_encoder_pipe`, `i2s_shared_bus`, `sip`, `rtsp`) log at DEBUG — verbose stats every 200 reads/writes

---

## Useful Commands

### Build and Upload
```bash
pio run -t upload -e seeed_xiao_esp32s3_idf
```

### Monitor Serial
```bash
pio device monitor -e seeed_xiao_esp32s3_idf
```

### Erase NVS
```bash
~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbmodem21201 \
    erase_region 0x9000 0x5000
```

### View Logs via Web
```
http://<device-ip>/logs.html
http://<device-ip>/logs.html?filter=core
http://<device-ip>/logs.html?filter=doorbell
```

### Re-embed Web Assets
```bash
python3 tools/embed_web_assets.py data/ include/
```

---

## Hardware

**Platform:** Seeed XIAO ESP32-S3 Sense
**Flash:** 8 MB
**PSRAM:** 8 MB (OPI)
**Camera:** OV2640

**Memory Usage (ESP-IDF build, Phase 4):**
- RAM: 80,728 bytes used (24.6%)
- Flash: 1,043,865 bytes (26.5%)

---

## Next Steps

### Phase 5 Remaining: INMP441 Mic Integration
1. Wire `audio_capture.c` to use `i2s_shared_bus` (replace standalone I2S_NUM_1 channel creation)
2. Wire `audio_output.c` to use `i2s_shared_bus` TX channel (true full-duplex, no stop-capture during gong)
3. Move `audio_capture_init()` out of camera-gated block in `main.c`
4. Propagate `hardware_diag_mode` changes at runtime from `camera.c` to `audio_output.c`
5. `POST /api/mic/test` endpoint: 2s record → stats → playback → JSON
6. "🎤 Record & Play" button on setup page Audio/Mic card

### Phase 6: HomeKit Doorbell
7. Integrate Espressif HAP SDK or bridge; expose doorbell + camera to Home app

### Phase 7: OTA Update System
8. Credential management (username + SHA-256 hash in NVS)
9. Time-limited upload window with Basic auth

See [IDF_ADF_MIGRATION_PLAN.md](IDF_ADF_MIGRATION_PLAN.md) for full roadmap.
