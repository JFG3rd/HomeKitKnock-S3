<!--
 Project: HomeKitKnock-S3
 File: docs/IDF_ADF_MIGRATION_PLAN.md
 Purpose: Phased plan to migrate from Arduino-as-component to pure ESP-IDF + ESP-ADF
 Author: Codex (with user direction)
 Last Updated: February 10, 2026
-->

# Migration Plan: Pure ESP-IDF + ESP-ADF

Goal: Retire the Arduino layer, keep ADF for audio, and deliver all current features
(SIP intercom, RTSP/HTTP streaming, embedded web UI, OTA, HomeKit doorbell).

---

## Phase 0 — Pre-migration hygiene (keep current stack) ✅ COMPLETE
- ✅ Align flash config to real hardware (8 MB) and regenerate partitions.
- ✅ Enable required mbedTLS options (PSK/ciphers) to keep TLS usable during transition.
- ✅ Full flash erase once the above are set.

## Phase 1 — Stand up IDF base (no Arduino) ✅ COMPLETE
- ✅ Create a minimal IDF app: logging, PSRAM init, Wi‑Fi STA/AP bring-up, raw NVS API.
- ✅ Reuse embedded web assets via `esp_http_server`.
- ✅ Success criteria: boots reliably, serves `/` from embedded assets, no NVS errors.

## Phase 2 — Networking & config services ✅ COMPLETE

### Implemented Features ✅
- ✅ Config storage with raw NVS namespaces (`wifi`, `config`)
- ✅ WiFi credential persistence across reboots
- ✅ HTTP-based OTA firmware update (`/api/ota`)
- ✅ WiFi scanning in AP mode (APSTA mode with scan caching)
- ✅ SSID deduplication (filters duplicates, keeps strongest signal)
- ✅ Credential clearing and device restart endpoints
- ✅ Root redirect logic (AP mode → wifi-setup, STA mode → index)
- ✅ System status API (`/api/status`) with uptime, heap, RSSI
- ✅ Captive portal with DNS server (auto-popup on mobile devices)
- ✅ Auto-restart after saving WiFi credentials
- ✅ Styled restart page with auto-reconnect
- ✅ Web-based log viewer (`/logs.html`) with filtering
- ✅ Browser autofill protection on WiFi setup form

## Phase 3 — SIP intercom ✅ COMPLETE

### Implemented Features ✅
- ✅ SIP client ported to ESP-IDF component (lwIP sockets)
- ✅ SIP REGISTER with digest authentication (MD5)
- ✅ SIP INVITE/CANCEL/ACK/BYE for call management
- ✅ Fritz!Box door intercom integration (`**11`, `**12`, etc.)
- ✅ Physical doorbell button (GPIO4, active-low, debounced)
- ✅ Status LED (GPIO2, PWM with multiple patterns)
- ✅ SNTP time synchronization (German timezone)
- ✅ Real timestamps in web log viewer
- ✅ Single-line verbose SIP logging
- ✅ NVS persistence for SIP credentials
- ✅ Web UI for SIP configuration (`/sip.html`)
- ✅ Test ring button in web interface

### Success Criteria (All Met)
| Criterion | Status | Notes |
|-----------|--------|-------|
| SIP registration to Fritz!Box | ✅ | Digest auth working |
| Ring phones on button press | ✅ | GPIO4 triggers SIP INVITE |
| Visual feedback | ✅ | LED breathing during ring |
| Web configuration | ✅ | SIP settings via `/sip.html` |
| Survives reboots | ✅ | Config in NVS |

---

## Phase 4 — Video path 🔧 IN PROGRESS

### Implemented Features ✅
- ✅ esp32-camera component (`src_idf/components/camera/`) with OV2640, VGA JPEG, 2 PSRAM buffers
- ✅ MJPEG HTTP streaming (`src_idf/components/mjpeg_server/`) on port 81, raw lwIP sockets, max 2 clients
- ✅ JPEG snapshot endpoint (`/capture`)
- ✅ Camera stream info endpoint (`/cameraStreamInfo`)
- ✅ Feature toggle for HTTP camera (`http_cam_enabled` in NVS, gated init)
- ✅ Camera config at runtime via `/control?var=X&val=Y` (framesize, quality, brightness, contrast)
- ✅ Camera settings persisted to NVS and restored at boot
- ✅ Mic/audio settings persisted to NVS (mic_enabled, mic_muted, mic_sensitivity, aac_sample_rate, aac_bitrate)
- ✅ Combined `/status` endpoint returns system + camera + audio fields
- ✅ PSRAM 8MB OPI enabled in `sdkconfig.seeed_xiao_esp32s3_idf`
- ✅ lwIP sockets increased to 16 (was 10)
- ✅ Setup page Camera Config card fully functional (apply + persist all settings)

