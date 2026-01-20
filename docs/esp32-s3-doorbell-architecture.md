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
	•	Stream video + audio from ESP32-S3 to Scrypted (RTSP), with HTTP MJPEG + WAV for browser preview
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
	3.	ESP32 performs HTTP GET to Scrypted doorbell endpoint
	4.	ESP32 triggers FRITZ!Box SIP internal ring (DECT group)
	5.	Scrypted fires HomeKit doorbell event
	6.	Apple devices display doorbell notification + live video

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
	•	Framework: Arduino (inside PlatformIO)

Board:

seeed_xiao_esp32s3

Key PlatformIO flags:
	•	Enable PSRAM
	•	Use qio_opi memory mode

Used with:
	•	✅ RTSP streaming (Phase 1 - COMPLETE, includes audio)
	•	✅ MJPEG HTTP streaming (Phase 1 - COMPLETE, companion WAV audio)
	•	Later upgrade to H.264 + WebRTC/two-way audio (Phase 2)

⸻

🧱 Phase 1 — MVP Implementation ✅ COMPLETE

Focus:
	•	✅ Video stream including audio using Onboard digital microphone (XIAO ESP32-S3 Sense) to Scrypted via RTSP
	•	✅ Doorbell button → HomeKit ring
	•	✅ FRITZ!Box internal phone ring via SIP

Components:
	•	ESP32-S3 Sense running:
	•	✅ RTSP server (port 8554) for Scrypted
	•	✅ MJPEG HTTP stream (port 81) for browser
	•	✅ RTSP audio (PCMU) from onboard mic
	•	✅ HTTP WAV audio stream (port 81) for MJPEG companion audio
	•	✅ Button GPIO input (debounced)
	•	✅ SIP client for FRITZ!Box IP-phone registration (Digest auth)
	•	✅ SIP INVITE/CANCEL for ringing internal phones
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

Audio streaming is now implemented for RTSP (PCMU) and a companion HTTP WAV stream. Use RTSP for Scrypted/HomeKit; the HTTP stream is for local browser/tools. Advanced A/V sync and two-way audio remain Phase 2.

⸻

🔊 Phase 2 — Advanced A/V + Two-Way Audio (Planned)

Goal:
	•	Improve A/V sync, codec efficiency, and enable talk-back audio

Approach:
	•	Evaluate H.264 + AAC over RTSP or a WebRTC pipeline
	•	Consider ESP-IDF / ESP-ADF for an integrated A/V pipeline
	•	Add two-way audio (speaker + mic) and echo handling

⸻

🔉 Audio Hardware (Current)

Active hardware:
	•	Onboard digital microphone (XIAO ESP32-S3 Sense)
	•	MAX98357A I2S DAC amp
	•	Small speaker (doorbell chime + local monitoring)

Notes:
	•	Audio path will be I2S in/out (mic in, DAC out)
	•	Onboard PDM mic: GPIO42 = CLK, GPIO41 = DATA (I2S0 RX)
	•	MAX98357A I2S DAC: GPIO7 = BCLK, GPIO8 = LRCLK/WS, GPIO9 = DIN (I2S1 TX)
	•	MAX98357A SC/SD: tie to 3V3 (always on)
	•	GPIO7/8/9 are default SPI pins; avoid SPI on those pins or remap if needed
	•	Feature setup exposes mic enable/mute + sensitivity and audio out enable/mute + volume
	•	HTTP audio preview: http://ESP32-IP/audio.wav
	•	Continuous HTTP audio (MJPEG companion): http://ESP32-IP:81/audio
	•	Browser A/V page: http://ESP32-IP/live
	•	Local gong playback uses `/gong.pcm` from LittleFS when present

⸻

🔌 Current Wiring (Rev A)

Pin assignments (current):
	•	Doorbell button: GPIO4 (active-low, internal pull-up)
	•	Status LED (online/ready): GPIO2 (active-high) + 330 ohm resistor
	•	I2C (reserved for sensors): GPIO5 = SDA, GPIO6 = SCL
	•	MAX98357A I2S: GPIO7 = BCLK, GPIO8 = LRC/WS, GPIO9 = DIN
	•	PDM mic: GPIO42 = CLK, GPIO41 = DATA
	•	Camera pins: see `include/camera_pins.h` (XIAO ESP32-S3 Sense map)

Status LED behavior (priority):
	•	Ringing: breathing (dim in/out)
	•	AP mode: fast double blink
	•	WiFi connecting: 2 Hz blink
	•	SIP error: slow pulse
	•	SIP ok: steady low glow
	•	RTSP active: short tick every 2 seconds

