<!--
 Project: HomeKitKnock-S3
 File: docs/IMPLEMENTATION_SUMMARY.md
 Author: Jesse Greene
 Last Updated: February 4, 2026
 -->

# ESP32-S3 Doorbell - Implementation Summary

## Project Status: ESP-IDF Migration Phase 2 Complete ✅

The project has successfully migrated from Arduino to pure ESP-IDF. WiFi provisioning via captive portal is fully functional, with web-based log viewing for debugging without serial monitor.

---

## What's Working

### WiFi Provisioning ✅
- **Captive Portal**: Auto-popup on iOS/Android/Windows when connecting to AP
- **DNS Server**: Redirects all queries to 192.168.4.1
- **WiFi Scanning**: APSTA mode with cached, deduplicated results
- **Credential Storage**: NVS persistence across reboots
- **Auto-Restart**: Device reboots after saving credentials
- **Styled Restart Page**: Progress bar with auto-reconnect

### Web Interface ✅
- **Embedded Assets**: 12 gzip-compressed HTML/CSS files (~35KB)
- **REST API**: WiFi config, status, OTA, logs
- **Log Viewer**: Filter by category, auto-refresh, download
- **Root Redirect**: AP mode → setup, STA mode → dashboard

### System Infrastructure ✅
- **NVS Manager**: Raw C API with auto-recovery
- **WiFi Manager**: STA/AP/APSTA modes with events
- **DNS Server**: Captive portal support
- **Log Buffer**: Ring buffer with ESP-IDF logging hook
- **Web Server**: esp_http_server with wildcard routing

---

## Architecture

### Component Structure
```
src_idf/
├── main/main.c                          # Entry point (5-step boot)
├── components/
│   ├── nvs_manager/                     # NVS abstraction
│   ├── wifi_manager/                    # WiFi state machine
│   ├── web_server/                      # HTTP server + API
│   ├── dns_server/                      # Captive portal DNS
│   ├── log_buffer/                      # Web log viewer backend
│   └── config_manager/                  # Settings storage
├── generated/embedded_web_assets.*      # Gzip assets (auto-generated)
```

### Boot Sequence
```
[1/5] NVS Manager Init (with corruption recovery)
[2/5] Log Buffer Init (hooks into ESP-IDF logging)
[3/5] WiFi Manager Init (esp_netif + events)
[4/5] WiFi Start (STA if credentials, else AP+DNS)
[5/5] Web Server (via WiFi event callback)
```

### API Endpoints
| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Redirect based on WiFi state |
| `/api/wifi` | POST | Save WiFi credentials |
| `/api/wifi` | DELETE | Clear WiFi credentials |
| `/api/status` | GET | System status JSON |
| `/api/ota` | POST | Firmware update |
| `/api/logs` | GET | Get logs with filter |
| `/api/logs` | DELETE | Clear log buffer |
| `/saveWiFi` | POST | Legacy credential save |
| `/scanWifi` | GET | Start WiFi scan |
| `/wifiScanResults` | GET | Get scan results (deduplicated) |
| `/restart` | GET | Restart with progress UI |
| `/generate_204` | GET | Android captive portal |
| `/hotspot-detect.html` | GET | iOS captive portal |

### Web Pages
| Page | Purpose |
|------|---------|
| `index.html` | Main dashboard |
| `wifi-setup.html` | WiFi provisioning |
| `logs.html` | Log viewer with filters |
| `ota.html` | Firmware update |
| `sip.html` | SIP configuration (Phase 3) |
| `live.html` | Live A/V viewer (Phase 4) |

---

## Key Files

### ESP-IDF Components
| File | Purpose |
|------|---------|
| `src_idf/main/main.c` | Boot sequence |
| `src_idf/components/nvs_manager/*` | NVS operations |
| `src_idf/components/wifi_manager/*` | WiFi state machine |
| `src_idf/components/web_server/*` | HTTP server + handlers |
| `src_idf/components/dns_server/*` | Captive portal DNS |
| `src_idf/components/log_buffer/*` | Log ring buffer |

### Web Assets
| File | Purpose |
|------|---------|
| `data/index.html` | Main dashboard |
| `data/wifi-setup.html` | WiFi provisioning |
| `data/logs.html` | Log viewer |
| `data/style.css` | Unified styling |

---

## Migration Progress

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | ✅ Complete | Pre-migration hygiene |
| Phase 1 | ✅ Complete | IDF base (boot, NVS, WiFi, web) |
| Phase 2 | ✅ Complete | Config services, captive portal, logs |
| Phase 3 | ⏳ Next | SIP intercom |
| Phase 4 | ❌ Pending | Video path (RTSP/MJPEG) |
| Phase 5 | ❌ Pending | Audio path with ADF |
| Phase 6 | ❌ Pending | HomeKit doorbell |
| Phase 7 | ❌ Pending | Cleanup & resilience |

---

## Features Detail

### Captive Portal
When device is in AP mode:
1. DNS server starts on port 53
2. All DNS queries return 192.168.4.1
3. Detection URLs trigger redirect to setup page:
   - `/generate_204` (Android)
   - `/hotspot-detect.html` (iOS/macOS)
   - `/connecttest.txt` (Windows)
   - `/canonical.html` (Firefox)
4. Mobile device shows "Sign in to network" popup

### Web Log Viewer (`/logs.html`)
- **Ring Buffer**: 100 entries captured from ESP-IDF logging
- **Category Tabs**:
  - All: Everything
  - Core: main, wifi, nvs, web_server, dns, httpd
  - Camera: camera, rtsp, mjpeg, stream
  - Doorbell: doorbell, sip, button, tr064
- **Controls**:
  - Level filter: All / Error (E) / Warn (W) / Info (I) / Debug (D) / Verbose (V)
  - Sort order: Newest First / Oldest First
  - Font size: XS / Small / Medium / Large / XL
  - Word wrap: Toggle for long messages
  - Auto-refresh: Off / 2s / 5s / 10s
- **Features**:
  - Color-coded levels (E=red, W=yellow, I=green, D=cyan, V=gray)
  - Full-width responsive table layout
  - Sticky footer with action buttons
  - Download filtered logs as text file
  - Clear all logs
  - Preferences saved in localStorage
  - Direct URL linking (`/logs.html?filter=core`)

### WiFi Scanning
- Uses APSTA mode (AP + Station simultaneously)
- Scan results cached (ESP-IDF API is destructive)
- Deduplication: Same SSID from multiple APs merged
- Strongest signal kept for each unique SSID

### Restart Page
- Styled HTML with progress bar
- Status messages during reboot
- Auto-polls `/api/status` to detect reconnect
- Redirects to `/` when device is back online

---

## Useful Commands

### Build and Upload
```bash
pio run -t upload -e esp32s3_idf
```

### Monitor Serial
```bash
pio device monitor -e esp32s3_idf
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
http://<device-ip>/logs.html?filter=camera
http://<device-ip>/logs.html?filter=doorbell
```

---

## Hardware

**Platform:** Seeed XIAO ESP32-S3 Sense
**Flash:** 8 MB
**PSRAM:** 8 MB (OPI)
**Camera:** OV2640

**Memory Usage (ESP-IDF build):**
- RAM: ~70KB used
- Flash: ~1.4MB (includes ~35KB embedded web assets)

---

## Next Steps (Phase 3)

1. Port SIP client to ESP-IDF component
2. Implement lwIP socket-based SIP state machine
3. Add TR-064 client for Fritz!Box integration
4. Test SIP registration and call flow

See [IDF_ADF_MIGRATION_PLAN.md](IDF_ADF_MIGRATION_PLAN.md) for full roadmap.
