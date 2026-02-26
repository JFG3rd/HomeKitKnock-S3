<!--
 Project: HomeKitKnock-S3
 File: docs/CURRENT_STATUS.md
 Author: Jesse Greene
 Last Updated: February 26, 2026
 Purpose: Current project status and next steps
 -->

# Current Project Status

**Last Updated:** February 26, 2026
**Device:** Seeed XIAO ESP32-S3 Sense
**Framework:** Pure ESP-IDF 5.5.0 (Arduino completely removed)
**Branch:** `aac-esp-adf`
**Status:** ✅ **Phase 5 Complete** — Full audio I/O working

---

## Executive Summary

ESP-IDF migration phases 0–5 are complete and verified on hardware:

| Feature | Status | Notes |
|---------|--------|-------|
| WiFi / Captive portal | ✅ Working | STA + AP + APSTA modes |
| Web UI (12 embedded pages) | ✅ Working | Gzip-embedded in flash |
| NVS config persistence | ✅ Working | All settings survive reboot |
| SIP / Fritz!Box ring | ✅ Working | Digest auth, INVITE/CANCEL |
| Doorbell button (GPIO4) | ✅ Working | Debounced, triggers SIP + gong |
| Status LED (GPIO2) | ✅ Working | PWM patterns |
| OV2640 camera / MJPEG | ✅ Working | VGA JPEG, port 81 |
| RTSP stream (port 8554) | ✅ Working | MJPEG-over-RTP |
| Speaker gong (MAX98357A) | ✅ Working | PCM from flash, volume 0–100% |
| INMP441 mic capture | ✅ Working | GPIO5 SD, shared I2S bus |
| Record & Play test | ✅ Working | Verified end-to-end on hardware |
| SIP bidirectional audio | 🔧 Planned | Unblocked — RTP TX next |
| RTSP AAC audio | 🔧 Planned | Unblocked — wire aac_encoder_pipe |
| HomeKit integration | ❌ Pending | Phase 6 |
| Full OTA system | ❌ Pending | Phase 7 |

---

## Phase 5 Audio — What Was Solved

Phase 5 required solving three hardware/driver bugs before mic capture worked:

### Bug 1: GPIO12 Camera Conflict
**Symptom**: INMP441 all-zeros on any reading.
**Root cause**: INMP441 SD was wired to GPIO12. On the Seeed XIAO ESP32-S3 Sense expansion
board, GPIO12 = OV2640 camera Y7 data output. The camera chip drives GPIO12 electrically
regardless of whether the camera is initialized in software. This overrode the I2S DIN line.
**Fix**: Moved INMP441 SD to **GPIO5 (D4 header pin)** — free, not on the camera expansion board.
**Config**: `I2S_INMP441_SD = 5` in `include/config.h`. GPIO12 is permanently forbidden for INMP441.

### Bug 2: BCLK Not Generated
**Symptom**: INMP441 outputting no data even after GPIO fix.
**Root cause**: In ESP-IDF full-duplex I2S, the TX channel is the BCLK master. The BCLK pin
(GPIO7) has no clock until TX is enabled. `audio_capture_start()` only enabled the RX channel,
leaving GPIO7 static. INMP441 requires BCLK to clock out data.
**Fix**: `start_inmp441_mic()` explicitly enables the TX channel after enabling RX.
`disable_tx_channel()` checks `audio_capture_is_running()` and skips the disable while
INMP441 capture is active, keeping BCLK alive.

### Bug 3: Stereo DMA in MONO Mode (50% zeros)
**Symptom**: `peak=11% zeros=50%` — every other sample was zero; playback inaudible.
**Root cause**: ESP-IDF STD I2S driver returns stereo-interleaved `[L, R, L, R, ...]` data in
the DMA buffer even when `I2S_SLOT_MODE_MONO` is configured. INMP441 with L/R=GND outputs
only on the left channel; the right channel slot is always zero. `audio_capture_read()` was
treating the buffer as pure mono → every other sample was R=0.
**Fix**: `audio_capture_read()` reads stereo frames in 256-frame chunks (1 KB stack buffer)
and extracts only the L channel: `buffer[i] = chunk[2*i]`.

**Result after all three fixes**: `peak=~22% zeros=~0% played=yes` — clear audible playback. ✅

---

## Next: Phase 6 — HomeKit Doorbell

The device already:
- Streams video via RTSP to Scrypted
- Triggers doorbell events via HTTP webhook to Scrypted
- Rings Fritz!Box DECT phones via SIP

Phase 6 adds native HomeKit doorbell integration (HAP SDK or Scrypted bridge).

### Audio Stretch Goals (Phase 5 follow-on)
- **SIP two-way audio**: G.711 PCMU/PCMA RTP TX path (mic → caller)
- **RTSP AAC audio**: Wire `aac_encoder_pipe` (ESP-ADF) into RTSP server

---

## Memory Budget (Phase 5)

| Resource | Used | Available | % |
|----------|------|-----------|---|
| RAM | 93,304 bytes | 327,680 bytes | 28.5% |
| Flash | 1,231,045 bytes | 3,932,160 bytes | 31.3% |

Significant headroom remains for Phase 6 (HomeKit HAP SDK typically ~100KB flash).

---

## Known Wiring Notes

| Pin | Usage | Warning |
|-----|-------|---------|
| GPIO5 (D4) | INMP441 SD | Reassigned from I2C SDA placeholder |
| GPIO12 | OV2640 camera Y7 | **DO NOT use for INMP441** — camera chip drives this pin |
| GPIO7 | Shared I2S BCLK | MAX98357A + INMP441 share this clock |
| GPIO8 | Shared I2S WS | MAX98357A + INMP441 share this word select |
| GPIO41/42 | Onboard PDM mic | Integrated on XIAO ESP32-S3 Sense PCB by Seeedstudio; available as alternative mic source via software |

See [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) for the full wiring map.

---

## Build & Flash

```bash
# Build
pio run -e seeed_xiao_esp32s3_idf

# Upload
pio run -e seeed_xiao_esp32s3_idf -t upload

# Monitor
pio device monitor -e seeed_xiao_esp32s3_idf

# Erase NVS only
~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbmodem21201 \
    erase_region 0x9000 0x5000
```
