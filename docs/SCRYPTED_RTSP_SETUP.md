<!--
 Project: HomeKitKnock-S3
 File: docs/SCRYPTED_RTSP_SETUP.md
 Author: Jesse Greene
 -->

# RTSP Camera Integration with Scrypted

## Overview
Your ESP32-S3 doorbell now supports **RTSP (Real-Time Streaming Protocol)** for seamless integration with Scrypted. This is the preferred method for camera integration compared to MJPEG.

## What Changed

### New Features
1. **RTSP Server** running on port **8554**
2. Non-blocking RTSP client handling in main loop
3. RTSP URL displayed in web UI
4. Compatible with Scrypted, VLC, and other RTSP clients

### Files Added
- `include/rtsp_server.h` - RTSP server interface
- `src/rtsp_server.cpp` - RTSP server implementation using Micro-RTSP library

### Library Added
- **Micro-RTSP** (geeksville/Micro-RTSP) - Lightweight RTSP server for ESP32

## RTSP Stream URL

Your doorbell provides an RTSP stream at:

```
rtsp://<ESP32-IP>:8554/mjpeg/1
```

Example:
```
rtsp://192.168.178.188:8554/mjpeg/1
```

You can find this URL in the web UI under **Camera Settings** → **RTSP Stream (for Scrypted)**.

## Scrypted Setup Instructions

### 1. Add Camera to Scrypted

1. Open Scrypted web interface (typically `http://raspberry-pi:10443`)
2. Click **"+ Add Device"**
3. Select **"RTSP Camera"** from the plugin list
4. Enter the following settings:
   - **Name**: `Front Door Camera` (or your preferred name)
   - **RTSP URL**: `rtsp://192.168.178.188:8554/mjpeg/1`
   - **Username**: (leave empty - no auth required)
   - **Password**: (leave empty)
5. Click **"Save"**

### 2. Create Doorbell Device

1. In Scrypted, click **"+ Add Device"** again
2. Select **"Doorbell"** from the plugin list
3. Configure:
   - **Name**: `Front Door Doorbell`
   - **Camera**: Select the camera you just added
   - **Button URL**: `http://<ESP32-IP>/ring/sip` (optional - for testing)
4. Click **"Save"**

### 3. Configure HomeKit Integration

1. Find your doorbell device in Scrypted
2. Click on it to open settings
3. Under **"Integrations"**, enable **HomeKit**
4. In Apple Home app:
   - Tap **"+"** → **"Add Accessory"**
   - Scan the QR code from Scrypted
   - Your doorbell should appear as **"Front Door Doorbell"**

### 4. Test the Stream

**Test in VLC:**
1. Open VLC Media Player
2. File → Open Network Stream
3. Enter: `rtsp://192.168.178.188:8554/mjpeg/1`
4. Click **Play**

**Test in Scrypted:**
1. Open your camera device in Scrypted
2. Click the video preview
3. Stream should load within 1-2 seconds

## Doorbell Event Integration

### Option 1: Scrypted Webhook (Recommended)

Configure ESP32 to trigger the Scrypted doorbell webhook from the web UI:

1. In Scrypted, open your doorbell device
2. Find the **"Webhook URL"** (usually under Advanced settings)
3. Copy the URL (looks like: `http://scrypted-ip:11080/endpoint/<id>/public/`)
4. Open `http://<ESP32-IP>/sip` and paste it into **Doorbell Webhook URL**
5. Save and press the doorbell button to verify HomeKit notifications

### Option 2: Motion Detection in Scrypted

Enable motion detection in Scrypted camera settings to automatically detect events.

## Troubleshooting

### RTSP Stream Not Loading

**Check ESP32 Serial Monitor:**
```
✅ RTSP server started
📡 RTSP URL: rtsp://192.168.178.188:8554/mjpeg/1
```

**Check Network Connectivity:**
```bash
# From your computer/Scrypted server
ping 192.168.178.188
telnet 192.168.178.188 8554
```

**Check Firewall:**
- Ensure UDP port **8554** is not blocked
- ESP32 and Scrypted must be on same network

### Stream Connects But No Video

**Check Camera Initialization:**
- Serial monitor should show: `✅ Camera initialized`
- Web UI should show camera status and snapshot should work

**Verify MJPEG Stream Works:**
- Test MJPEG first: `http://192.168.178.188:81/stream`
- If MJPEG doesn't work, camera isn't initialized

