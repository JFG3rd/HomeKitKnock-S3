<!--
 Project: HomeKitKnock-S3
 File: docs/IDF_ADF_MIGRATION_PLAN.md
 Purpose: Phased plan to migrate from Arduino-as-component to pure ESP-IDF
 Author: Codex (with user direction)
 Last Updated: February 22, 2026
-->

# Migration Plan: Pure ESP-IDF + ESP-ADF

Goal: Retire the Arduino layer and deliver all current features using pure ESP-IDF
(SIP intercom, RTSP/HTTP streaming, embedded web UI, OTA, HomeKit doorbell).
Note: ESP-ADF is NOT used — audio is implemented with native ESP-IDF I2S drivers.

---

## Phase 0 — Pre-migration hygiene (keep current stack) ✅ COMPLETE
- ✅ Align flash config to real hardware (8 MB) and regenerate partitions.
- ✅ Enable required mbedTLS options (PSK/ciphers) to keep TLS usable during transition.
- ✅ Full flash erase once the above are set.

## Phase 1 — Stand up IDF base (no Arduino) ✅ COMPLETE
- ✅ Create a minimal IDF app: logging, PSRAM init, Wi‑Fi STA/AP bring-up, raw NVS API.
- ✅ Reuse embedded web assets via `esp_http_server`.
- ✅ Success criteria: boots reliably, serves `/` from embedded assets, no NVS errors.

## Phase 2 — Networking & config services ✅ COMPLETE

### Implemented Features ✅
- ✅ Config storage with raw NVS namespaces (`wifi`, `config`)
- ✅ WiFi credential persistence across reboots
- ✅ HTTP-based OTA firmware update (`/api/ota`)
- ✅ WiFi scanning in AP mode (APSTA mode with scan caching)
- ✅ SSID deduplication (filters duplicates, keeps strongest signal)
- ✅ Credential clearing and device restart endpoints
- ✅ Root redirect logic (AP mode → wifi-setup, STA mode → index)
- ✅ System status API (`/api/status`) with uptime, heap, RSSI
- ✅ Captive portal with DNS server (auto-popup on mobile devices)
- ✅ Auto-restart after saving WiFi credentials
- ✅ Styled restart page with auto-reconnect
- ✅ Web-based log viewer (`/logs.html`) with filtering
- ✅ Browser autofill protection on WiFi setup form

## Phase 3 — SIP intercom ✅ COMPLETE

### Implemented Features ✅
- ✅ SIP client ported to ESP-IDF component (lwIP sockets)
- ✅ SIP REGISTER with digest authentication (MD5)
- ✅ SIP INVITE/CANCEL/ACK/BYE for call management
- ✅ Fritz!Box door intercom integration (`**11`, `**12`, etc.)
- ✅ Physical doorbell button (GPIO4, active-low, debounced)
- ✅ Status LED (GPIO2, PWM with multiple patterns)
- ✅ SNTP time synchronization (German timezone)
- ✅ Real timestamps in web log viewer
- ✅ Single-line verbose SIP logging
- ✅ NVS persistence for SIP credentials
- ✅ Web UI for SIP configuration (`/sip.html`)
- ✅ Test ring button in web interface

### Success Criteria (All Met)
| Criterion | Status | Notes |
|-----------|--------|-------|
| SIP registration to Fritz!Box | ✅ | Digest auth working |
| Ring phones on button press | ✅ | GPIO4 triggers SIP INVITE |
| Visual feedback | ✅ | LED breathing during ring |
| Web configuration | ✅ | SIP settings via `/sip.html` |
| Survives reboots | ✅ | Config in NVS |

---

## Phase 4 — Video path ✅ COMPLETE

