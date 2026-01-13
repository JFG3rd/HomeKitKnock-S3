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
	•	Stream video (Phase 1) and later audio (Phase 2) from ESP32-S3 to Scrypted
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
	•	✅ RTSP streaming (Phase 1 - COMPLETE)
	•	✅ MJPEG HTTP streaming (Phase 1 - COMPLETE)
	•	Later upgrade to H.264 + audio (Phase 2)

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

Audio streaming is Phase 2.

⸻

🔊 Phase 2 — Audio Streaming (Planned)

Goal:
	•	Provide true AV stream to Scrypted

Approach:
	•	Migrate to ESP-IDF / ESP-ADF
	•	Use RTSP example pipeline
	•	Capture:
	•	Camera frames
	•	I2S digital mic audio (XIAO ESP32-S3 Sense onboard mic)
	•	Stream:
	•	H.264 + AAC / G.711 over RTSP
	•	Output:
	•	MAX98357A I2S DAC amp + speaker for local audio

Scrypted consumes RTSP
→ HomeKit receives audio-enabled live stream.

Two-way audio is out-of-scope initially.

⸻

🔉 Planned Audio Hardware

Planned additions:
	•	Onboard digital microphone (XIAO ESP32-S3 Sense)
	•	MAX98357A I2S DAC amp
	•	Small speaker (doorbell chime + local monitoring)

Notes:
	•	Audio path will be I2S in/out (mic in, DAC out)
	•	Onboard PDM mic: GPIO42 = CLK, GPIO41 = DATA (I2S0 RX)
	•	MAX98357A I2S DAC: GPIO7 = BCLK, GPIO8 = LRCLK/WS, GPIO9 = DIN (I2S1 TX)
	•	MAX98357A SD/EN: tie to 3V3 (always on) or use a spare GPIO (e.g., GPIO1/D0) for mute
	•	GPIO7/8/9 are default SPI pins; avoid SPI on those pins or remap if needed
	•	Feature setup exposes mic enable/mute + sensitivity and audio out enable/mute + volume
	•	HTML test endpoint: http://ESP32-IP/audio.wav
	•	Local gong playback uses `/gong.pcm` from LittleFS when present

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
	•	Confirm final GPIO pin mapping
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
