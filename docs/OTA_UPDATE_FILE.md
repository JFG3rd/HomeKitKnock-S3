<!--
 Project: HomeKitKnock-S3
 File: docs/OTA_UPDATE_FILE.md
 Author: Jesse Greene
 Last Updated: 2026-03-10
 -->

# OTA Firmware Update Guide

This project uses **gzip-embedded C arrays** for all web assets — there is no separate filesystem
partition. Only a single `firmware.bin` file is needed for OTA updates.

---

## Prerequisites

- Device is on the local network and you know its IP address
- You have your device **username** and **password** (set during first-time setup)
- OTA must be explicitly enabled from the Setup page before each upload

---

## Step 1: Build the Firmware

### If web assets changed (`data/*.html`, `data/style.css`)
Regenerate the embedded headers before building:
```bash
python3 tools/embed_web_assets.py data/ include/
python3 tools/embed_web_assets.py data/ src_idf/components/embedded_web_assets/
python3 tools/embed_web_assets.py data/ src_idf/main/generated/
```

### Build
```bash
pio run
```

### Output file
```
.pio/build/seeed_xiao_esp32s3_idf/firmware.bin
```
> If a custom `build_dir` is set in `platformio.ini`, the path may be:
> `/tmp/pio-doorbell-build/seeed_xiao_esp32s3_idf/firmware.bin`

---

## Step 2: Enable OTA from the Setup Page

1. Open `http://<device-ip>/setup` in a browser
2. Enter your **username** and **password** when prompted
3. In the **OTA Updates** card, click **Enable OTA (5 min)**
4. Status should change to: `enabled (4m 59s)`

The OTA window stays open for **5 minutes**. You must upload before it closes.

---

## Step 3: Upload Firmware

1. From the Setup page, click **Open OTA Page** (same tab — credentials are preserved)
2. Click **Choose Firmware File** and select `firmware.bin`
3. Click **Upload Firmware**
4. Watch the progress bar — upload takes ~10–15 seconds over WiFi
5. On success: `✅ Firmware uploaded successfully. Device will reboot in ~5s.`
6. Device reboots, reconnects to WiFi, and loads the new firmware

---

## Optional: Bump Version Before Building

To change the version string shown in the UI and logs:

1. Edit `platformio.ini`
2. Update `custom_fw_version` (e.g. `1.4.0`)
3. Rebuild and upload

---

## Flashing via USB (Direct)

For initial flashing or when OTA is unavailable:
```bash
# Kill serial monitor if running
kill $(lsof -t /dev/cu.usbmodem21201) 2>/dev/null

# Flash
pio run -t upload

# Monitor serial output
pio device monitor
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Upload button disabled | OTA window not enabled, or no file selected | Click "Enable OTA (5 min)" first |
| `❌ OTA window is not open` | 5-min window expired | Re-enable OTA from Setup |
| Upload fails at 100% | Firmware too large or flash write error | Check serial logs for details |
| Device doesn't reboot | OTA partition mismatch | Flash via USB: `pio run -t upload` |
| Login overlay appears on OTA page | Session expired (new tab) | Enter username + password |