### Remaining TODO
- ❌ RTSP MJPEG server on port 8554 (using ESP-ADF `esp_media_protocols` library)
- ❌ `rtsp_enabled` feature toggle with NVS persistence
- ❌ RTSP client count → `LED_STATE_RTSP_ACTIVE` integration

### Success Criteria
| Criterion | Status | Notes |
|-----------|--------|-------|
| Camera init with PSRAM | ✅ | 8MB OPI, 2 frame buffers |
| MJPEG HTTP stream (port 81) | ✅ | Stable, tested with browser |
| JPEG snapshot (/capture) | ✅ | 640x480, ~35KB |
| Camera settings persist | ✅ | NVS save/restore across reboots |
| RTSP stream (port 8554) | ❌ | Pending |
| Stable stream to Scrypted | ⏳ | HTTP works, RTSP pending |

## Phase 5 — Audio path with ADF
- Wire I2S mic pipeline into ADF AAC-LC encoder → RTP/RTSP payloads.
- Support two mic sources: onboard PDM (GPIO41/42) and external INMP441 I2S (GPIO43/44/12).
- Mic source selectable in setup page, stored in NVS (`mic_source` key in `camera` namespace).
- I2S0 for MAX98357A speaker output (GPIO7/8/9), I2S1 for INMP441 mic input (GPIO43/44/12).
- Software volume control via ESP-ADF audio pipeline (not hardware GAIN pin).
  - MAX98357A GAIN pin: leave floating (9 dB fixed) — it's a hardware strap, not runtime-adjustable.
  - Volume slider on setup page, 0–100%, applied digitally before I2S output.
  - NVS key: `aud_volume` in `camera` namespace, default 80.
- Success: mono 16 kHz AAC stream in sync with video (acceptable A/V skew).

## Phase 6 — HomeKit doorbell
- Integrate Espressif HAP SDK (IDF) or external bridge; expose doorbell + camera.
- Success: doorbell event + live video/audio in Home app.

## Phase 7 — OTA update system
- Port the full OTA system from Arduino version to ESP-IDF.
- Credential management: username + SHA-256 password hash in NVS (`ota` namespace).
- Time-limited upload window (5 minutes), enabled via authenticated request.
- HTTP Basic auth for `/ota/config`, `/ota/enable` endpoints.
- Firmware upload via `/ota/update` (replaces bare-bones `/api/ota`).
- Remove LittleFS filesystem upload (not needed with embedded assets).
- Web UI already complete: `setup.html` OTA card + `ota.html` upload page.
- Success: secure OTA firmware update from local network, credentials survive reboots.

## Phase 8 — Cleanup & resilience
- Remove Arduino build artifacts, unused scripts, LittleFS stubs.
- Add factory-reset path for NVS, brownout handling, watchdog tuning.

---

## Current Architecture (Phase 4 In Progress)

### Component Structure
```
src_idf/
├── main/
│   └── main.c                    # Boot sequence, main loop, button/LED/camera integration
└── components/
    ├── nvs_manager/              # Raw NVS C API wrapper
    ├── wifi_manager/             # WiFi STA/AP/APSTA modes
    ├── web_server/               # HTTP server + REST API
    ├── dns_server/               # Captive portal DNS
    ├── log_buffer/               # Web-accessible log ring buffer
    ├── config_manager/           # Typed configuration storage
    ├── sip_client/               # SIP state machine + RTP
    ├── button/                   # Doorbell button (GPIO4)
    ├── status_led/               # PWM LED patterns (GPIO2)
    ├── camera/                   # OV2640 driver + NVS config (Phase 4)
    └── mjpeg_server/             # MJPEG HTTP streaming port 81 (Phase 4)
```

