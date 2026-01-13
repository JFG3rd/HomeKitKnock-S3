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

### 1. Install FFmpeg Camera Plugin

The RTSP stream outputs **MJPEG (YUV 4:2:2)** which needs conversion to **H.264 (YUV 4:2:0)** for HomeKit compatibility.

1. Open Scrypted web interface (typically `http://raspberry-pi:10443`)
2. Go to **Plugins** → **Install Plugin**
3. Search for **"FFmpeg Camera"** and install it

### 2. Add Camera to Scrypted

1. Click **"+ Add Device"**
2. Select **"FFmpeg Camera"** (NOT "RTSP Camera")
3. Enter the following settings:
   - **Name**: `Front Door Camera` (or your preferred name)
   - **RTSP URL**: `rtsp://192.168.178.188:8554/mjpeg/1`
   - **Username**: (leave empty - no auth required)
   - **Password**: (leave empty)
4. Click **"Save"**

### 3. Configure FFmpeg Output Settings

This is the **critical step** to convert MJPEG (4:2:2) to H.264 (4:2:0) for HomeKit:

1. Open your newly created camera device in Scrypted
2. Navigate to **"Streams"** section
3. Find **"FFmpeg Output Prefix"** dropdown
4. Select or enter the following preset:
   ```
   -c:v libx264 -pix_fmt yuvj420p -preset ultrafast -bf 0 -g 60 -r 15 -b:v 500000 -bufsize 1000000 -maxrate 500000
   ```
5. Click **"Save"**

**What this does:**
- `-c:v libx264` - Uses H.264 encoder
- `-pix_fmt yuvj420p` - **Converts YUV 4:2:2 to 4:2:0** (required for HomeKit)
- `-preset ultrafast` - Fast encoding for low latency
- `-bf 0` - No B-frames (reduces latency)
- `-g 60` - Keyframe every 60 frames
- `-r 15` - Output at 15 fps
- `-b:v 500000` - 500 kbps bitrate
- `-bufsize 1000000` - 1 MB buffer
- `-maxrate 500000` - 500 kbps max rate

### 4. Create Doorbell Device

1. In Scrypted, click **"+ Add Device"** again
2. Select **"Doorbell"** from the plugin list
3. Configure:
   - **Name**: `Front Door Doorbell`
   - **Camera**: Select the FFmpeg camera you just added
   - **Button URL**: `http://<ESP32-IP>/ring/sip` (optional - for testing)
4. Click **"Save"**

### 5. Configure HomeKit Integration

1. Find your doorbell device in Scrypted
2. Click on it to open settings
3. Under **"Integrations"**, enable **HomeKit**
4. In Apple Home app:
   - Tap **"+"** → **"Add Accessory"**
   - Scan the QR code from Scrypted
   - Your doorbell should appear as **"Front Door Doorbell"**

### 6. Test the Stream

**Test in VLC:**
1. Open VLC Media Player
2. File → Open Network Stream
3. Enter: `rtsp://192.168.178.188:8554/mjpeg/1`
4. Click **Play**
5. You should see the camera feed (MJPEG format)

**Test in Scrypted:**
1. Open your FFmpeg camera device in Scrypted
2. Click the video preview
3. Stream should load and show H.264 video within 1-2 seconds

**Verify H.264 Conversion:**
Check Scrypted logs for confirmation:
```
Input #0, rtsp, from 'rtsp://192.168.178.188:8554/mjpeg/1':
  Stream #0:0: Video: mjpeg (Baseline), yuvj422p, 480x320, 30 fps
Stream mapping:
  Stream #0:0 -> #0:0 (mjpeg (native) -> h264 (libx264))
[libx264] profile Constrained Baseline, level 2.1, 4:2:0, 8-bit
```

## Expected Performance

**RTSP Stream from ESP32:**
- Format: MJPEG (Motion JPEG)
- Resolution: 480x320
- Frame Rate: 30 fps
- Color Space: YUV 4:2:2
- Bandwidth: ~300-500 KB/s

**After FFmpeg Conversion:**
- Format: H.264
- Resolution: 480x320
- Frame Rate: 15 fps (downsampled)
- Color Space: YUV 4:2:0
- Bandwidth: ~60-80 KB/s

## Troubleshooting

### "main profile doesn't support 4:2:2" Error

**Problem:** ffmpeg fails with error about H.264 main profile not supporting 4:2:2 chroma.

**Solution:** This is why we use the **FFmpeg Camera Plugin** with `-pix_fmt yuvj420p`. Make sure you configured Step 3 correctly.

### "Receiver reported picture loss"

**Problem:** Scrypted logs show "Adaptive Streaming: Receiver reported picture loss"

**Solution:** This is **normal** and not an error. It happens when:
- Network has minor packet loss
- Client requests keyframes
- Bitrate adjustments occur

### Stream Not Loading

**Problem:** Video preview in Scrypted doesn't load

**Solutions:**
1. Verify ESP32 is reachable: `ping 192.168.178.188`
2. Test RTSP directly in VLC first
3. Check Scrypted logs for ffmpeg errors
4. Ensure FFmpeg Camera plugin is installed
5. Verify FFmpeg Output Prefix is set correctly

### Low Frame Rate

**Problem:** Video appears choppy

**Solutions:**
1. Increase `-r 15` to `-r 20` or `-r 30` in FFmpeg Output Prefix
2. Increase `-b:v 500000` to `-b:v 1000000` for higher bitrate
3. Check network bandwidth between ESP32 and Scrypted

## Alternative: HTTP MJPEG Stream

If RTSP has issues, you can use the HTTP MJPEG stream instead:

```
http://192.168.178.188:81/stream
```

Configure in Scrypted:
1. Use **"FFmpeg Camera"** plugin
2. Set **URL**: `http://192.168.178.188:81/stream`
3. Use same **FFmpeg Output Prefix** as above

**Note:** HTTP MJPEG is simpler but less efficient than RTSP.

## HomeKit + FFmpeg Output Preset (Recommended)

### Recommended Settings

```
-c:v libx264 -pix_fmt yuvj420p -preset ultrafast -bf 0 -g 60 -r 15 -b:v 500000 -bufsize 1000000 -maxrate 500000
```

### Why This Preset Is Needed
The ESP32 outputs **MJPEG** (JPEG frames). HomeKit and Scrypted’s prebuffering expect **H.264** with regular keyframes and a compatible pixel format. Without re-encoding, Scrypted can’t find sync frames and HomeKit streams may fail to start.

This preset forces:
- **H.264 encoding** (`-c:v libx264`) so HomeKit accepts the stream.
- **YUV 4:2:0 pixel format** (`-pix_fmt yuvj420p`) which HomeKit requires.
- **Low-latency encoding** (`-preset ultrafast`) to reduce CPU and delay.
- **No B-frames** (`-bf 0`) to simplify decoding and improve compatibility.
- **Regular keyframes every 2 seconds** (`-g 60` at 15 fps) so Scrypted can sync quickly.
- **Stable 500 kbps bitrate** (`-b:v 500000 -maxrate 500000 -bufsize 1000000`) to limit bandwidth spikes.
- **Fixed 15 fps** (`-r 15`) which is easier on the ESP32 and HomeKit pipeline.

### Where to Set It
In Scrypted:
1. Open the **FFmpeg Camera** device.
2. Paste the preset into **FFmpeg Output Prefix**.
3. Keep the **Input** pointed at:
   - MJPEG: `http://<ESP32-IP>:81/stream`
   - Or RTSP: `rtsp://<ESP32-IP>:8554/mjpeg/1`

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
