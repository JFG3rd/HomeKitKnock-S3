<!--
 Project: HomeKitKnock-S3
 File: docs/esp32-s3-doorbell-architecture.md
 Author: Jesse Greene
 -->

📄 docs/esp32-s3-doorbell-architecture.md

ESP32-S3 Sense → Scrypted → HomeKit Doorbell

Project Notes & Architecture Overview

🎯 Goal

Build a DIY Audio/Video doorbell using:
	•	Seeed Studio XIAO ESP32-S3 Sense
	•	Scrypted (running on Raspberry Pi 5 NVR server)
	•	HomeKit Secure Video doorbell integration

Objectives:
	•	Stream video + audio from ESP32-S3 to Scrypted (RTSP), with HTTP MJPEG + AAC for browser preview
	•	Trigger doorbell rings using a physical button → GPIO → HTTP webhook
	•	Trigger FRITZ!Box SIP internal ring for a DECT phone group
	•	Let Apple’s Home app handle:
	•	Doorbell notifications
	•	Live stream view with audio
	•	HSV event history & previews

Scrypted acts as the bridge + NVR, while the ESP32 device is the camera + doorbell trigger.

⸻

🧩 System Architecture

Per-door device (Front Door / Gate)

Component                       Role
ESP32-S3 Sense                  Camera + button input + HTTP webhook
FRITZ!Box (SIP)                 Internal call trigger to DECT phones
FRITZ!DECT phones               Audible ring with custom ringtone
Scrypted Camera Device          Receives video stream
Scrypted Doorbell Group         Combines camera + button
HomeKit Doorbell                Exposed via Scrypted HomeKit plugin


Doorbell event flow:
	1.	Physical button press
	2.	ESP32 detects GPIO edge
	3.  ESP32 Triggers **GPIO3** -> Original gong relay IN (.8s high then back to low)
	3.  ESP32 plays gong.pcm over MAX98357A DAC/Amp to speaker
	4.	ESP32 performs HTTP GET to Scrypted doorbell endpoint
	5.	ESP32 triggers FRITZ!Box SIP internal ring (DECT group)
	6.	Scrypted fires HomeKit doorbell event
	7.	Apple devices display doorbell notification + live video

⸻

🍏 What HomeKit Displays

When the doorbell endpoint is triggered:
	•	iPhone / iPad:
	•	“Doorbell — Front Door”
	•	Snapshot preview (if enabled)
	•	Tap → Live stream with audio
	•	Apple Watch:
	•	Thumbnail notification + “View”
	•	Apple TV:
	•	Picture-in-picture popup option

HomeKit Secure Video analysis and recording (person/package/etc.)
is handled by the Home hub (Apple TV / HomePod), not by ESP32 or Scrypted.

⸻

🛠 Development Environment

Preferred workflow:
	•	VS Code
	•	PlatformIO
	•	Framework: ESP-ADF (inside PlatformIO)

Board:

seeed_xiao_esp32s3

Key PlatformIO flags:
	•	Enable PSRAM
	•	Use qio_opi memory mode

Used with:
	•	✅ RTSP streaming (Phase 1 - COMPLETE, includes audio)
	•	✅ MJPEG HTTP streaming (Phase 1 - COMPLETE, companion AAC audio)
	•	Later upgrade to H.264 + WebRTC/two-way audio (Phase 2)

⸻

🧱 Phase 1 — MVP Implementation ✅ COMPLETE (Arduino)

**Note:** This phase was completed with the Arduino framework. The project is now migrating to pure ESP-IDF for better stability and control. See ESP-IDF Migration Status below.

Focus:
	•	✅ Video stream including audio using Onboard digital microphone (XIAO ESP32-S3 Sense) to Scrypted via RTSP
	•	✅ Doorbell button → HAP HomeKit ring
	•	✅ FRITZ!Box internal phone ring via SIP

