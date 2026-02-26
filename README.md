<!--
 Project: HomeKitKnock-S3
 File: README.md
 Author: Jesse Greene
 -->

# ESP32-S3 Sense Doorbell Firmware (PlatformIO)

![License](https://img.shields.io/github/license/JFG3rd/HomeKitKnock-S3)

Firmware for the **ESP32-S3 Sense–based DIY video doorbell**, built with **PlatformIO**. Currently migrating from Arduino to **pure ESP-IDF** for improved stability and audio support via ESP-ADF.

**Current Status:** ESP-IDF Migration Phase 2 Complete ✅
- WiFi provisioning via captive portal
- Web-based configuration and log viewer
- See [docs/IDF_ADF_MIGRATION_PLAN.md](docs/IDF_ADF_MIGRATION_PLAN.md) for roadmap

---

## 🎯 Purpose

- Stream camera video from **XIAO ESP32-S3 Sense** to Scrypted
- Detect a physical doorbell button press
- Trigger a Scrypted **doorbell endpoint**
- Let Scrypted expose the device as a **HomeKit Secure Video doorbell**

Audio streaming is planned for a later phase.

---

## 🧰 Requirements

### Hardware
- Seeed Studio **XIAO ESP32-S3 Sense**
- Doorbell push button (GPIO4 to GND, internal pull-up)
- Status LED + 330 ohm resistor (GPIO2 to GND, online/ready indicator)
- MAX98357A I2S mono amp + small speaker (local gong output)
- USB data cable for programming
- **8VAC transformer** (existing doorbell transformer) + supercapacitor ride-through circuit (see BOM below)
  - Bridge rectifier (DF04M)
  - Supercapacitors (2× 5.5F 2.7V) + balancing resistors
  - Buck converters (LM2596, MP1584) or breakout modules
  - Ideal diode controller (LTC4412) or equivalent
- Two relay modules (door opener + gong, rated 3.3V/5V input)

### Software
- **Visual Studio Code** with **PlatformIO IDE extension**, or the PlatformIO CLI (`pio`)
- ESP32 platform installed via PlatformIO

---

## 🚀 Quick Start

### ESP-IDF Build (Recommended)
```bash
# Build and upload ESP-IDF firmware
pio run -t upload -e seeed_xiao_esp32s3_idf

# Monitor serial output
pio device monitor -e seeed_xiao_esp32s3_idf
```

### Arduino Build (Legacy)
```bash
# Build and upload Arduino firmware
pio run -t upload -e seeed_xiao_esp32s3

# Upload filesystem (LittleFS)
pio run -t uploadfs -e seeed_xiao_esp32s3
```

### VS Code
1. Open this folder in VS Code (PlatformIO will detect the project).
2. Select environment: `seeed_xiao_esp32s3_idf` (ESP-IDF) or `seeed_xiao_esp32s3` (Arduino)
3. Build: PlatformIO sidebar → Build
4. Upload: PlatformIO sidebar → Upload
5. Monitor: PlatformIO sidebar → Monitor

---

## 🔌 Wiring (Current Pin Map)

Summary (see `docs/esp32-s3-doorbell-architecture.md` for full diagram + steps):
- Doorbell button: GPIO4 → momentary switch → GND (active-low, internal pull-up)
- Status LED: GPIO2 → 330 ohm → LED → GND (active-high)
- I2S amp: GPIO7 = BCLK, GPIO8 = LRC/WS, GPIO9 = DIN, 3V3 + GND, SC tied to 3V3
- I2C reserved for sensors: GPIO5 = SDA, GPIO6 = SCL (add pull-ups when used)

Status LED is on when WiFi is connected (ready) and off during AP provisioning.

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

### ESP-IDF API Endpoints (New)
- `GET /` - Dashboard (STA mode) or redirect to setup (AP mode)
- `GET /api/status` - System status JSON (IP, RSSI, uptime, heap)
- `POST /api/wifi` - Save WiFi credentials (JSON: `{"ssid":"...","password":"..."}`)
- `DELETE /api/wifi` - Clear WiFi credentials and restart
- `POST /api/ota` - Firmware update (multipart/form-data)
- `GET /api/logs?filter=all|core|camera|doorbell` - Get logs as JSON
- `DELETE /api/logs` - Clear log buffer
- `GET /logs.html` - Web-based log viewer with filtering
- `GET /restart` - Styled restart page with auto-reconnect

### Legacy Endpoints (Arduino Compatibility)
- `GET /deviceStatus` - JSON status (RSSI + uptime)
- `GET /ring` - Trigger ring (SIP)
- `GET /ring/sip` - Trigger FRITZ!Box SIP ring
- `GET /sip` - SIP setup page
- `GET /sipDebug` - SIP debug JSON
- `GET /logs/camera` - Camera/streaming logs (legacy)
- `GET /logs/doorbell` - Doorbell/SIP logs (legacy)

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

If Scrypted shows an ffmpeg error about “Unable to choose an output format for rtsp://…”, ensure the URL is provided as an **input** (RTSP Camera plugin) or prefixed with `-i` in an FFmpeg Camera plugin.

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
4. Save WiFi credentials
5. After reboot, open `http://<device-ip>/` for the main UI
6. Verify camera and stream URLs, then add to Scrypted

### Troubleshooting
- If the stream does not load, confirm `http://<device-ip>:81/stream` is reachable and that port 81 is not blocked.
- If snapshots fail, try `http://<device-ip>/capture` first and check serial logs for camera init errors.
- If the camera fails to initialize, confirm the XIAO ESP32-S3 Sense pin map in `include/camera_pins.h`.
- If DECT phones do not ring, confirm the SIP target number and that the SIP account is active in FRITZ!Box.

### Web-Based Log Viewer
The ESP-IDF build includes a full-featured log viewer at `/logs.html`:
- **Category tabs**: All, Core, Camera, Doorbell
- **Level filter**: Error, Warn, Info, Debug, Verbose
- **Controls**: Sort order, font size, word wrap, auto-refresh
- **Actions**: Download logs, clear logs, refresh
- **Direct links**: `/logs.html?filter=core` for filtered views

### UI Assets
**ESP-IDF**: Web assets are embedded (gzip-compressed) in firmware. After editing files in `data/`:
```bash
python3 tools/embed_web_assets.py data include --idf
pio run -t upload -e seeed_xiao_esp32s3_idf
```

**Arduino**: UI stylesheet is served from LittleFS. After editing `data/style.css`:
```bash
pio run -t uploadfs -e seeed_xiao_esp32s3
```

### OTA Update Files
Use `tools/build_ota.py` to generate OTA `.bin` files in `dist/ota/`.
Full steps are in `docs/OTA_UPDATE_FILE.md`.

### Bill of Materials (BOM)
See `docs/PROJECT_BOM.md` for a comprehensive parts list with sourcing suggestions for the entire project, including the supercapacitor power supply.

---

## ⚙️ PlatformIO Configuration

Two environments are available in [platformio.ini](platformio.ini):

| Environment | Framework | Status |
|-------------|-----------|--------|
| `seeed_xiao_esp32s3_idf` | ESP-IDF | **Active** (recommended) |
| `seeed_xiao_esp32s3` | Arduino | Legacy |

### ESP-IDF Environment (Recommended)
```ini
[env:seeed_xiao_esp32s3_idf]
platform = espressif32
board = seeed_xiao_esp32s3
framework = espidf
board_build.flash_mode = qio
board_build.psram_mode = opi
```

### Arduino Environment (Legacy)
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

- **PSRAM enabled** via OPI mode for ESP32-S3
- **Flash**: 8 MB QIO mode

---

## 📁 Project Layout

```
├── src_idf/                    # ESP-IDF source (active)
│   ├── main/main.c             #   Entry point (5-step boot)
│   └── components/             #   IDF components
│       ├── nvs_manager/        #     NVS abstraction
│       ├── wifi_manager/       #     WiFi state machine
│       ├── web_server/         #     HTTP server + API
│       ├── dns_server/         #     Captive portal DNS
│       └── log_buffer/         #     Web log viewer backend
├── src/                        # Arduino source (legacy)
├── data/                       # Web UI assets (HTML, CSS)
├── include/                    # Headers + embedded assets
├── tools/                      # Build scripts
│   └── embed_web_assets.py     #   Gzip web assets for IDF
├── docs/                       # Documentation
└── platformio.ini              # Build configuration
```

See [docs/IDF_ADF_MIGRATION_PLAN.md](docs/IDF_ADF_MIGRATION_PLAN.md) for migration roadmap and [docs/IMPLEMENTATION_SUMMARY.md](docs/IMPLEMENTATION_SUMMARY.md) for current status.

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
