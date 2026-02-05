<!--
 Project: HomeKitKnock-S3
 File: docs/IDF_ADF_MIGRATION_PLAN.md
 Purpose: Phased plan to migrate from Arduino-as-component to pure ESP-IDF + ESP-ADF
 Author: Codex (with user direction)
 Last Updated: February 4, 2026
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

### Success Criteria (All Met)
| Criterion | Status | Notes |
|-----------|--------|-------|
| Config storage with raw NVS | ✅ | `wifi` and `config` namespaces |
| HTTP OTA | ✅ | `/api/ota` endpoint working |
| Change Wi-Fi creds via HTTP | ✅ | Captive portal + auto-restart |
| Survives reboots | ✅ | NVS persistence verified |
| Web-based debugging | ✅ | Log buffer with web viewer |

---

## Phase 3 — SIP intercom (NEXT)
- Port SIP state machine to an IDF component; use lwIP sockets directly.
- remove TR-064 client (for Fritz!Box) as the current Fritz!boxes do not support dialing etc. anymore.
- Success: register to Fritz!Box, place/receive call on button press.

## Phase 4 — Video path
- Add esp32-camera component and a lightweight RTSP/MJPEG server (IDF HTTP + RTP).
- Success: stable 640x480/15fps stream to Scrypted, no watchdogs.

## Phase 5 — Audio path with ADF
- Wire I2S mic pipeline into ADF AAC-LC encoder → RTP/RTSP payloads.
- Success: mono 16 kHz AAC stream in sync with video (acceptable A/V skew).

## Phase 6 — HomeKit doorbell
- Integrate Espressif HAP SDK (IDF) or external bridge; expose doorbell + camera.
- Success: doorbell event + live video/audio in Home app.

## Phase 7 — Cleanup & resilience
- Remove Arduino build artifacts, unused scripts, LittleFS stubs.
- Add factory-reset path for NVS, brownout handling, watchdog tuning.

---

## Current Architecture (Phase 2 Complete)

### Component Structure
```
src_idf/
├── main/
│   └── main.c                    # Boot sequence, event loop
└── components/
    ├── nvs_manager/              # Raw NVS C API wrapper
    ├── wifi_manager/             # WiFi STA/AP/APSTA modes
    ├── web_server/               # HTTP server + REST API
    ├── dns_server/               # Captive portal DNS
    ├── log_buffer/               # Web-accessible log ring buffer
    └── config_manager/           # Typed configuration storage
```

### Boot Sequence (5 Steps)
```
1. NVS Manager Init (with auto-recovery)
2. Log Buffer Init (hooks into ESP-IDF logging)
3. WiFi Manager Init (esp_netif + event loop)
4. WiFi Start (STA if credentials, else AP+DNS)
5. Web Server (starts via WiFi event callback)
```

### NVS Namespaces
| Namespace | Keys | Purpose |
|-----------|------|---------|
| `wifi` | `ssid`, `password` | WiFi credentials |
| `config` | `sip_*`, `cam_*`, `aud_*`, `sys_*` | Application settings |

### API Endpoints
| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Redirect (AP→setup, STA→index) |
| `/api/wifi` | POST | Save WiFi credentials |
| `/api/wifi` | DELETE | Clear WiFi credentials |
| `/api/status` | GET | System status JSON |
| `/api/ota` | POST | Firmware update |
| `/api/logs` | GET | Get logs (filter=all/core/camera/doorbell) |
| `/api/logs` | DELETE | Clear log buffer |
| `/saveWiFi` | POST | Legacy credential save |
| `/scanWifi` | GET | Start WiFi scan |
| `/wifiScanResults` | GET | Get deduplicated scan results |
| `/restart` | GET | Styled restart page with auto-reconnect |

### Captive Portal
- DNS server redirects all queries to 192.168.4.1
- Detection handlers for iOS, Android, Windows, Firefox
- Auto-popup when connecting to `doorbell-setup` AP

### Web-Based Log Viewer (`/logs.html`)
- **Ring buffer** stores 100 log entries
- **Category tabs**: All, Core, Camera, Doorbell
- **Level filter**: All, Error (E), Warn (W), Info (I), Debug (D), Verbose (V)
- **Sort order**: Newest First / Oldest First
- **Font size**: XS, Small, Medium, Large, XL
- **Word wrap**: Toggle for long SIP messages
- **Auto-refresh**: 2s / 5s / 10s / Off
- **Color-coded levels**: Error (red), Warn (yellow), Info (green), Debug (cyan)
- **Full-width layout**: Table fills available viewport
- **Sticky footer**: Home, Refresh, Download, Clear buttons
- **Download**: Export filtered logs as text file
- **Preferences saved**: Font size and word wrap persist in localStorage
- **URL params**: Direct link to filtered view (e.g., `/logs.html?filter=core`)

### Web Assets (Embedded Gzip)
12 files totaling ~35KB compressed:
- index.html, style.css, wifi-setup.html, setup.html
- live.html, guide.html, ota.html, sip.html, tr064.html
- logs.html, logs-camera.html, logs-doorbell.html

---

## Commands Reference

### Build and Upload
```bash
pio run -t upload -e esp32s3_idf
```

### Monitor Serial Output
```bash
pio device monitor -e esp32s3_idf
```

### Erase NVS (Clear Credentials)
```bash
~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbmodem21201 \
    erase_region 0x9000 0x5000
```

### Full Flash Erase
```bash
pio run -t erase -e esp32s3_idf
```

---

## Session Summary (February 4, 2026)

### Issues Fixed
1. **Browser autofill bug** - Added `autocomplete="off"` to manual SSID input
2. **WiFi scan timeout** - Increased initial poll from 2s to 8s
3. **Duplicate SSIDs** - Added deduplication keeping strongest signal
4. **Missing auto-restart** - Device now restarts after saving credentials
5. **Captive portal** - Added DNS server + detection URL handlers
6. **Log viewer crash** - Fixed va_list handling with va_copy()
7. **Stack overflow in sys_evt** - Moved log formatting to static buffer with mutex
8. **Missing logs.html** - Added to embedded assets list
9. **Query string in asset URLs** - Fixed find_asset() to strip `?params`

### Log Viewer Enhancements (Feb 4)
1. Full-width responsive layout matching main dashboard style
2. Category tabs (All, Core, Camera, Doorbell)
3. Level filter dropdown (All, Error, Warn, Info, Debug, Verbose)
4. Sort order (Newest/Oldest first)
5. Font size control (XS to XL)
6. Word wrap toggle for long messages
7. Sticky footer with action buttons
8. Preferences saved in localStorage

### WiFi Provisioning Flow (Working)
1. Device boots into AP mode (`doorbell-setup` / `doorbell123`)
2. Phone connects → captive portal pops up automatically
3. User selects network, enters password
4. Device saves credentials and auto-restarts
5. Device connects to home WiFi
6. Web UI available at assigned IP