Components:
	•	ESP32-S3 Sense running:
	•	✅ RTSP server (port 8554) for Scrypted
	•	✅ MJPEG HTTP stream (port 81) for browser
	•	✅ RTSP audio (AAC-LC) from onboard mic
	•	✅ HTTP AAC audio stream (port 81) for MJPEG companion audio
	•	✅ Button GPIO input (debounced)
	•	✅ SIP client for FRITZ!Box IP-phone registration (Digest auth)
	•	✅ SIP INVITE/CANCEL for ringing internal phones
	•	✅ SIP RTP audio (PCMU/PCMA) with DTMF door opener support
	•	✅ Configuration storage in NVS (WiFi, SIP, camera)
	•	✅ Web UI for setup and testing

	•	Scrypted:
	•	Add ESP32 as RTSP camera device
	•	Create Doorbell Group
	•	Export to HomeKit

Expected result:
	•	✅ Doorbell notification appears on Apple devices
	•	✅ Live audio/video stream plays in Home app (via RTSP and Scrypted)
	•	✅ FRITZ!Box DECT phones ring when button pressed
	•	✅ Event appears in HSV timeline (if enabled)

Audio streaming is now implemented for RTSP (AAC-LC) and a companion HTTP AAC stream. Use RTSP for Scrypted/HomeKit; the HTTP stream is for local browser/tools. SIP intercom audio still uses PCMU/PCMA. Advanced A/V sync and two-way audio remain Phase 2.

⸻

🔧 ESP-IDF Migration Status

The project is migrating from Arduino to pure ESP-IDF for better reliability and control.

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 0 | ✅ Complete | Pre-migration hygiene |
| Phase 1 | ✅ Complete | IDF base (boot, NVS, WiFi, web) |
| Phase 2 | ✅ Complete | Captive portal, log viewer, config |
| Phase 3 | ✅ Complete | SIP client, button, LED, SNTP |
| Phase 4 | ✅ Complete | Video path — camera, MJPEG, RTSP |
| Phase 5 | ✅ Complete | Audio path — speaker + INMP441 mic fully working (Record & Play verified) |
| Phase 6 | ❌ Pending | HomeKit doorbell integration |
| Phase 7 | ❌ Pending | OTA update system (credentials, time-limited window) |
| Phase 8 | ❌ Pending | Cleanup & resilience |

**ESP-IDF Components (src_idf/components/):**
- `nvs_manager` - NVS abstraction
- `wifi_manager` - WiFi STA/AP/APSTA modes
- `web_server` - HTTP server + REST API
- `dns_server` - Captive portal DNS
- `log_buffer` - Ring buffer with timestamps
- `sip_client` - SIP state machine + RTP
- `button` - Doorbell button (GPIO4)
- `status_led` - PWM LED patterns (GPIO2)
- `camera` - OV2640 driver + NVS config (Phase 4)
- `mjpeg_server` - MJPEG HTTP streaming port 81 (Phase 4)
- `rtsp_server` - RTSP server port 8554 (Phase 4)
- `audio_output` - MAX98357A speaker, gong, volume (Phase 5)
- `audio_capture` - INMP441 mic capture via shared I2S bus (Phase 5)
- `i2s_shared_bus` - Full-duplex I2S_NUM_1 shared channel manager (Phase 5)
- `aac_encoder_pipe` - AAC-LC encoder for RTSP audio (Phase 5)

⸻

🔊 Phase 2 — Advanced A/V + Two-Way Audio (Planned)

Goal:
	•	Improve A/V sync, codec efficiency, and enable talk-back audio

Approach:
	•	Evaluate H.264 + WebRTC pipeline for two‑way A/V
	•	Keep ESP‑ADF for AAC encode/decode and future audio processing
	•	Add two-way audio (speaker + mic) and echo handling

⸻

🔉 Audio Hardware (Current)

Active hardware:
	•	INMP441 external I2S microphone (required wiring — see WIRING_DIAGRAM.md)
	•	MAX98357A I2S DAC amp
	•	Small speaker (doorbell chime + local monitoring)