### Implemented Features ✅
- ✅ esp32-camera component (`src_idf/components/camera/`) with OV2640, VGA JPEG, 2 PSRAM buffers
- ✅ MJPEG HTTP streaming (`src_idf/components/mjpeg_server/`) on port 81, raw lwIP sockets, max 2 clients
- ✅ JPEG snapshot endpoint (`/capture`)
- ✅ Camera stream info endpoint (`/cameraStreamInfo`)
- ✅ Feature toggle for HTTP camera (`http_cam_enabled` in NVS, gated init)
- ✅ Camera config at runtime via `/control?var=X&val=Y` (framesize, quality, brightness, contrast)
- ✅ Camera settings persisted to NVS and restored at boot
- ✅ Mic/audio settings persisted to NVS (mic_enabled, mic_muted, mic_sensitivity, aac_sample_rate, aac_bitrate)
- ✅ Combined `/status` endpoint returns system + camera + audio fields
- ✅ PSRAM 8MB OPI enabled in `sdkconfig.seeed_xiao_esp32s3_idf`
- ✅ lwIP sockets increased to 16 (was 10)
- ✅ Setup page Camera Config card fully functional (apply + persist all settings)
- ✅ RTSP MJPEG server on port 8554 (`src_idf/components/rtsp_server/`)
- ✅ `rtsp_enabled` feature toggle with NVS persistence
- ✅ RTSP client count → `LED_STATE_RTSP_ACTIVE` integration

### Success Criteria
| Criterion | Status | Notes |
|-----------|--------|-------|
| Camera init with PSRAM | ✅ | 8MB OPI, 2 frame buffers |
| MJPEG HTTP stream (port 81) | ✅ | Stable, tested with browser |
| JPEG snapshot (/capture) | ✅ | 640x480, ~35KB |
| Camera settings persist | ✅ | NVS save/restore across reboots |
| RTSP stream (port 8554) | ✅ | MJPEG-over-RTP implemented |
| Stable stream to Scrypted | ✅ | Both HTTP MJPEG and RTSP working |

## Phase 5 — Audio path (pure ESP-IDF, no ADF) ✅ COMPLETE

**Architecture decision**: Audio is implemented with native ESP-IDF I2S drivers, not ESP-ADF.
INMP441 (external I2S mic) is the active source. The onboard PDM mic (GPIO41/42) is physically integrated on the XIAO ESP32-S3 Sense PCB by Seeedstudio and is available as a software-selectable fallback.
`aac_encoder_pipe` will use ESP-ADF for future RTSP AAC streaming (not yet wired into build).

### Shared I2S Bus
GPIO7 (BCLK) and GPIO8 (WS) are physically shared between MAX98357A speaker and INMP441 mic.
The `i2s_shared_bus` component (`src_idf/components/i2s_shared_bus/`) creates a full-duplex
I2S_NUM_1 channel pair — TX (GPIO9 → MAX98357A DIN) and RX (**GPIO5** ← INMP441 SD) — simultaneously.
Both `audio_output` and `audio_capture` call `i2s_shared_bus_init()` (idempotent) and get their
channel handles from the shared bus. No stop-capture-before-play required.

**GPIO5 (D4) is the INMP441 SD pin.** GPIO12 must NOT be used — it is the OV2640 camera Y7 data
output pin and the camera chip drives it electrically regardless of camera software state.

### Key Implementation Insights
- **TX = BCLK master**: TX channel must be enabled before INMP441 can clock out data. `start_inmp441_mic()` explicitly enables TX; `disable_tx_channel()` skips disable while INMP441 is running.
- **Stereo DMA in MONO mode**: ESP-IDF STD I2S RX returns stereo-interleaved `[L, R, L, R, ...]` in the DMA buffer even when configured as `I2S_SLOT_MODE_MONO`. `audio_capture_read()` de-interleaves in 256-frame chunks to extract L-channel only (INMP441 with L/R=GND outputs on left channel).
- **slot_bit_width=32**: INMP441 requires 64 BCLK per LRCLK (32-bit I2S frames). Default macro gives 16-bit → no output. Both TX and RX force `I2S_SLOT_BIT_WIDTH_32BIT`.

### Implemented ✅
- ✅ `audio_output` component: MAX98357A speaker, I2S_NUM_1 via shared bus, gong PCM from flash, synthesized 880/660 Hz fallback, volume 0–100%, deferred TX channel, NVS persistence
- ✅ `audio_output_init()` runs unconditionally at boot (not gated by camera enable flag)
- ✅ `audio_out_enabled` NVS default = 1 (gong is a core feature)
- ✅ Volume slider always interactive (not disabled by audio_out_enabled checkbox)
- ✅ `audio_capture` component: INMP441 via shared bus, stereo DMA de-interleave, PDM fallback, sensitivity scaling, NVS persistence
- ✅ `i2s_shared_bus` component: full-duplex I2S_NUM_1, TX BCLK master, RX INMP441
- ✅ `POST /api/mic/test`: record 2s → stats → play back → JSON (`peak`, `rms`, `zeros`, `played`)
- ✅ "🎤 Record & Play" button on setup page — verified working end-to-end
- ✅ `audio_output_flush_and_stop()`: DMA silence flush prevents circular buffer replay
- ✅ `aac_encoder_pipe` component: AAC-LC encoder stub (wiring to RTSP pending)
- ✅ setup.html: `rebootRequired` flag, confirmation modals for Save/Restart/OTA