MAX98357A pin order (left → right): LRC, BCLK, DIN, GAIN, SC, GND, Vin

Wiring diagram:
- See `docs/WIRING_DIAGRAM.md` for the full schematic-style wiring map.

Build steps (soldering + wiring):
	1.	Solder headers on the XIAO ESP32-S3 Sense and mount it securely.
	2.	Doorbell switch: connect one leg to GPIO4 and the other to GND (internal pull-up is enabled in firmware).
	3.	Status LED: connect GPIO2 → 330 ohm resistor → LED anode; LED cathode to GND.
	4.	MAX98357A: wire LRC→GPIO8, BCLK→GPIO7, DIN→GPIO9, GND→GND, Vin→3V3.
	5.	MAX98357A SC: tie to 3V3 for always-on.
	6.	MAX98357A GAIN: leave floating for default gain (or strap per datasheet).
	7.	Speaker: connect to MAX98357A L+ and L- (do not connect either side to GND).
	8.	Reserve GPIO5/6 for future I2C sensors; add pull-ups when you install sensors.

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
	•	D6 = GPIO43 (UART TX)
	•	D7 = GPIO44 (UART RX)
	•	D8/A8 = GPIO7
	•	D9/A9 = GPIO8
	•	D10/A10 = GPIO9

Reserved/used by this project:
	•	GPIO2: status LED
	•	GPIO4: doorbell button
	•	GPIO5/GPIO6: I2C (reserved)
	•	GPIO7/8/9: I2S DAC (audio out)
	•	GPIO41/42: PDM mic (onboard)
	•	Camera pins: GPIO10/11/12/13/14/15/16/17/18/38/39/40/47/48 (see `include/camera_pins.h`)

Free header GPIOs with current wiring:
	•	GPIO1 (D0/A0) - safe for digital/analog input or WS2812 data
	•	GPIO3 (D2/A2) - safe for digital/analog input or simple PWM output
	•	GPIO43/GPIO44 (D6/D7) - available if you are not using UART TX/RX

Suggested usage:
	•	WS2812B or status LED data line: GPIO1 or GPIO3
	•	PIR/door contact sensor input: GPIO1 or GPIO3
	•	I2C sensors: GPIO5/6 when you decide to populate the I2C header
	•	External UART device (RS485/GPS): GPIO43/44 if you do not need UART for debugging

Note: The onboard LED is GPIO21 (not on the header). Use it only if you want a dedicated internal status LED.

⸻

🧠 Core & Task Strategy (Stability + Future Audio)

Guiding principle:
	•	Keep Wi-Fi/LwIP on core 0
	•	Pin streaming + future audio tasks to core 1

Current direction:
	•	MJPEG stream server tasks pinned to core 1
	•	RTSP handling runs on core 1 task
	•	Main loop remains lightweight (SIP/TR-064, button debounce)

Future audio plan:
	•	I2S mic capture + DAC playback on core 1
	•	Avoid heavy CPU work on Wi-Fi core to reduce jitter

⸻

🧷 Doorbell Button Hardware Plan

Two supported wiring strategies:
	1.	Parallel AC detector module
	•	Non-invasive
	•	ESP reads isolated digital output
	•	Gong wiring remains unchanged
	2.	Button as dry contact → ESP → relay drives gong
	•	More control
	•	Allows smart gong behaviour
	•	Requires mild rewiring

Current focus:
👉 Detect press at button → provide clean GPIO edge to ESP.

⸻

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

🧭 Next Implementation Steps
	1.	Create PlatformIO project for XIAO ESP32-S3 Sense
	2.	Add CameraWebServer-style HTTP stream
	3.	Add GPIO button ISR + debounce
	4.	Call Scrypted doorbell endpoint on press
	5.	Add device to Scrypted
	6.	Create Doorbell Group
	7.	Export to HomeKit & test UX

After MVP works:
	8.	Begin RTSP + audio pipeline exploration

⸻

📝 Open Questions / To-Do
	•	Select final doorbell button sensing scheme:
	•	AC detector vs dry contact + relay
	•	Confirm I2C sensor selection + pull-up values
	•	Confirm DECT group number and FRITZ!Box SIP account settings
	•	Decide target RTSP audio codec (AAC vs G.711)
	•	Verify I2S pin mapping for mic + MAX98357A on hardware (GPIO42/41 + GPIO7/8/9)
	•	Confirm speaker power + enclosure placement
	•	Evaluate latency + HomeKit experience
	•	Consider adding:
	•	status page (/status)
	•	uptime + last ring log
	•	OTA update support

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
