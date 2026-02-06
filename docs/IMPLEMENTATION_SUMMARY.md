<!--
 Project: HomeKitKnock-S3
 File: docs/IMPLEMENTATION_SUMMARY.md
 Author: Jesse Greene
 Last Updated: February 6, 2026
 -->

# ESP32-S3 Doorbell - Implementation Summary

## Project Status: ESP-IDF Migration Phase 3 Complete ✅

The project has successfully completed Phase 3: SIP client integration. The doorbell can now ring Fritz!Box DECT phones via SIP, with physical button input and visual status LED feedback.

---

## What's Working

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
- **REST API**: WiFi config, status, OTA, logs, SIP config
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
│   └── status_led/                      # PWM LED patterns
├── generated/embedded_web_assets.*      # Gzip assets (auto-generated)
```

### Boot Sequence
```
[1/5] NVS Manager Init (with corruption recovery)
[2/5] Log Buffer Init (hooks into ESP-IDF logging)
      Status LED Init (PWM for visual feedback)
      Button Init (GPIO4 with debounce callback)
[3/5] WiFi Manager Init (esp_netif + events)
[4/5] WiFi Start (STA if credentials, else AP+DNS)
[5/5] Web Server + SIP Client (via WiFi event callback)
```

### Main Loop
- Button polling (debounced, triggers SIP ring)
- Status LED updates (pattern based on system state)
- SIP processing (registration, call handling)
- SNTP initialization (when WiFi connected)

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
| `/api/sip` | GET | Get SIP config (without password) |
| `/api/sip` | POST | Save SIP config |
| `/api/sip/ring` | POST | Trigger SIP ring |
| `/api/sip/verbose` | GET/POST | Verbose logging toggle |
| `/ring/sip` | GET | Legacy SIP ring trigger |
| `/saveWiFi` | POST | Legacy credential save |
| `/saveSIP` | POST | Legacy SIP config save |
| `/scanWifi` | GET | Start WiFi scan |
| `/wifiScanResults` | GET | Get scan results (deduplicated) |
| `/restart` | GET | Restart with progress UI |

### Web Pages
| Page | Purpose |
|------|---------|
| `index.html` | Main dashboard |
| `wifi-setup.html` | WiFi provisioning |
| `logs.html` | Log viewer with filters |
| `ota.html` | Firmware update |
| `sip.html` | SIP configuration |
| `live.html` | Live A/V viewer (Phase 4) |

---

## GPIO Assignments

| GPIO | Function | Notes |
|------|----------|-------|
| GPIO4 | Doorbell Button | Active-low, internal pull-up, 50ms debounce |
| GPIO2 | Status LED | PWM, 330Ω resistor to LED |
| GPIO1 | Door Opener Relay | Active-high (future) |
| GPIO3 | Gong Relay | Active-high (future) |
| GPIO5/6 | I2C | Reserved for sensors |
| GPIO7/8/9 | I2S DAC | MAX98357A audio output (future) |
| GPIO41/42 | PDM Mic | Onboard microphone (future) |

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

### Web Assets
| File | Purpose |
|------|---------|
| `data/index.html` | Main dashboard |
| `data/wifi-setup.html` | WiFi provisioning |
| `data/logs.html` | Log viewer |
| `data/sip.html` | SIP configuration |
| `data/style.css` | Unified styling |

---

## Migration Progress

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | ✅ Complete | Pre-migration hygiene |
| Phase 1 | ✅ Complete | IDF base (boot, NVS, WiFi, web) |
| Phase 2 | ✅ Complete | Config services, captive portal, logs |
| Phase 3 | ✅ Complete | SIP intercom, button, LED, SNTP |
| Phase 4 | ⏳ Next | Video path (RTSP/MJPEG) |
| Phase 5 | ❌ Pending | Audio path with ADF |
| Phase 6 | ❌ Pending | HomeKit doorbell |
| Phase 7 | ❌ Pending | Cleanup & resilience |

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

### Status LED Patterns
Priority (highest to lowest):
1. **Ringing**: Breathing (fade in/out over 1.4s, 6s duration)
2. **AP Mode**: Double-blink every 1s
3. **WiFi Connecting**: 2Hz blink
4. **SIP Error**: Slow pulse (2s period)
5. **SIP OK**: Steady low glow (24/255 duty)
6. **RTSP Active**: Short tick every 2s (future)

### Log Timestamps
- **SNTP Synced**: Full date/time (2026-02-06 15:30:45)
- **Pre-Sync**: Uptime (0:05:23)
- **Timezone**: German CET/CEST with automatic DST

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

---

## Hardware

**Platform:** Seeed XIAO ESP32-S3 Sense
**Flash:** 8 MB
**PSRAM:** 8 MB (OPI)
**Camera:** OV2640

**Memory Usage (ESP-IDF build):**
- RAM: ~72KB used (22%)
- Flash: ~950KB (24%)

---

## Next Steps (Phase 4)

1. Add esp32-camera component
2. Implement MJPEG HTTP streaming
3. Add RTSP server for Scrypted integration
4. Test video stability with watchdog handling

See [IDF_ADF_MIGRATION_PLAN.md](IDF_ADF_MIGRATION_PLAN.md) for full roadmap.