### Success Criteria
| Criterion | Status | Notes |
|-----------|--------|-------|
| Speaker gong on button press | ✅ | MAX98357A via I2S_NUM_1 shared bus |
| Volume control 0–100% | ✅ | NVS persistence |
| Shared bus full-duplex | ✅ | GPIO7/8 shared, TX=BCLK master |
| INMP441 capture (Record & Play) | ✅ | GPIO5 SD, de-interleaved, verified |
| INMP441 independent of camera | ✅ | Camera-gate bug fixed |
| SIP bidirectional audio | 🔧 | Unblocked — RTP TX path next |
| RTSP audio (AAC) | 🔧 | Unblocked — wire aac_encoder_pipe next |

## Phase 6 — HomeKit doorbell
- Integrate Espressif HAP SDK (IDF) or external bridge; expose doorbell + camera.
- Success: doorbell event + live video/audio in Home app.

## Phase 7 — OTA update system
- Port the full OTA system from Arduino version to ESP-IDF.
- Credential management: username + SHA-256 password hash in NVS (`ota` namespace).
- Time-limited upload window (5 minutes), enabled via authenticated request.
- HTTP Basic auth for `/ota/config`, `/ota/enable` endpoints.
- Firmware upload via `/ota/update` (replaces bare-bones `/api/ota`).
- Remove LittleFS filesystem upload (not needed with embedded assets).
- Web UI already complete: `setup.html` OTA card + `ota.html` upload page.
- Success: secure OTA firmware update from local network, credentials survive reboots.

## Phase 8 — Cleanup & resilience
- Remove Arduino build artifacts, unused scripts, LittleFS stubs.
- Add factory-reset path for NVS, brownout handling, watchdog tuning.

---

## Current Architecture (Phase 5 In Progress)

### Component Structure
```
src_idf/
├── main/
│   └── main.c                    # Boot sequence, main loop, button/LED/camera/audio integration
└── components/
    ├── nvs_manager/              # Raw NVS C API wrapper
    ├── wifi_manager/             # WiFi STA/AP/APSTA modes
    ├── web_server/               # HTTP server + REST API
    ├── dns_server/               # Captive portal DNS
    ├── log_buffer/               # Web-accessible log ring buffer
    ├── config_manager/           # Typed configuration storage
    ├── sip_client/               # SIP state machine + RTP
    ├── button/                   # Doorbell button (GPIO4)
    ├── status_led/               # PWM LED patterns (GPIO2)
    ├── camera/                   # OV2640 driver + NVS config (Phase 4)
    ├── mjpeg_server/             # MJPEG HTTP streaming port 81 (Phase 4)
    ├── rtsp_server/              # RTSP server port 8554 (Phase 4)
    ├── audio_output/             # MAX98357A speaker, gong playback, volume ✅ (Phase 5)
    ├── audio_capture/            # INMP441 mic capture via shared I2S bus ✅ (Phase 5)
    ├── i2s_shared_bus/           # Full-duplex I2S_NUM_1 shared channel manager ✅ (Phase 5)
    └── aac_encoder_pipe/         # AAC-LC encoder pipeline for RTSP audio (Phase 5)
```

### Boot Sequence
```
1. NVS Manager Init (with auto-recovery)
2. Log Buffer Init (hooks into ESP-IDF logging)
   Status LED Init (PWM visual feedback)
   Button Init (GPIO4, debounce callback)
   Audio Output Init (unconditional — speaker is core feature, TX channel deferred)
3. WiFi Manager Init (esp_netif + event loop)
4. WiFi Start (STA if credentials, else AP+DNS)
5. Web Server + SIP Client (via WiFi event callback)
   SNTP Init (when connected to WiFi)
6. Audio Capture Init (deferred to WiFi got-IP, if mic enabled — independent of camera)
7. Camera Init + MJPEG/RTSP Server Start (deferred, after WiFi GOT_IP, if enabled)
```

