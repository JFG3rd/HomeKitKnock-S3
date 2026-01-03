# ESP32-S3 Sense Doorbell Firmware (PlatformIO)

![License](https://img.shields.io/github/license/JFG3rd/HomeKitKnock-S3)

Firmware for the **ESP32-S3 Sense–based DIY video doorbell**, built with **PlatformIO (Arduino framework)**. Streams camera video to **Scrypted** and triggers **HomeKit doorbell events** via HTTP.

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
- `GET /style.css` - UI styles (served inline)

### Normal Mode Endpoints
- `GET /` - Device status page
- `GET /forget` - Clears WiFi credentials and restarts

### TR-064 Settings
The setup page lets you configure FRITZ!Box TR-064 credentials and the internal
ring number (e.g., `**9` or a group extension). These settings are stored in NVS.
The router IP is derived from the WiFi gateway after the device connects.

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