### Boot Sequence
```
1. NVS Manager Init (with auto-recovery)
2. Log Buffer Init (hooks into ESP-IDF logging)
   Status LED Init (PWM visual feedback)
   Button Init (GPIO4, debounce callback)
3. WiFi Manager Init (esp_netif + event loop)
4. WiFi Start (STA if credentials, else AP+DNS)
5. Web Server + SIP Client (via WiFi event callback)
   SNTP Init (when connected to WiFi)
6. Camera Init + MJPEG Server Start (deferred, after WiFi GOT_IP, if enabled)
```

### Main Loop Processing
- Button polling (debounced edge detection)
- Status LED pattern updates
- SIP message handling
- Deferred ring requests
- SIP registration refresh
- Deferred camera init (when WiFi connected + camera enabled)
- MJPEG client count → LED state tracking

### NVS Namespaces
| Namespace | Keys | Purpose |
|-----------|------|---------|
| `wifi` | `ssid`, `password` | WiFi credentials |
| `config` | `sip_*`, `cam_*`, `aud_*`, `sys_*` | Application settings |
| `sip` | `user`, `password`, `displayname`, `target`, `enabled`, `verbose` | SIP configuration |
| `camera` | `http_cam_en`, `framesize`, `quality`, `brightness`, `contrast` | Camera feature toggle + sensor config |
| `camera` | `mic_en`, `mic_mute`, `mic_sens`, `aac_rate`, `aac_bitr`, `mic_source` | Mic/audio config (NVS-only, Phase 5 hardware) |
| `ota` | `username`, `pass_hash` | OTA credentials (Phase 7) |

### GPIO Assignments
| GPIO | Function | Configuration |
|------|----------|---------------|
| GPIO4 | Doorbell Button | Active-low, internal pull-up, 50ms debounce |
| GPIO2 | Status LED | PWM (LEDC), 8-bit, 5kHz |
| GPIO1 | Door Opener Relay | Active-high (Phase 4+) |
| GPIO3 | Gong Relay | Active-high (Phase 4+) |
| GPIO7/8/9 | I2S0 DAC | MAX98357A speaker output (Phase 5) |
| GPIO41/42 | PDM Mic | Onboard mic (Phase 5) |
| GPIO43/44/12 | I2S1 Mic | INMP441 external mic — SCK/WS/SD (Phase 5) |

### API Endpoints
| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Redirect (AP→setup, STA→index) |
| `/api/wifi` | POST | Save WiFi credentials |
| `/api/wifi` | DELETE | Clear WiFi credentials |
| `/api/status` | GET | System status JSON (detailed) |
| `/api/ota` | POST | Firmware update |
| `/api/logs` | GET | Get logs (filter=all/core/camera/doorbell) |
| `/api/logs` | DELETE | Clear log buffer |
| `/api/features` | GET | Get feature toggle states |
| `/api/sip` | GET | Get SIP config |
| `/api/sip` | POST | Save SIP config |
| `/api/sip/ring` | POST | Trigger SIP ring |
| `/api/sip/verbose` | GET/POST | Toggle verbose logging |
| `/capture` | GET | JPEG snapshot (Phase 4) |
| `/cameraStreamInfo` | GET | Camera/streaming status (Phase 4) |
| `/control` | GET | Camera/mic settings `?var=X&val=Y` (Phase 4) |
| `/status` | GET | Combined system + camera + audio status (Phase 4) |
| `/saveFeatures` | POST | Save feature toggles |
| `/ring/sip` | GET | Legacy ring trigger |
| `/saveWiFi` | POST | Legacy credential save |
| `/saveSIP` | POST | Legacy SIP config save |
| `/scanWifi` | GET | Start WiFi scan |
| `/wifiScanResults` | GET | Get deduplicated scan results |
| `/deviceStatus` | GET | Basic device status |
| `/sipDebug` | GET | SIP debug info |
| `/restart` | GET | Styled restart page with auto-reconnect |

### Status LED Patterns (Priority Order)
1. **Ringing**: Breathing animation (1.4s period, 6s duration)
2. **AP Mode**: Double-blink (1s period)
3. **WiFi Connecting**: 2Hz blink (500ms period)
4. **SIP Error**: Slow pulse (2s period)
5. **SIP OK**: Steady low glow (duty 24/255)
6. **RTSP Active**: Short tick every 2s (future)