### Main Loop Processing
- Button polling (debounced edge detection, triggers SIP ring + gong)
- Status LED pattern updates
- SIP message handling + RTP media processing
- Deferred ring requests
- SIP registration refresh
- Deferred audio capture init (when WiFi connected + mic enabled)
- Deferred camera init (when WiFi connected + camera enabled)
- MJPEG/RTSP client count → LED state tracking

### NVS Namespaces
| Namespace | Keys | Purpose |
|-----------|------|---------|
| `wifi` | `ssid`, `password` | WiFi credentials |
| `sip` | `user`, `password`, `displayname`, `target`, `enabled`, `verbose` | SIP configuration |
| `camera` | `http_cam_en`, `rtsp_en`, `framesize`, `quality`, `brightness`, `contrast` | Camera feature toggle + sensor config |
| `camera` | `mic_en`, `mic_mute`, `mic_sens`, `aac_rate`, `aac_bitr`, `mic_source` | Mic/audio capture config |
| `camera` | `aud_out_en`, `aud_volume`, `hw_diag` | Speaker output + diagnostic mode |
| `system` | `timezone` | Timezone string (POSIX TZ format) |
| `ota` | `username`, `pass_hash` | OTA credentials (Phase 7) |

### GPIO Assignments
| GPIO | Function | Configuration |
|------|----------|---------------|
| GPIO4 | Doorbell Button | Active-low, internal pull-up, 50ms debounce |
| GPIO2 | Status LED | PWM (LEDC), 8-bit, 5kHz |
| GPIO1 | Door Opener Relay | Active-high (future) |
| GPIO3 | Gong Relay | Active-high (future) |
| GPIO7 | Shared I2S BCLK | MAX98357A BCLK + INMP441 SCK (shared via i2s_shared_bus) |
| GPIO8 | Shared I2S WS | MAX98357A LRC + INMP441 WS (shared via i2s_shared_bus) |
| GPIO9 | I2S_NUM_1 TX data | MAX98357A DIN (speaker data out) |
| GPIO5 | I2S_NUM_1 RX data | INMP441 SD/DOUT (mic data in) — GPIO12 forbidden (camera Y7) |
| GPIO41/42 | PDM Mic (onboard) | Integrated on XIAO ESP32-S3 Sense PCB by Seeedstudio; INMP441 is active source |

### API Endpoints
| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Redirect (AP→setup, STA→index) |
| `/api/wifi` | POST | Save WiFi credentials |
| `/api/wifi` | DELETE | Clear WiFi credentials |
| `/api/status` | GET | System status JSON (detailed) |
| `/api/ota` | POST | Firmware update |
| `/api/logs` | GET | Get logs (filter=all/core/camera/doorbell) |
| `/api/logs` | DELETE | Clear log buffer |
| `/api/features` | GET | Get feature toggle states |
| `/api/sip` | GET | Get SIP config |
| `/api/sip` | POST | Save SIP config |
| `/api/sip/ring` | POST | Trigger SIP ring |
| `/api/sip/verbose` | GET/POST | Toggle verbose logging |
| `/api/audio/gong` | POST | Play test gong on speaker |
| `/api/mic/test` | POST | Record 2s + play back (mic diagnostics) |
| `/capture` | GET | JPEG snapshot (Phase 4) |
| `/cameraStreamInfo` | GET | Camera/streaming status (Phase 4) |
| `/control` | GET | Camera/mic settings `?var=X&val=Y` (Phase 4) |
| `/status` | GET | Combined system + camera + audio status (Phase 4) |
| `/saveFeatures` | POST | Save feature toggles |
| `/ring/sip` | GET | Legacy ring trigger |
| `/saveWiFi` | POST | Legacy credential save |
| `/saveSIP` | POST | Legacy SIP config save |
| `/scanWifi` | GET | Start WiFi scan |
| `/wifiScanResults` | GET | Get deduplicated scan results |
| `/deviceStatus` | GET | Basic device status |
| `/sipDebug` | GET | SIP debug info |
| `/restart` | GET | Styled restart page with auto-reconnect |

### Status LED Patterns (Priority Order)
1. **Ringing**: Breathing animation (1.4s period, 6s duration)
2. **AP Mode**: Double-blink (1s period)
3. **WiFi Connecting**: 2Hz blink (500ms period)
4. **SIP Error**: Slow pulse (2s period)
5. **SIP OK**: Steady low glow (duty 24/255)
6. **RTSP Active**: Short tick every 2s (future)

