<!--
 Project: HomeKitKnock-S3
 File: docs/esp32-s3-doorbell-architecture.md
 Author: Jesse Greene
-->

# ESP32-S3 Doorbell — System Architecture

This document describes the overall design of the ESP32-S3 DIY video doorbell: how it's wired, what each piece of software does, and how the parts fit together.

---

## What This Project Does

A single **XIAO ESP32-S3 Sense** board handles:
- **Video** — OV2640 camera streams MJPEG over RTSP and HTTP
- **Audio** — INMP441 mic captures AAC-LC audio for RTSP; MAX98357A speaker plays doorbell chime and SIP call audio
- **SIP intercom** — registers with a FRITZ!Box as a door intercom, rings DECT phones, supports two-way talk and door opener
- **Relays** — one relay triggers the original doorbell gong, another opens the door
- **HomeKit** — Scrypted bridges the RTSP stream to HomeKit Secure Video

---

## How the Doorbell Event Works

When someone presses the button, this happens in sequence:

1. **GPIO4** detects the button press (active-low, debounced)
2. ESP32 plays a **ding-dong chime** over the speaker (MAX98357A)
3. ESP32 pulses **GPIO3** to ring the original 8VAC gong through a relay
4. ESP32 sends a **SIP INVITE** to the FRITZ!Box, which rings all DECT phones
5. ESP32 triggers the **Scrypted doorbell webhook** via HTTP
6. Scrypted fires a **HomeKit doorbell notification** — your iPhone shows the camera feed

When someone answers on a FRITZ!fon:
- **Two-way audio** starts (G.711 PCMU over RTP)
- Pressing "Open" on the FRITZ!fon sends DTMF digits "1-2-3" via SIP INFO
- ESP32 detects the sequence and pulses **GPIO1** to open the door

---

## Hardware Overview

### Board
**Seeed Studio XIAO ESP32-S3 Sense** — ESP32-S3 with 8MB Flash, 8MB PSRAM (OPI), and an OV2640 camera on the expansion board.

### GPIO Assignments

| GPIO | What It Does | Wiring Notes |
|------|-------------|--------------|
| GPIO1 | Door opener relay | Active-high; fires on DTMF "123" from FRITZ!fon |
| GPIO2 | Status LED | 330 ohm resistor to LED; shows WiFi/SIP/ring status |
| GPIO3 | Gong relay | Active-high; 150ms startup delay, then 800ms pulse |
| GPIO4 | Doorbell button | Wire button between GPIO4 and GND (pull-up in firmware) |
| GPIO5 | INMP441 mic data (SD) | **Must be GPIO5** — GPIO12 is used by the camera |
| GPIO7 | Shared I2S BCLK | Connects to both MAX98357A and INMP441 |
| GPIO8 | Shared I2S WS | Connects to both MAX98357A and INMP441 |
| GPIO9 | Speaker data (DIN) | MAX98357A I2S data input |
| GPIO10-18, 38-40, 47-48 | OV2640 camera | Fixed by the Sense expansion board |
| GPIO41/42 | Onboard PDM mic | Built into the board; INMP441 is the preferred mic |

### Audio Hardware

The speaker (MAX98357A) and microphone (INMP441) share the same I2S clock lines (GPIO7 BCLK, GPIO8 WS). The firmware manages this as a full-duplex shared bus — both can run simultaneously.

- **MAX98357A**: I2S DAC/amplifier. Tie SC pin to 3V3 (always on). Connect a small 4-8 ohm speaker to L+/L-.
- **INMP441**: I2S MEMS microphone. Tie L/R pin to GND (left channel). Wire SD to GPIO5.
- **Important**: GPIO12 looks available on the header but it's the camera's Y7 data line — the camera chip drives it electrically even when the camera software is off.

### Power Supply

The doorbell runs from your existing **8VAC doorbell transformer**:
- Bridge rectifier → supercapacitor bank (2x 5.5F) → buck converter → 3.3V
- The supercaps provide ~45 seconds of hold-up power when the gong relay briefly disconnects the transformer
- See [POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md) for the full schematic

---

## Software Architecture