Notes:
	•	Audio uses native ESP-IDF I2S drivers (no ESP-ADF) for all active paths (speaker, capture, Record & Play)
	•	`aac_encoder_pipe` will use ESP-ADF `adf_pipeline`/`adf_codec` for future RTSP AAC streaming (not yet wired)
	•	**Required mic**: INMP441 external I2S mic (GPIO7=SCK, GPIO8=WS, **GPIO5=SD**, GND=L/R)
	•	**GPIO12 MUST NOT be used for INMP441 SD** — GPIO12 = OV2640 camera Y7 data output; camera chip drives it regardless of software state
	•	**Onboard PDM mic** (GPIO41=DATA, GPIO42=CLK): physically integrated on the XIAO ESP32-S3 Sense PCB by Seeedstudio; available as a software-selectable source but INMP441 is the preferred mic for this project
	•	**INMP441 must NOT be on GPIO43/44** — those pins are free for UART use
	•	MAX98357A I2S DAC: GPIO7 = BCLK, GPIO8 = LRCLK/WS, GPIO9 = DIN (I2S_NUM_1 TX)
	•	GPIO7/8 BCLK/WS are physically shared between MAX98357A and INMP441 (`i2s_shared_bus` component)
	•	ESP-IDF STD I2S RX returns stereo-interleaved DMA data [L,R,L,R,...] even in MONO mode; `audio_capture_read()` de-interleaves to extract L channel
	•	MAX98357A SC/SD: tie to 3V3 (always on)
	•	Feature setup exposes mic enable/mute + sensitivity, AAC sample-rate/bitrate, and audio out enable/mute + volume
	•	Browser A/V page: http://ESP32-IP/live
	•	SIP intercom audio: RTP on UDP port 40000 (PCMU/PCMA + DTMF)
	•	Local gong playback: embedded PCM data in flash (gong_data.c, generated from data/gong.pcm)

⸻

🔌 Current Wiring (Rev A)

Pin assignments (current):
	•	Doorbell button: GPIO4 (active-low, internal pull-up)
	•	Status LED (online/ready): GPIO2 (active-high) + 330 ohm resistor
	•	Door opener relay: GPIO1 (active-high, relay module or transistor driver)
	•	Original 8VAC gong relay: GPIO3 (active-high, relay module rated for AC)
	•	I2C (reserved for sensors): GPIO5 = SDA, GPIO6 = SCL
	•	MAX98357A I2S: GPIO7 = BCLK, GPIO8 = LRC/WS, GPIO9 = DIN
	•	PDM mic: GPIO42 = CLK, GPIO41 = DATA (onboard Seeedstudio hardware; INMP441 is the active mic source in this project)
	•	Camera pins: see `include/camera_pins.h` (XIAO ESP32-S3 Sense map)
	•	INMP441 I2S Microphone (external, offboard): GPIO7 = SCK, GPIO8 = WS, GPIO5 (D4) = SD, GND = L/R
Power supply:
	•	8VAC transformer (existing doorbell transformer or similar) → bridge rectifier → supercapacitor ride-through → 3.3V buck
	•	Supercapacitor bank provides ~45 second hold-up during gong relay activation
	•	Full schematic and BOM in `docs/POWER_SUPPLY_DESIGNS.md`

Status LED behavior (priority order, highest first):
	•	Ringing: breathing animation (1.4s period, 6s duration) ✅ Implemented
	•	AP mode: fast double blink (1s period) ✅ Implemented
	•	WiFi connecting: 2 Hz blink (500ms period) ✅ Implemented
	•	SIP error: slow pulse (2s period) ✅ Implemented
	•	SIP ok: steady low glow (duty 24/255) ✅ Implemented
	•	RTSP active: short tick every 2 seconds (future)

LED status codes (summary):
	•	Double‑blink = AP provisioning mode (no saved Wi‑Fi credentials or AP mode forced).
	•	Breathing = doorbell ringing active (triggered by button or web test).
	•	Steady low glow = SIP registered and idle.
	•	Short tick = RTSP session active (overlaid on SIP OK) — not yet implemented.
	•	2 Hz blink = Wi‑Fi connect in progress.
	•	Slow pulse = SIP error (registration failed or timed out).

**Implementation:** `src_idf/components/status_led/` - PWM via LEDC, 8-bit resolution, 5kHz.

MAX98357A pin order (left → right): LRC, BCLK, DIN, GAIN, SC, GND, Vin

Wiring diagram:
- See `docs/WIRING_DIAGRAM.md` for the full schematic-style wiring map.

