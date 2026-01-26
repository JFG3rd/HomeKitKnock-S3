<!--
 Project: HomeKitKnock-S3
 File: docs/QUICK_START.md
 Author: Jesse Greene
 -->

# Quick Start: ESP32-S3 Doorbell → Scrypted → HomeKit

## 30-Second Setup

### 1. Get RTSP URL from ESP32
Open in browser: `http://<ESP32-IP>`

Copy RTSP URL from **Camera Settings** card:
```
rtsp://192.168.178.188:8554/mjpeg/1
```

### 2. Add to Scrypted
1. Scrypted → **Add Device** → **RTSP Camera**
2. Paste RTSP URL
3. Save

### 3. Create Doorbell
1. Scrypted → **Add Device** → **Doorbell**
2. Select the camera
3. Save

### 4. Add to HomeKit
1. Scrypted → Doorbell device → Enable **HomeKit**
2. Apple Home → Scan QR code
3. Done!

## Testing

**Test Camera Stream:**
```bash
# VLC
vlc rtsp://192.168.178.188:8554/mjpeg/1

# Or open in browser
http://192.168.178.188:81/stream
```

**Test Doorbell Button:**
- Press physical button on GPIO
- OR visit: `http://192.168.178.188/ring/sip`

**Expected Result:**
- HomeKit notification appears
- FRITZ!Box phones ring (if SIP configured)
- Video stream available in Home app
- Door opener relay can be triggered by DTMF sequence `123` (GPIO1)

## Key URLs

| Service | URL | Purpose |
|---------|-----|---------|
| Web UI | `http://192.168.178.188` | Configuration |
| RTSP | `rtsp://192.168.178.188:8554/mjpeg/1` | Scrypted camera |
| MJPEG | `http://192.168.178.188:81/stream` | Browser preview |
| MJPEG Audio | `http://192.168.178.188:81/audio` | Companion WAV stream |
| Live A/V | `http://192.168.178.188/live` | MJPEG + audio page |
| Snapshot | `http://192.168.178.188/capture` | Single frame |
| Setup Guide | `http://192.168.178.188/guide` | Feature setup instructions |
| SIP Setup | `http://192.168.178.188/sip` | Configure phone ring |
| Test Ring | `http://192.168.178.188/ring/sip` | Trigger manually |

## Credentials

### ESP32
- **WiFi**: Configure via AP mode (192.168.4.1)
- **No authentication** for web UI/camera

### FRITZ!Box SIP
- **Username**: IP phone username (e.g., 620)
- **Password**: IP phone password
- **Target**: `**610` (all DECT) or specific extension
 - **Door opener code**: `123` (matches ESP32 default DTMF sequence)
NOTE: FRITZ!Box may require SIP Digest auth; current firmware handles 401/407 challenges automatically.

### Scrypted
- **RTSP**: No username/password needed
- **HomeKit**: Auto-configured via QR code

## Troubleshooting

❌ **RTSP not working**
→ Check port 8554 is open
→ Verify ESP32 IP hasn't changed
→ Restart ESP32

❌ **Doorbell button not triggering**
→ Check GPIO wiring (see config.h)
→ Test with `/ring/sip` endpoint first
→ Verify SIP configured correctly

❌ **HomeKit not showing video**
→ Restart Scrypted
→ Check Scrypted camera preview works
→ Remove and re-add to Home app

## Full Documentation

- **SIP Integration**: See `SIP_INTEGRATION.md`
- **RTSP Setup**: See `SCRYPTED_RTSP_SETUP.md`
- **Architecture**: See `esp32-s3-doorbell-architecture.md`
