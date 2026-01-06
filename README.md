<!--
 Project: HomeKitKnock-S3
 File: README.md
 Author: Jesse Greene
 -->

# ESP32-S3 Sense Doorbell Firmware (PlatformIO)

![License](https://img.shields.io/github/license/JFG3rd/HomeKitKnock-S3)

Firmware for the **ESP32-S3 Sense–based DIY video doorbell**, built with **PlatformIO (Arduino framework)**. Streams camera video to **Scrypted** and triggers **HomeKit doorbell events** via HTTP.

---

## 🎯 Purpose

- Stream camera video from **XIAO ESP32-S3 Sense** to Scrypted
- Detect a physical doorbell button press
- Trigger a Scrypted **doorbell endpoint**
- Trigger a FRITZ!Box **TR-064 internal ring** for a phone group
- Let Scrypted expose the device as a **HomeKit Secure Video doorbell**

Audio streaming is planned for a later phase.

---

## 🧰 Requirements

### Hardware
- Seeed Studio **XIAO ESP32-S3 Sense**
- Doorbell push button connected to a GPIO
- USB data cable for programming
- (Optional) AC detector or relay interface for legacy 8 V gong

### Software
- **Visual Studio Code** with **PlatformIO IDE extension**, or the PlatformIO CLI (`pio`)
- ESP32 platform installed via PlatformIO

---

## 🚀 Quick Start

1. Open this folder in VS Code (PlatformIO will detect the project).
2. Build:
   - VS Code: PlatformIO "Build" task
   - CLI: `pio run`
3. Upload:
   - VS Code: PlatformIO "Upload" task
   - CLI: `pio run -t upload`
4. Serial monitor (adjust baud if needed):
   - VS Code: PlatformIO "Monitor" task
   - CLI: `pio device monitor -b 115200`

---

## 📶 WiFi Provisioning (AP Mode)

On boot, the firmware loads saved WiFi credentials from NVS (Preferences).
If none are found or the connection fails, it starts a configuration AP:

- SSID: `ESP32_Doorbell_Setup`
- Captive portal via DNS redirect
- Default AP IP: `192.168.4.1`

### AP Mode Endpoints
- `GET /wifiSetup` - WiFi setup UI
- `POST /saveWiFi` - Save credentials (JSON: `{ "ssid": "...", "password": "..." }`)
- `GET /wifiStatus` - JSON status
- `GET /style.css` - UI styles (served from LittleFS)
- `GET /scanWifi` - Trigger an async WiFi rescan for the setup UI

### Normal Mode Endpoints
- `GET /` - Device status page
- `GET /forget` - Clears WiFi credentials and restarts
- `GET /deviceStatus` - JSON status (RSSI + uptime)
- `GET /ring` - Trigger ring (tries HTTP then TR-064)
- `GET /ring/http` - Trigger FRITZ!Box HTTP click-to-dial ring
- `GET /ring/tr064` - Trigger FRITZ!Box TR-064 ring
- `GET /ring/sip` - Trigger FRITZ!Box SIP ring
- `GET /tr064` - TR-064/HTTP ring setup page
- `GET /tr064Debug` - TR-064 debug JSON
- `GET /sip` - SIP setup page
- `GET /sipDebug` - SIP debug JSON

### TR-064 Settings
The setup page lets you configure FRITZ!Box TR-064 credentials and the internal
ring number (e.g., `**9` or a group extension). These settings are stored in NVS.
The router IP is derived from the WiFi gateway after the device connects.
Assign a custom ringtone on the handset for that internal number.

TR-064 is configurable from:
- AP setup page: `http://192.168.4.1/wifiSetup`
- Normal mode page: `http://<device-ip>/tr064`

### Camera Stream
- Stream URL: `http://<device-ip>:81/stream`
- Snapshot: `http://<device-ip>/capture`
- Status JSON: `http://<device-ip>/status`
- Control: `http://<device-ip>/control?var=<name>&val=<value>`
- RTSP URL: `rtsp://<device-ip>:8554/mjpeg/1`

### Scrypted Setup (Camera + Doorbell)
1. In Scrypted, add a new Camera device (or ONVIF/MJPEG camera).
2. Set the stream URL to `rtsp://<device-ip>:8554/mjpeg/1` (preferred) or `http://<device-ip>:81/stream` (MJPEG).
3. Set the snapshot URL to `http://<device-ip>/capture`.
4. Create or update a Doorbell Group and link this camera.
5. Expose the doorbell to HomeKit via the Scrypted HomeKit plugin.

### SIP Setup (FRITZ!Box IP Phone)
1. FRITZ!Box UI → Telefonie → Telefoniegeräte → “Neues Gerät einrichten”.
2. Select “Telefon (mit und ohne Anrufbeantworter)” → “LAN/WLAN (IP-Telefon)”.
3. Assign a username/password (e.g., 620) and name it “ESP32-Doorbell”.
4. Enter SIP username/password and target number in `http://<device-ip>/sip`.
5. Use `**610` to ring all DECT phones or a specific extension (e.g., `**611`).

### Workflow Summary
1. Flash firmware: `pio run -t upload`
2. Upload UI assets: `pio run -t uploadfs`
3. Connect to `ESP32_Doorbell_Setup` and open `http://192.168.4.1/wifiSetup`
4. Save WiFi and TR-064 credentials
5. After reboot, open `http://<device-ip>/` for the main UI
6. Verify camera and stream URLs, then add to Scrypted

### FRITZ!Box TR-064 Setup (DECT Ring)
1. Enable TR-064 access: FRITZ!Box UI → Heimnetz → Heimnetzübersicht → Netzwerkeinstellungen → enable „Zugriff für Anwendungen zulassen“.
2. TR-064 endpoints are available at `http://fritz.box:49000` and `https://fritz.box:49443`.
3. Create a dedicated user: System → FRITZ!Box-Benutzer.
4. Grant permissions: FRITZ!Box Einstellungen, Telefonie, Smart Home (if needed).
5. Configure an internal group number for the handsets you want to ring (e.g., `**9` or `**610`).
6. In AP setup (`/wifiSetup`) or normal mode (`/tr064`), enter the TR-064 username, password, and group number.
7. On each handset, assign a custom ringtone to that internal number.
8. If rings still fail, enable “Wählhilfe / Click-to-dial” for the device in FRITZ!Box and retry.

### Troubleshooting
- If the stream does not load, confirm `http://<device-ip>:81/stream` is reachable and that port 81 is not blocked.
- If snapshots fail, try `http://<device-ip>/capture` first and check serial logs for camera init errors.
- If the camera fails to initialize, confirm the XIAO ESP32-S3 Sense pin map in `include/camera_pins.h`.
- If TR-064 calls fail, verify TR-064 is enabled and the user has permissions, then re-save credentials in `/wifiSetup`.
- If DECT phones do not ring, confirm the internal group number and assign a ringtone for that number on the handsets.

### UI Assets
The UI stylesheet is served from LittleFS. After editing `data/style.css`, run:
`pio run -t uploadfs`

---

## ⚙️ PlatformIO Configuration

The environment is defined in [platformio.ini](platformio.ini) and targets:

| Item | Value |
|----|----|
| Board | `seeed_xiao_esp32s3` |
| Framework | Arduino |
| PSRAM | Required |

### Key Flags for ESP32-S3 Sense

```ini
[env:seeed_xiao_esp32s3]
platform = espressif32
board = seeed_xiao_esp32s3
framework = arduino
board_build.flash_mode = qio
board_build.psram_mode = opi
build_flags = 
    -DBOARD_HAS_PSRAM
```

- **PSRAM enabled** via `-DBOARD_HAS_PSRAM`
- **`board_build.psram_mode = opi`** – OPI mode for ESP32-S3
- **`board_build.flash_mode = qio`** – QIO flash mode

---

## 📁 Project Layout

- `src/` – Firmware source ([src/main.cpp](src/main.cpp))
- `include/` – Headers
- `lib/` – Local libraries
- `test/` – Tests
- `docs/` – Documentation

See [docs/esp32-s3-doorbell-architecture.md](docs/esp32-s3-doorbell-architecture.md) for detailed architecture information.

---

## 📝 Notes

- If uploads fail, select the correct serial port in PlatformIO.
- Keep [platformio.ini](platformio.ini) aligned with the board memory configuration (PSRAM and flash mode).
- Refer to [AGENTS.md](AGENTS.md) for AI assistant guidance on this project.

---

## 🤝 Contributing

See `CONTRIBUTING.md`.

---

## 📄 License

Apache-2.0. See `LICENSE`.