### SIP Integration Details
- **Fritz!Box Registration**: IP phone with internal number
- **Door Intercom Numbers**: `**11` = Ring Key 1, `**12` = Ring Key 2, etc.
- **Authentication**: MD5 digest with nonce from 401 response
- **Call Flow**: INVITE → 401 → Auth INVITE → 100/183 → 200 → ACK
- **Verbose Logging**: Single-line format with `|` separators

### Web Assets (Embedded Gzip)
12 files totaling ~35KB compressed:
- index.html, style.css, wifi-setup.html, setup.html
- live.html, guide.html, ota.html, sip.html, tr064.html
- logs.html, logs-camera.html, logs-doorbell.html

---

## Commands Reference

### Build and Upload
```bash
pio run -t upload -e seeed_xiao_esp32s3_idf
```

### Monitor Serial Output
```bash
pio device monitor -e seeed_xiao_esp32s3_idf
```

### Erase NVS (Clear Credentials)
```bash
~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbmodem21201 \
    erase_region 0x9000 0x5000
```

### Full Flash Erase
```bash
pio run -t erase -e seeed_xiao_esp32s3_idf
```

---

## Session Summary (February 6, 2026)

### Phase 3 Completion
1. **SIP Client**: Full port to ESP-IDF with lwIP sockets
2. **Fritz!Box Integration**: Register, INVITE, digest auth working
3. **Physical Button**: GPIO4 with debounce triggers SIP ring
4. **Status LED**: GPIO2 PWM with multiple state patterns
5. **SNTP**: Real timestamps in logs (German timezone)
6. **Config Reload**: Fixed stale config bug on ring
7. **Verbose Logging**: Single-line SIP packets for easy debugging

### Key Bug Fixes
1. **SIP config not reloading** - Config was cached at boot; now reloads from NVS on each ring
2. **Wrong target number** - Was sending `**11` when user configured `**12`
3. **Multi-line verbose logs** - Changed to single-line with `|` separators

### Hardware Tested
- XIAO ESP32-S3 Sense
- Fritz!Box 6591 Cable
- Physical button on GPIO4
- LED on GPIO2

### Fritz!Box Configuration
- Created IP phone (LAN/WLAN)
- Assigned internal number (e.g., 620)
- Configured Ring Keys: `**11` → Call group, `**12` → specific phone

---

## Session Summary (February 8-10, 2026)

### Phase 4 Progress (Camera + MJPEG Streaming)
1. **Camera Component**: OV2640 with PSRAM frame buffers, NVS config persistence
2. **MJPEG Server**: Raw lwIP sockets on port 81, max 2 clients, TCP_NODELAY
3. **Web Integration**: `/capture`, `/cameraStreamInfo`, `/control`, `/status` endpoints
4. **Feature Toggle**: HTTP camera enable/disable with NVS persistence, gated camera init
5. **Camera Config Card**: All settings (framesize, quality, brightness, contrast) saved to NVS
6. **Mic/Audio Config**: Settings stored in NVS (mic_enabled, mic_muted, sensitivity, sample rate, bitrate)
7. **CSS Layout Fix**: Cards top-align, grow to footer on desktop, scrollable on mobile

### Key Bug Fixes
1. **esp_camera not found** - PlatformIO lib_deps doesn't auto-register as IDF component; added EXTRA_COMPONENT_DIRS
2. **PSRAM not enabled** - Settings were in wrong sdkconfig file; PlatformIO uses `sdkconfig.seeed_xiao_esp32s3_idf`
3. **Socket exhaustion** - httpd + MJPEG server exceeded lwIP pool; increased CONFIG_LWIP_MAX_SOCKETS to 16
4. **Feature toggle not saving** - `/saveFeatures` only handled sip_enabled; added camera toggle
5. **Brightness/contrast not persisting** - OV2640 `WRITE_REG_OR_RETURN` macro returns non-zero on I2C issues; decoupled NVS save from sensor return code

### Memory Usage (Phase 4)
- RAM: 24.6% (80,728 / 327,680 bytes)
- Flash: 26.5% (1,043,865 / 3,932,160 bytes)

### Next: RTSP Server
- Implement RTSP MJPEG server on port 8554 using ESP-ADF `esp_media_protocols` library
- Wire `rtsp_enabled` feature toggle
- Test with Scrypted RTSP source