### Framework
Pure **ESP-IDF** (no Arduino). Built with **PlatformIO**.

### Component Structure

The firmware is organized into 21 independent ESP-IDF components:

| Component | What It Does |
|-----------|-------------|
| `nvs_manager` | Non-volatile storage (settings persist across reboots) |
| `wifi_manager` | WiFi STA/AP/APSTA modes with auto-reconnect |
| `web_server` | HTTP server hosting all web pages and REST APIs |
| `dns_server` | Captive portal DNS (redirects to setup page in AP mode) |
| `log_buffer` | Ring buffer that feeds the web log viewer |
| `config_manager` | Typed configuration storage |
| `sip_client` | Full SIP stack — registration, calls, RTP audio, DTMF |
| `button` | Doorbell button handler with debounce |
| `status_led` | PWM LED with breathing, blinking, and pulse patterns |
| `relay_controller` | Gong relay (GPIO3) + door opener relay (GPIO1) |
| `camera` | OV2640 driver with runtime config and NVS persistence |
| `mjpeg_server` | HTTP MJPEG streaming on port 81 |
| `rtsp_server` | RTSP 1.0 server on port 8554 (MJPEG video + AAC audio) |
| `audio_output` | MAX98357A speaker output, gong playback, volume control |
| `audio_capture` | INMP441 mic capture with sensitivity control |
| `i2s_shared_bus` | Full-duplex I2S bus shared between speaker and mic |
| `aac_encoder_pipe` | AAC-LC encoder for RTSP audio stream |
| `timestamp_overlay` | Burns timestamp and camera name into JPEG frames |
| `embedded_web_assets` | Gzip-compressed web files served from flash |
| `adf_codec` | ESP-ADF audio codec wrapper (for AAC encoding) |
| `adf_pipeline` | ESP-ADF pipeline wrapper (for AAC encoding) |

### Boot Sequence

```
1. NVS init (with corruption auto-recovery)
2. Log buffer + Status LED + Doorbell button + Speaker init
3. WiFi init (STA if credentials saved, else AP + captive portal)
4. Web server + SIP client start (on WiFi connect)
5. Mic capture init (if mic enabled in settings)
6. Camera + MJPEG/RTSP servers start (if camera enabled in settings)
```

The speaker initializes unconditionally at boot — the doorbell chime is a core feature that works even without WiFi. Camera and mic are deferred until WiFi connects and their feature toggles are enabled.

### Main Loop (10ms cycle)

The main loop handles:
- Button polling and debounce
- LED pattern updates
- SIP message processing and RTP media
- Registration refresh
- Deferred initialization of camera and mic

Heavy work (MJPEG streaming, RTSP serving) runs on separate tasks pinned to Core 1 to keep WiFi/networking responsive on Core 0.

---

## Streaming

### RTSP (Primary — for Scrypted)

The RTSP server on port 8554 sends:
- **Video**: MJPEG frames wrapped in RTP per RFC 2435
- **Audio**: AAC-LC at 16kHz wrapped in RTP per RFC 3640

Scrypted's FFmpeg Camera plugin transcodes the MJPEG to H.264 for HomeKit compatibility.

### HTTP MJPEG (Secondary — for browsers)

Port 81 serves a multipart MJPEG stream directly viewable in any browser. Good for quick testing but less efficient than RTSP.

### Snapshots

`/capture` returns a single JPEG frame — used by Scrypted for thumbnails and HomeKit notifications.

---

## Web Interface

All web pages are embedded as gzip-compressed C arrays in the firmware. No filesystem partition needed — everything is in flash.

| Page | What It's For |
|------|--------------|
| Dashboard (`/`) | System status at a glance |
| Setup (`/setup`) | All configuration — features, camera, audio, SIP, Scrypted, security |
| Live (`/live`) | Video + audio preview in the browser |
| Logs (`/logs.html`) | Filterable log viewer with download |
| OTA (`/ota`) | Firmware upload (time-limited, password protected) |
| WiFi Setup (`/wifi-setup.html`) | WiFi provisioning in AP mode |

### Security