### SIP Integration Details
- **Fritz!Box Registration**: IP phone with internal number
- **Door Intercom Numbers**: `**11` = Ring Key 1, `**12` = Ring Key 2, etc.
- **Authentication**: MD5 digest with nonce from 401 response
- **Call Flow**: INVITE → 401 → Auth INVITE → 100/183 → 200 → ACK
- **Verbose Logging**: Single-line format with `|` separators

### Web Assets (Embedded Gzip)
11 files totaling ~35KB compressed:
- index.html, style.css, wifi-setup.html, setup.html
- live.html, guide.html, ota.html, sip.html
- logs.html, logs-camera.html, logs-doorbell.html

---

## Commands Reference

### Build and Upload
```bash
pio run -t upload -e seeed_xiao_esp32s3_idf
```

### Monitor Serial Output
```bash
pio device monitor -e seeed_xiao_esp32s3_idf
```

### Erase NVS (Clear Credentials)
```bash
~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbmodem21201 \
    erase_region 0x9000 0x5000
```

### Full Flash Erase
```bash
pio run -t erase -e seeed_xiao_esp32s3_idf
```

---

## Session Summary (February 6, 2026)

### Phase 3 Completion
1. **SIP Client**: Full port to ESP-IDF with lwIP sockets
2. **Fritz!Box Integration**: Register, INVITE, digest auth working
3. **Physical Button**: GPIO4 with debounce triggers SIP ring
4. **Status LED**: GPIO2 PWM with multiple state patterns
5. **SNTP**: Real timestamps in logs (German timezone)
6. **Config Reload**: Fixed stale config bug on ring
7. **Verbose Logging**: Single-line SIP packets for easy debugging

### Key Bug Fixes
1. **SIP config not reloading** - Config was cached at boot; now reloads from NVS on each ring
2. **Wrong target number** - Was sending `**11` when user configured `**12`
3. **Multi-line verbose logs** - Changed to single-line with `|` separators

### Hardware Tested
- XIAO ESP32-S3 Sense
- Fritz!Box 6591 Cable
- Physical button on GPIO4
- LED on GPIO2

### Fritz!Box Configuration
- Created IP phone (LAN/WLAN)
- Assigned internal number (e.g., 620)
- Configured Ring Keys: `**11` → Call group, `**12` → specific phone

---

## Session Summary (February 8-10, 2026)

### Phase 4 Progress (Camera + MJPEG Streaming)
1. **Camera Component**: OV2640 with PSRAM frame buffers, NVS config persistence
2. **MJPEG Server**: Raw lwIP sockets on port 81, max 2 clients, TCP_NODELAY
3. **Web Integration**: `/capture`, `/cameraStreamInfo`, `/control`, `/status` endpoints
4. **Feature Toggle**: HTTP camera enable/disable with NVS persistence, gated camera init
5. **Camera Config Card**: All settings (framesize, quality, brightness, contrast) saved to NVS
6. **Mic/Audio Config**: Settings stored in NVS (mic_enabled, mic_muted, sensitivity, sample rate, bitrate)
7. **CSS Layout Fix**: Cards top-align, grow to footer on desktop, scrollable on mobile

### Key Bug Fixes
1. **esp_camera not found** - PlatformIO lib_deps doesn't auto-register as IDF component; added EXTRA_COMPONENT_DIRS
2. **PSRAM not enabled** - Settings were in wrong sdkconfig file; PlatformIO uses `sdkconfig.seeed_xiao_esp32s3_idf`
3. **Socket exhaustion** - httpd + MJPEG server exceeded lwIP pool; increased CONFIG_LWIP_MAX_SOCKETS to 16
4. **Feature toggle not saving** - `/saveFeatures` only handled sip_enabled; added camera toggle
5. **Brightness/contrast not persisting** - OV2640 `WRITE_REG_OR_RETURN` macro returns non-zero on I2C issues; decoupled NVS save from sensor return code

### Memory Usage (Phase 4)
- RAM: 24.6% (80,728 / 327,680 bytes)
- Flash: 26.5% (1,043,865 / 3,932,160 bytes)

### Next: RTSP Server
- Implement RTSP MJPEG server on port 8554 using ESP-ADF `esp_media_protocols` library
- Wire `rtsp_enabled` feature toggle
- Test with Scrypted RTSP source