Build steps (soldering + wiring):
	1.	Solder headers on the XIAO ESP32-S3 Sense and mount it securely.
	2.	Doorbell switch: connect one leg to GPIO4 and the other to GND (internal pull-up is enabled in firmware).
	3.	Status LED: connect GPIO2 → 330 ohm resistor → LED anode; LED cathode to GND.
	4.	Door opener relay: GPIO1 → relay IN, relay VCC → 3V3 (or 5V module with 3.3V logic), relay GND → GND.
	5.	MAX98357A: wire LRC→GPIO8, BCLK→GPIO7, DIN→GPIO9, GND→GND, Vin→3V3.
	6.	MAX98357A SC: tie to 3V3 for always-on.
	7.	MAX98357A GAIN: leave floating for default gain (or strap per datasheet).
	8.	Speaker: connect to MAX98357A L+ and L- (do not connect either side to GND).
	9.	Reserve GPIO5/6 for future I2C sensors; add pull-ups when you install sensors.

DTMF door opener sequence default: `123` (configurable in `include/config.h`).

⸻

🔌 Accessible Header GPIOs (XIAO ESP32-S3 Sense)

Full header mapping and usage notes live in `docs/GPIO_MAP.md`.

Header mapping (from `pins_arduino.h` in the PlatformIO variant):
	•	D0/A0 = GPIO1
	•	D1/A1 = GPIO2
	•	D2/A2 = GPIO3
	•	D3/A3 = GPIO4
	•	D4/A4 = GPIO5
	•	D5/A5 = GPIO6
	•	D8/A8 = GPIO7
	•	D9/A9 = GPIO8
	•	D10/A10 = GPIO9

Reserved/used by this project:
	•	GPIO2: status LED
	•	GPIO4: doorbell button
	•	GPIO5 (D4): INMP441 SD (mic data in) — **was: I2C SDA placeholder; now used for mic**
	•	GPIO6: I2C SCL (reserved for sensors, not populated)
	•	GPIO7/8: shared I2S clocks (MAX98357A + INMP441)
	•	GPIO9: I2S DOUT to MAX98357A DIN
	•	GPIO12: OV2640 camera Y7 data — **DO NOT use for INMP441** (camera chip drives this pin)
	•	GPIO41/42: PDM mic (onboard Seeedstudio hardware; INMP441 is active mic source)
	•	Camera pins: GPIO10/11/12/13/14/15/16/17/18/38/39/40/47/48 (see `include/camera_pins.h`)

Free header GPIOs with current wiring:
	•	GPIO1 (D0/A0) - safe for digital/analog input or WS2812 data
	•	GPIO3 (D2/A2) - safe for digital/analog input or simple PWM output
	•	GPIO43/GPIO44 (D6/D7) - free (not used for INMP441 in required wiring)

Suggested usage:
	•	WS2812B or status LED data line: GPIO1 or GPIO3
	•	PIR/door contact sensor input: GPIO1 or GPIO3
	•	I2C sensors: GPIO5/6 when you decide to populate the I2C header
	•	External UART device (RS485/GPS): GPIO43/44

Note: The onboard LED is GPIO21 (not on the header). Use it only if you want a dedicated internal status LED.

⸻

🧠 Core & Task Strategy (Stability + Future Audio)

Guiding principle:
	•	Keep Wi-Fi/LwIP on core 0
	•	Pin streaming + future audio tasks to core 1

Current direction:
	•	MJPEG stream server tasks pinned to core 1
	•	RTSP handling runs on core 1 task
	•	Main loop remains lightweight (SIP, button debounce)

Current audio plan:
	•	I2S mic capture + DAC playback on core 1
	•	AAC encode tasks pinned to core 1 via ESP‑ADF
	•	Avoid heavy CPU work on Wi‑Fi core to reduce jitter

⸻

🧷 Doorbell Button ✅ Implemented

**Current Implementation (ESP-IDF):**
- GPIO4, active-low with internal pull-up
- 50ms debounce in software (polling-based)
- Callback triggers SIP ring + LED animation + plays gong sound or two tone sound over MAX98357A DAC/Amp and connected speaker.

- Component: `src_idf/components/button/`

Two supported wiring strategies:
	1.	Parallel AC detector module
	•	Non-invasive
	•	ESP reads isolated digital output
	•	Gong wiring remains unchanged
	2.	Button as dry contact → ESP → relay drives gong
	•	More control
	•	Allows smart gong behaviour
	•	Requires mild rewiring



🖥 UI Diagnostics