- On first boot, the device redirects to `/first-setup` where you create a username and password
- Protected pages use HTTP Basic Auth (credentials cached in browser session)
- OTA uploads require an explicit "Enable OTA" step that opens a 5-minute upload window
- Factory reset: hold the doorbell button 5s, release, hold 5s again — erases all settings

---

## Settings Storage (NVS)

All settings are stored in ESP-IDF's Non-Volatile Storage and survive reboots and OTA updates.

| Namespace | What's Stored |
|-----------|--------------|
| `wifi` | SSID and password |
| `sip` | SIP credentials, target number, enable/verbose flags |
| `camera` | Camera sensor settings, mic/audio config, feature toggles, overlay settings |
| `relay` | Gong pulse duration, door opener pulse duration |
| `rtsp` | RTSP credentials, Scrypted streaming preferences |
| `system` | Timezone |
| `auth` | Device username + SHA-256 password hash |

---

## Status LED Patterns

The LED on GPIO2 shows what the doorbell is doing (highest priority wins):

| Pattern | Meaning |
|---------|---------|
| Breathing (fade in/out) | Doorbell is ringing |
| Fast double-blink | AP mode (waiting for WiFi setup) |
| 2Hz blink | Connecting to WiFi |
| Slow pulse | SIP registration error |
| Steady low glow | SIP registered, idle, ready |

---

## Development Phases

The firmware was built incrementally over 8 phases:

| Phase | Status | What Was Built |
|-------|--------|---------------|
| 0 | Done | Flash config, partition alignment |
| 1 | Done | ESP-IDF base: boot, NVS, WiFi, web server |
| 2 | Done | Captive portal, log viewer, config storage |
| 3 | Done | SIP client, doorbell button, LED, SNTP |
| 4 | Done | Camera, MJPEG streaming, RTSP server |
| 5 | Done | Speaker + mic audio, shared I2S bus, SIP two-way audio |
| 6 | Done | Scrypted integration, RTSP AAC audio, camera overlays |
| 7 | Done | OTA updates, device authentication, security |
| 8 | Planned | Cleanup: remove legacy code, add watchdog hardening |

See [IDF_ADF_MIGRATION_PLAN.md](IDF_ADF_MIGRATION_PLAN.md) for detailed session logs and bug fix history.

---

## Memory Usage (March 2026)

| Resource | Used | Total | Percentage |
|----------|------|-------|------------|
| RAM | 156,832 bytes | 327,680 bytes | 47.9% |
| Flash | 1,270,725 bytes | 3,932,160 bytes | 32.3% |

---

## Integration Map

| Responsibility | Who Handles It |
|---------------|---------------|
| Camera + audio streaming | ESP32-S3 |
| Doorbell button + gong | ESP32-S3 |
| DECT phone ringing | ESP32 → FRITZ!Box SIP |
| Door opener | ESP32 relay via FRITZ!fon DTMF |
| Video transcoding (H.264) | Scrypted (FFmpeg) |
| HomeKit bridge | Scrypted HomeKit plugin |
| HSV recording + analytics | Apple Home hub |
| NVR storage | Scrypted |

---

## Documentation Index

| Document | Description |
|----------|-------------|
| [README.md](../README.md) | Quick start and project overview |
| [SCRYPTED_RTSP_SETUP.md](SCRYPTED_RTSP_SETUP.md) | Scrypted camera + doorbell setup |
| [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) | Full wiring schematic |
| [GPIO_MAP.md](GPIO_MAP.md) | GPIO pin assignments and availability |
| [PROJECT_BOM.md](PROJECT_BOM.md) | Bill of materials |
| [POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md) | Supercapacitor power circuit |
| [AUDIO_INTEGRATION.md](AUDIO_INTEGRATION.md) | Audio paths and tuning |
| [SIP_INTEGRATION.md](SIP_INTEGRATION.md) | SIP protocol details |
| [IDF_ADF_MIGRATION_PLAN.md](IDF_ADF_MIGRATION_PLAN.md) | Development history and session logs |
| [OTA_UPDATE_FILE.md](OTA_UPDATE_FILE.md) | OTA image creation |
| [QUICK_START.md](QUICK_START.md) | Fast setup checklist |