**Check RTSP Client Connection:**
- Serial monitor shows: `🔌 RTSP client connected from <IP>`
- If you see disconnection immediately, check RTSP URL format

### Scrypted Rebroadcast Error (Missing -i)
If Scrypted logs show:
```
Unable to choose an output format for 'rtsp://...'
```
then ffmpeg is treating the camera URL as an output because `-i` is missing.

Fix:
1. Use the **RTSP Camera** plugin and set **RTSP URL** to `rtsp://<ESP32-IP>:8554/mjpeg/1`.
2. If using **FFmpeg Camera**, ensure your input starts with `-i`:
   ```
   -i rtsp://<ESP32-IP>:8554/mjpeg/1
   ```
3. As a fallback, use MJPEG:
   - Stream: `http://<ESP32-IP>:81/stream`
   - Snapshot: `http://<ESP32-IP>/capture`

### High Latency or Lag

**Reduce JPEG Quality:**
1. Open ESP32 web UI
2. Camera Settings → JPEG Quality
3. Increase value (lower quality = smaller files)
4. Try values between 10-20 for best balance

**Reduce Frame Size:**
1. Camera Settings → Frame Size
2. Try **HVGA (480x320)** or **CIF (400x296)**
3. Smaller resolution = faster streaming

**Network Optimization:**
- Use 5GHz WiFi if available (less congestion)
- Place ESP32 closer to WiFi router
- Check WiFi signal strength in web UI

### RTSP Server Crashes or Watchdog Resets

**This is a known limitation:**
- RTSP handling is non-blocking but resource-intensive
- Only one RTSP client supported at a time
- Multiple connections may cause watchdog resets

**Workaround:**
- Ensure Scrypted is the only RTSP client
- Don't use VLC simultaneously with Scrypted
- Restart ESP32 if stream becomes unresponsive

## Current Streaming Capabilities

### What Works Now ✅
- **RTSP video streaming** (MJPEG over RTSP)
- **Snapshot capture** (via HTTP)
- **MJPEG live stream** (via HTTP on port 81)
- **Scrypted integration** (RTSP camera)
- **HomeKit video doorbell** (via Scrypted)

### Future Enhancements 🚧
- **Audio support** (requires ESP-ADF and I2S microphone)
- **H.264 encoding** (better quality, lower bandwidth)
- **Two-way audio** (speaker + microphone)
- **Motion detection** (on ESP32 before streaming)

## Performance Metrics

**Memory Usage:**
- RAM: 15.8% (51,732 bytes)
- Flash: 33.8% (1,128,093 bytes)

**Stream Specs:**
- Protocol: RTSP (RFC 2326)
- Codec: Motion JPEG (MJPEG)
- Default Resolution: HVGA (480x320)
- Default Quality: 10 (JPEG quality)
- Port: 8554 (standard RTSP port)

## Comparing Protocols

### RTSP (Recommended) ✅
- **Pros**: Standard protocol, Scrypted native support, seeking/pausing
- **Cons**: Slightly more overhead than MJPEG

### MJPEG over HTTP
- **Pros**: Simple, works in browsers, low latency
- **Cons**: Not standard for NVR systems, no seeking

## Next Steps

1. ✅ Upload firmware to ESP32
2. ✅ Verify RTSP URL in web UI
3. ✅ Test stream in VLC
4. ✅ Add camera to Scrypted
5. ✅ Create doorbell device in Scrypted
6. ✅ Add to HomeKit via Scrypted
7. ✅ Configure doorbell button webhook
8. ✅ Test complete doorbell flow

## Support Resources

- **Scrypted Documentation**: https://docs.scrypted.app/
- **RTSP Specification**: RFC 2326
- **Micro-RTSP Library**: https://github.com/geeksville/Micro-RTSP
- **ESP32 Camera Docs**: https://github.com/espressif/esp32-camera

## Example Scrypted Configuration

```json
{
  "name": "Front Door Camera",
  "type": "Camera",
  "pluginId": "@scrypted/rtsp",
  "interfaces": [
    "Camera",
    "VideoCamera",
    "RTSPCamera"
  ],
  "settings": {
    "url": "rtsp://192.168.178.188:8554/mjpeg/1",
    "skipTranscode": false
  }
}
```

Your ESP32-S3 doorbell is now ready for Scrypted integration! 🎉