---

## Session Summary (February 26, 2026)

### Phase 5 Completion — INMP441 Mic Working End-to-End

1. **GPIO conflict found and fixed**: INMP441 SD was on GPIO12. GPIO12 = OV2640 camera Y7 data output pin on the Seeed XIAO ESP32-S3 Sense expansion board. Camera chip drives GPIO12 at all times regardless of software camera state → all-zeros on I2S DIN. Moved INMP441 SD to **GPIO5 (D4 header pin)** which is free.
2. **BCLK not generated**: TX channel (BCLK master on I2S_NUM_1) was not enabled when `audio_capture_start()` was called. INMP441 requires BCLK to clock out data. Fix: `start_inmp441_mic()` explicitly enables TX; `disable_tx_channel()` checks `audio_capture_is_running()` and skips disable while INMP441 is capturing.
3. **50% zeros root cause**: ESP-IDF STD I2S RX always returns stereo-interleaved DMA data `[L, R, L, R, ...]` even in `I2S_SLOT_MODE_MONO`. INMP441 with L/R=GND outputs only on the left channel; R slot is always zero. Fix: `audio_capture_read()` reads 256-frame stereo chunks and extracts `chunk[2*i]` (L channel) into the output buffer.
4. **Result**: `peak=~22%, zeros=~0%, played=yes` — Record & Play produces clear audible playback. ✅

### Key Bug Fixes
1. **GPIO12 camera conflict** — Never use GPIO12 for INMP441 SD on XIAO ESP32-S3 Sense with expansion board
2. **BCLK not generated** — TX must be enabled before RX capture starts; must stay enabled while INMP441 is running
3. **Stereo DMA in MONO mode** — ESP-IDF always returns stereo DMA; must de-interleave to get mono L-channel

### Memory Usage (Phase 5 Complete)
- RAM: 28.5% (93,304 / 327,680 bytes)
- Flash: 31.3% (1,231,045 / 3,932,160 bytes)

---

## Session Summary (February 22, 2026)

### Phase 4 Completion + Phase 5 Audio Output Fixes

1. **RTSP Server**: Implemented (`src_idf/components/rtsp_server/`), Phase 4 complete
2. **Audio Output Camera-Gate Bug**: `audio_output_init()` was inside the camera-enabled block — speaker never initialized if HTTP streaming was disabled. Moved to unconditional boot init (after button callback registration).
3. **NVS Default Bug**: `audio_out_enabled` key defaulted to 0 when absent — speaker was disabled by default. Changed to default 1 (gong is a core doorbell feature).
4. **Volume Slider Coupled to Checkbox**: `updateAudioUi()` disabled the volume slider when `audio_out_enabled` was unchecked. Removed this coupling — volume slider is always interactive.
5. **Deferred TX Channel**: `audio_output_init()` no longer creates the I2S TX channel at boot. TX is created at first `prepare_exclusive_playback()` call to avoid GPIO conflicts with INMP441 on the same I2S_NUM_1 port.
6. **Slider Auto-Save**: Added `change` event fallback alongside `input` event for reliable auto-save when slider is released.

### Architecture Decision: No ADF
Confirmed that audio uses native ESP-IDF I2S drivers only, not ESP-ADF:
- `audio_output`: standalone `driver/i2s_std.h` with `i2s_shared_bus` (Phase 5)
- `audio_capture`: `driver/i2s_std.h` + `driver/i2s_pdm.h` (PDM onboard — not connected; INMP441 via shared bus)
- `aac_encoder_pipe`: custom pipeline using `esp_audio_codec` for AAC-LC

### Wiring Clarification
Per `docs/WIRING_DIAGRAM.md`: GPIO7/8 are PHYSICALLY shared between MAX98357A and INMP441. GPIO43/44 (PDM pins) are explicitly NOT connected. Therefore INMP441 is the sole mic source and `i2s_shared_bus` is mandatory for full-duplex.

### Next Session: Shared Bus Integration
- Fix two bugs in `i2s_shared_bus.c` (RX `.dout` conflict, TX slot mode)
- Wire `audio_output.c` and `audio_capture.c` to use shared bus
- Move `audio_capture_init()` out of camera-gated block
- Add `POST /api/mic/test` + "🎤 Record & Play" button
