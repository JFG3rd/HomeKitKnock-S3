<!--
 Project: HomeKitKnock-S3
 File: docs/CURRENT_STATUS.md
 Author: Jesse Greene
 Last Updated: March 1, 2026
 Purpose: Current project status and next steps
 -->

# Current Project Status

**Last Updated:** March 1, 2026
**Device:** Seeed XIAO ESP32-S3 Sense
**Framework:** Pure ESP-IDF 5.5.0 (Arduino completely removed)
**Branch:** `aac-esp-adf`
**Status:** ✅ **Phase 5 + SIP Audio Complete** — Full doorbell workflow verified end-to-end

---

## Executive Summary

ESP-IDF migration phases 0–5 are complete and verified on hardware. Full doorbell workflow
confirmed end-to-end including bidirectional SIP audio and door opener relay.

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
| GPIO3 gong relay | ✅ Working | 150ms delay, 800ms pulse, original bell |
| GPIO1 door opener relay | ✅ Working | Triggered by DTMF "123" from Fritz!fon |
| SIP bidirectional audio | ✅ Working | G.711 PCMU/PCMA RTP, port 40000 |
| RTSP AAC audio | 🔧 Ready | Components built — enable RTSP in setup UI to test |
| Scrypted + HomeKit | ❌ Pending | RTSP audio → Scrypted → HSV |
| Full OTA system | ❌ Pending | Bare /api/ota works; hardening deferred |

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

## Bugs Fixed (March 2026 Session)

All fixes committed and pushed to `aac-esp-adf` branch.

| # | Component | Fix |
|---|-----------|-----|
| 1 | `main.c` | Main loop 50ms→10ms — restored G.711 RTP 20ms cadence |
| 2 | `sip_client.c` | SIP INFO handler for `application/dtmf-relay` door opener |
| 3 | `sip_client.c` | Static buffers in RTP functions — fixed main task stack overflow |
| 4 | `sip_client.c` | BYE/CANCEL Call-ID validation — stale retransmits no longer kill active calls |
| 5 | `sip_client.c` → `audio_output.c` | `audio_output_flush_and_stop()` in `reset_sip_call()` — eliminates post-call DMA replay knock |
| 6 | `main.c` | `on_dtmf()` rolling 3-digit buffer matching "123" (Fritz!Box door station protocol) |
| 7 | `docs/SIP Stack Spec.md` | Corrected rtp_port (7078→40000), direction (sendonly→sendrecv), dtmf sequence ("#9"→"123") |

---

## Next Steps

1. **RTSP AAC audio** — Enable RTSP in Setup UI → test with VLC (`rtsp://<ip>:8554/mjpeg/1`)
2. **UI cleanup** — Reorganize setup.html cards (relay timing, audio/mic separation, etc.)
3. **Scrypted + HomeKit Secure Video** — After RTSP audio confirmed:
   - Add ESP32 as RTSP Camera in Scrypted
   - Configure Doorbell Group + webhook
   - Enable HomeKit Secure Video (HSV) in Scrypted
4. **OTA hardening** — SHA-256 credentials + time-limited upload window (low priority)

---

## Memory Budget (Phase 5 + SIP Audio)

| Resource | Used | Available | % |
|----------|------|-----------|---|
| RAM | ~96,000 bytes | 327,680 bytes | ~29.3% |
| Flash | 1,231,045 bytes | 3,932,160 bytes | 31.3% |

RAM increased slightly from 28.5% → 29.3% as large local RTP buffers were moved to BSS (static).

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
