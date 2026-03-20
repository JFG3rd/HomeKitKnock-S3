<!--
 Project: HomeKitKnock-S3
 File: README.md
 Author: Jesse Greene
-->

# ESP32-S3 Sense DIY Video Doorbell

![License](https://img.shields.io/github/license/JFG3rd/HomeKitKnock-S3)

A **full-featured DIY video doorbell** built on the **XIAO ESP32-S3 Sense**, using pure **ESP-IDF** firmware. Streams video and audio to **Scrypted** for **HomeKit Secure Video** integration, rings your FRITZ!Box DECT phones via SIP, and drives your original doorbell gong through a relay.

**Status:** Phase 7 Complete — all core features working and verified on hardware.

---

## What's Built

- **Video streaming** — RTSP (port 8554) and HTTP MJPEG (port 81) with configurable resolution, quality, and frame overlays (timestamp, camera name)
- **Audio streaming** — AAC-LC audio over RTSP from INMP441 I2S microphone
- **SIP intercom** — registers with FRITZ!Box as an IP Door Intercom; bidirectional G.711 audio, DTMF door opener ("123" sequence)
- **Doorbell gong** — plays PCM chime over MAX98357A speaker + activates original 8VAC gong via relay
- **Door opener** — GPIO relay triggered by FRITZ!fon "Open" button (SIP INFO DTMF)
- **Scrypted + HomeKit** — RTSP camera feed + doorbell webhook for HomeKit Secure Video notifications
- **Web UI** — setup page for all settings, live A/V viewer, log viewer, OTA firmware updates
- **OTA updates** — password-protected, time-limited (5 min) upload window
- **WiFi provisioning** — captive portal AP mode on first boot

---

## Hardware

| Part | Purpose |
|------|---------|
| Seeed Studio **XIAO ESP32-S3 Sense** | Camera + MCU (8MB Flash, 8MB PSRAM) |
| **INMP441** I2S MEMS microphone | Audio capture for RTSP + SIP |
| **MAX98357A** I2S DAC/Amp + small speaker | Doorbell chime + SIP audio playback |
| Doorbell push button | GPIO4 to GND (active-low, internal pull-up) |
| Status LED + 330 ohm resistor | GPIO2, shows connection/ring status |
| 2x relay modules (3.3V logic) | GPIO3 = gong relay, GPIO1 = door opener |
| 8VAC transformer + supercap power supply | Powers from existing doorbell wiring |

See [docs/PROJECT_BOM.md](docs/PROJECT_BOM.md) for the full parts list and [docs/POWER_SUPPLY_DESIGNS.md](docs/POWER_SUPPLY_DESIGNS.md) for the power supply schematic.

---

## Wiring

| GPIO | Function | Notes |
|------|----------|-------|
| GPIO1 | Door opener relay | Active-high, triggered by DTMF "123" from FRITZ!fon |
| GPIO2 | Status LED | PWM via LEDC, 330 ohm to LED |
| GPIO3 | Gong relay | Active-high, 150ms startup delay + 800ms pulse |
| GPIO4 | Doorbell button | Active-low, internal pull-up, 50ms debounce |
| GPIO5 | INMP441 SD (mic data) | **Do not use GPIO12** — camera Y7 data pin |
| GPIO7 | Shared I2S BCLK | MAX98357A + INMP441 (shared clock) |
| GPIO8 | Shared I2S WS | MAX98357A + INMP441 (shared word select) |
| GPIO9 | I2S TX data | MAX98357A DIN (speaker output) |
| GPIO10-18, 38-40, 47-48 | OV2640 camera | See `include/camera_pins.h` |
| GPIO41/42 | Onboard PDM mic | Built into XIAO Sense board; INMP441 is preferred |

Full wiring diagram: [docs/WIRING_DIAGRAM.md](docs/WIRING_DIAGRAM.md)

---

## Quick Start

### Build and Flash

```bash
# Clone and open in VS Code with PlatformIO
pio run -t upload -e seeed_xiao_esp32s3_idf

# Monitor serial output
pio device monitor -e seeed_xiao_esp32s3_idf
```

### First Boot

1. Connect to the `ESP32_Doorbell_Setup` WiFi network
2. The captive portal opens automatically — enter your WiFi credentials
3. After reboot, open `http://<device-ip>/` in your browser
4. You'll be prompted to create a device password on first visit

### OTA Firmware Updates

1. Go to `/setup` and click **Enable OTA (5 min)**
2. Click **Open OTA Page**
3. Upload your `firmware.bin` file

### Updating Web UI

If you edit files in `data/`, regenerate the embedded assets before building:

```bash
python3 tools/embed_web_assets.py data include
pio run -t upload
```

---

## Streaming URLs

| Protocol | URL | Use |
|----------|-----|-----|
| RTSP | `rtsp://<device-ip>:8554/mjpeg/1` | Scrypted, VLC, ffplay |
| HTTP MJPEG | `http://<device-ip>:81/stream` | Browser preview |
| Snapshot | `http://<device-ip>/capture` | Single JPEG frame |
| Live A/V | `http://<device-ip>/live` | Browser audio + video page |

---

## Scrypted + HomeKit Setup

1. Install the **FFmpeg Camera** plugin in Scrypted
2. Add a new FFmpeg Camera device with RTSP URL: `rtsp://<device-ip>:8554/mjpeg/1`
3. Set the **FFmpeg Output Prefix** (required for MJPEG → H.264 conversion):
   ```
   -c:v libx264 -pix_fmt yuvj420p -preset ultrafast -bf 0 -g 60 -r 15 -b:v 500000 -bufsize 1000000 -maxrate 500000
   ```
4. Create a Doorbell device and link the camera
5. Enable HomeKit in Scrypted and pair with Apple Home

Full guide: [docs/SCRYPTED_RTSP_SETUP.md](docs/SCRYPTED_RTSP_SETUP.md)

---

## FRITZ!Box SIP Setup

1. FRITZ!Box UI → Telephony → Telephony Devices → Add new device
2. Select **IP Door Intercom System** (Tursprechanlage)
3. Assign username/password and configure Ring Key 1 to your phone group
4. Enter SIP credentials on the ESP32 setup page (`/setup`)
5. The door opener sequence is `123` — sent when you press "Open" on FRITZ!fon

See [docs/SIP_INTEGRATION.md](docs/SIP_INTEGRATION.md) for detailed SIP flow documentation.

---

## Web Interface Pages

| Page | URL | Purpose |
|------|-----|---------|
| Dashboard | `/` | System status overview |
| Setup | `/setup` | All device settings, features, camera config |
| Live View | `/live` | Video + audio preview in browser |
| Logs | `/logs.html` | Filterable log viewer (Core, Camera, Doorbell) |
| OTA | `/ota` | Firmware upload (after enabling in Setup) |
| WiFi Setup | `/wifi-setup.html` | WiFi provisioning (AP mode) |
| Guide | `/guide` | User guide |

---

## Project Layout

```
├── src_idf/                        # ESP-IDF source
│   ├── main/main.c                 #   Entry point, boot sequence, main loop
│   └── components/                 #   21 IDF components
│       ├── nvs_manager/            #     NVS key-value storage
│       ├── wifi_manager/           #     WiFi STA/AP/APSTA modes
│       ├── web_server/             #     HTTP server + REST API
│       ├── dns_server/             #     Captive portal DNS
│       ├── log_buffer/             #     Ring buffer log backend
│       ├── config_manager/         #     Typed config storage
│       ├── sip_client/             #     SIP state machine + RTP audio
│       ├── button/                 #     Doorbell button (GPIO4)
│       ├── status_led/             #     PWM LED patterns (GPIO2)
│       ├── relay_controller/       #     Gong (GPIO3) + door opener (GPIO1)
│       ├── camera/                 #     OV2640 driver + NVS config
│       ├── mjpeg_server/           #     MJPEG HTTP streaming (port 81)
│       ├── rtsp_server/            #     RTSP server (port 8554)
│       ├── audio_output/           #     MAX98357A speaker + gong playback
│       ├── audio_capture/          #     INMP441 mic capture
│       ├── i2s_shared_bus/         #     Full-duplex I2S shared bus
│       ├── aac_encoder_pipe/       #     AAC-LC encoder for RTSP audio
│       ├── timestamp_overlay/      #     JPEG timestamp + camera name overlay
│       ├── embedded_web_assets/    #     Gzip web asset registry
│       ├── adf_codec/              #     ESP-ADF codec wrapper
│       └── adf_pipeline/           #     ESP-ADF pipeline wrapper
├── data/                           # Web UI source files (HTML, CSS)
├── include/                        # Headers + embedded gzip assets
├── tools/                          # Build scripts
│   ├── embed_web_assets.py         #   Gzip web assets → C headers
│   └── build_ota.py                #   Generate OTA .bin files
├── docs/                           # Documentation
└── platformio.ini                  # Build configuration
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [Architecture](docs/esp32-s3-doorbell-architecture.md) | System design, hardware, event flow |
| [Migration Plan](docs/IDF_ADF_MIGRATION_PLAN.md) | Development phases 0-7 with session logs |
| [Scrypted Setup](docs/SCRYPTED_RTSP_SETUP.md) | Camera + doorbell integration guide |
| [Wiring Diagram](docs/WIRING_DIAGRAM.md) | Full schematic-style wiring map |
| [GPIO Map](docs/GPIO_MAP.md) | Pin assignments and availability |
| [BOM](docs/PROJECT_BOM.md) | Parts list with sourcing |
| [Power Supply](docs/POWER_SUPPLY_DESIGNS.md) | Supercapacitor power circuit |
| [SIP Integration](docs/SIP_INTEGRATION.md) | SIP protocol flow and FRITZ!Box setup |
| [OTA Guide](docs/OTA_UPDATE_FILE.md) | OTA firmware image creation |
| [Audio Integration](docs/AUDIO_INTEGRATION.md) | Mic + speaker paths and tuning |

---

## Troubleshooting

- **No video in Scrypted**: Check that the FFmpeg Output Prefix is set (see Scrypted setup above). Without it, MJPEG won't convert to H.264 for HomeKit.
- **Camera not initializing**: Check serial logs for OV2640 errors. Ensure the Sense expansion board is seated properly.
- **DECT phones don't ring**: Verify SIP credentials in `/setup` and that the FRITZ!Box IP phone is active.
- **10+ second latency**: Almost always a Scrypted FFmpeg setting issue. Restore `-bufsize 1000000 -g 60` in the Output Prefix.
- **No mic audio**: Verify INMP441 is wired to GPIO5 (not GPIO12). Use "Record & Play" in `/setup` to test.

---

## Notes

- Web assets are embedded as gzip C arrays in firmware — no filesystem partition needed
- Only `firmware.bin` is required for OTA updates
- Factory reset: hold doorbell button 5s → release → hold 5s again → NVS erased, device reboots

---

## License

Apache-2.0. See `LICENSE`.