Main UI now includes metrics + diagnostic actions:
	•	RTSP sessions, HTTP clients, UDP endPacket fail count
	•	UDP backoff state (active + remaining ms)
	•	Buttons to reset UDP fail counter and clear backoff state
	•	Separate log pages: `/logs/camera` and `/logs/doorbell`

⸻

🔗 Integration Responsibilities

Responsibility                                 Owner
Camera streaming                                 ESP32-S3
Doorbell trigger                                 ESP32-S3 HTTP webhook
DECT ring trigger                                 ESP32-S3 SIP call
AV transport                                 Scrypted
NVR storage                                 Scrypted
HomeKit bridge                                 Scrypted HomeKit plugin
HSV analytic                                 Apple Home hub

Frigate / Hailo is not part of this project phase.

⸻

🧭 Next Implementation Steps (Phase 6 — HomeKit)

Phase 5 is complete. All audio I/O working end-to-end (Record & Play verified Feb 2026).

Phase 6 (HomeKit):
	1.	Integrate Espressif HAP SDK or Scrypted bridge for HomeKit doorbell event

Phase 5 remaining audio stretch goals:
	2.	SIP bidirectional audio (G.711 RTP TX/RX) — mic capture now unblocked
	3.	RTSP audio (AAC-LC via `aac_encoder_pipe` + ESP-ADF) — mic capture now unblocked

⸻

📝 Open Questions / To-Do
	•	Select final doorbell button sensing scheme:
	•	AC detector vs dry contact + relay
	•	Confirm I2C sensor selection + pull-up values (GPIO6=SCL available; GPIO5 now INMP441 SD)
	•	Confirm DECT group number and FRITZ!Box SIP account settings
	•	Tune AAC sample-rate/bitrate defaults for best quality vs bandwidth
	•	SIP bidirectional audio (mic now working — RTP TX path to implement)
	•	RTSP AAC audio stream (wire `aac_encoder_pipe` → RTSP server)
	•	Confirm speaker power + enclosure placement
	•	Evaluate latency + HomeKit experience
	•	Phase 6: HomeKit doorbell integration via HAP SDK or Scrypted bridge

⸻

👤 Maintainer Notes

This project is designed to integrate with:
	•	Raspberry Pi 5 Scrypted NVR cluster
	•	Future Frigate/Hailo AI event engine (optional)
	•	Existing HomeKit smart home environment

This document should evolve along with:
	•	wiring decisions
	•	firmware iterations
	•	Scrypted configuration changes

⸻

📚 Documentation Index (docs/)
	•	`docs/QUICK_START.md` — fast setup checklist + key URLs.
	•	`docs/esp32-s3-doorbell-architecture.md` — system overview and design notes.
	•	`docs/GPIO_MAP.md` — GPIO availability and reserved pins.
	•	`docs/WIRING_DIAGRAM.md` — wiring map and relay/sensor hookups.
	•	`docs/PROJECT_BOM.md` — parts list.
	•	`docs/POWER_SUPPLY_DESIGNS.md` — power options and schematic notes.
	•	`docs/AUDIO_INTEGRATION.md` — mic + speaker path, formats, and tuning.
	•	`docs/SIP_INTEGRATION.md` — SIP flow, authentication, and RTP notes.
	•	`docs/SIP-Fritzbox JSON Spec.md` — structured SIP/Fritz!Box config model.
	•	`docs/ESP32-Fritzbox-SIP-Documentation.md` — SIP interoperability notes.
	•	`docs/SCRYPTED_RTSP_SETUP.md` — Scrypted camera setup guidance.
	•	`docs/OTA_UPDATE_FILE.md` — OTA image creation and naming.
	•	`docs/UPDATING_WEB_INTERFACE.md` — LittleFS UI update workflow.
	•	`docs/IMPLEMENTATION_SUMMARY.md` — status and implementation notes.
	•	`docs/Mermaid SIP Sequence Diagram.md` — SIP message flow diagram.
	•	`docs/Mermaid Timing Diagram — SIP Message Timing & Retransmissions.md` — SIP timers and retries.
	•	`docs/Mermaid Class Diagram — ESP32 SIP Client Architecture.md` — SIP client structure.
	•	`docs/ Mermaid Flowchart — Digest Authentication Logic.md` — digest auth flow.
