# Phase 4: Video Path — Implementation Plan

## Overview
Add camera streaming to the ESP-IDF build: OV2640 initialization, JPEG snapshot,
MJPEG HTTP streaming (port 81), and RTSP server (port 8554) for Scrypted/HomeKit.

---

## Pre-requisites (Config Changes)

### 1. Enable PSRAM in sdkconfig
Camera frame buffers require PSRAM (VGA JPEG = ~30-50KB per frame, 2 buffers).
Must enable in `src_idf/sdkconfig`:
- `CONFIG_SPIRAM=y`
- `CONFIG_SPIRAM_MODE_OCT=y` (OPI mode for XIAO ESP32-S3)
- `CONFIG_SPIRAM_SPEED_80M=y`
- `CONFIG_SPIRAM_BOOT_INIT=y`
- Camera frame buffers allocated in PSRAM

### 2. Add esp32-camera library
Add to `platformio.ini` under `[env:seeed_xiao_esp32s3_idf]`:
```
lib_deps = espressif/esp32-camera
```
Plus CMakeLists.txt integration if needed.

### 3. Fix LEDC Channel Conflict
Status LED currently uses `LEDC_TIMER_0` / `LEDC_CHANNEL_0`.
Camera XCLK also needs a LEDC channel.
**Fix**: Move status LED to `LEDC_TIMER_1` / `LEDC_CHANNEL_1` (trivial change).

---

## New Components

### Component 1: `camera` (src_idf/components/camera/)
Wraps esp_camera for init, capture, and settings.

**Files:**
- `include/camera.h` — API header
- `camera.c` — Implementation
- `CMakeLists.txt` — Dependencies

**API:**
```c
esp_err_t camera_init(void);         // Init OV2640 with XIAO pins, VGA, JPEG
camera_fb_t *camera_capture(void);   // Get JPEG frame (caller must return it)
void camera_return_fb(camera_fb_t*); // Return frame buffer
bool camera_is_ready(void);          // Check if camera initialized
```

**Configuration:**
- XIAO ESP32-S3 pin map from `include/camera_pins.h`
- 10MHz XCLK, VGA (640x480), JPEG quality 10, 2 frame buffers in PSRAM
- `CAMERA_GRAB_LATEST` to always get the newest frame
- Uses LEDC_CHANNEL_0 / LEDC_TIMER_0 (status LED moves to channel 1)

### Component 2: `mjpeg_server` (src_idf/components/mjpeg_server/)
HTTP MJPEG streaming server on port 81, matching the Arduino version.

**Files:**
- `include/mjpeg_server.h` — API header
- `mjpeg_server.c` — Implementation
- `CMakeLists.txt`

**Architecture:**
- Runs a `lwip_socket`-based TCP server on port 81 (not httpd — raw sockets give
  better control for streaming, and avoids 2nd httpd instance overhead)
- Listener task on **core 1** accepts connections
- Per-client streaming tasks on **core 1** (max 2 concurrent)
- Each client task: send multipart headers, then loop `camera_capture()` → write → return fb
- Boundary: `multipart/x-mixed-replace;boundary=123456789000000000000987654321`
  (matches Arduino version so live.html works unchanged)

**API:**
```c
esp_err_t mjpeg_server_start(void);  // Start listener on port 81
void mjpeg_server_stop(void);        // Stop server and all clients
uint8_t mjpeg_server_client_count(void); // Active client count
```

### Component 3: `rtsp_server` (src_idf/components/rtsp_server/) — Phase 4b
RTSP server on port 8554 for Scrypted integration. Can be added after MJPEG works.

**Files:**
- `include/rtsp_server.h` — API header
- `rtsp_server.c` — RTSP protocol (DESCRIBE, SETUP, PLAY, TEARDOWN)
- `rtp_jpeg.c` — RFC 2435 JPEG/RTP packetization
- `CMakeLists.txt`

**Architecture:**
- TCP listener on port 8554, core 1
- RTP/RTCP over UDP (or TCP interleaved)
- Shares camera frames with MJPEG server via `camera_capture()`

---

## Integration in main.c

### Boot sequence addition:
After WiFi connects (deferred init pattern):
```
camera_init_pending = true;  // Set in WiFi event callback
```
In main loop:
```
if (camera_init_pending) {
    camera_init();
    mjpeg_server_start();
}
```

### Web server additions (port 80):
- `GET /capture` — Single JPEG snapshot (call camera_capture, send as image/jpeg)
- `GET /cameraStreamInfo` — JSON with streaming status, client count
- `GET /api/camera/status` — Sensor settings JSON

### Status LED integration:
- No changes needed — LED already tracks WiFi/SIP state
- Future: add RTSP active LED tick

---

## Implementation Order

| Step | What | Risk |
|------|------|------|
| 1 | Enable PSRAM in sdkconfig | Low — may need idf.py menuconfig |
| 2 | Fix LEDC conflict (move LED to ch1) | Trivial |
| 3 | Add esp32-camera lib dependency | Medium — PlatformIO IDF integration |
| 4 | Create camera component + init | Medium — pin config, PSRAM buffers |
| 5 | Add /capture endpoint (single JPEG) | Low — validates camera works |
| 6 | Create MJPEG server (port 81) | Medium — socket server + tasks |
| 7 | Test with live.html | Low — UI already exists |
| 8 | Add RTSP server (Phase 4b) | High — complex protocol, defer if needed |

---

## Risk Areas

1. **PSRAM configuration** — Must get sdkconfig right for OPI mode. Wrong settings = boot failure.
   Mitigation: Test with minimal camera init first.

2. **esp32-camera + PlatformIO ESP-IDF** — Library integration can be tricky.
   Mitigation: Use `lib_deps` first, fall back to git submodule if needed.

3. **Memory pressure** — Camera buffers + streaming tasks + SIP + web server.
   Current: 72KB RAM, 950KB flash. PSRAM gives 8MB for frame buffers.
   Mitigation: Frame buffers in PSRAM, limit concurrent streams to 2.

4. **LEDC conflict** — Camera XCLK needs LEDC. Status LED also uses LEDC.
   Mitigation: Assign different channels (addressed in step 2).

5. **Task stack sizes** — Streaming tasks need adequate stack.
   Mitigation: 4KB for MJPEG clients (minimal logic), 8KB for RTSP.
