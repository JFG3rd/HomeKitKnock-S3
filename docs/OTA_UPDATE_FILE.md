<!--
 Project: HomeKitKnock-S3
 File: docs/OTA_UPDATE_FILE.md
 Author: Jesse Greene
 -->

# OTA Update File Creation (Actionable Steps)

This guide shows how to **generate the OTA update files** (.bin) using PlatformIO.
Use these files in the `/ota` web interface.

## 1) Bump the Firmware Version (recommended)

Update the version string so the UI and logs reflect the new build:

- Edit `platformio.ini`
- Set `custom_fw_version` to the new version (example: `1.3.3`)

## 2) Build the Firmware OTA File (Flash)

### VS Code (PlatformIO)
1. Open the project in VS Code.
2. Run the PlatformIO **Build** task for `seeed_xiao_esp32s3`.

### CLI
```bash
pio run -e seeed_xiao_esp32s3
```

### Output File
```
.pio/build/seeed_xiao_esp32s3/firmware.bin
```

### Rename for OTA Upload (recommended)
Use a descriptive filename that includes the project name + version:

Example:
```
XIAOS3Sense-1.3.3-firmware.bin
```

CLI:
```bash
cp .pio/build/seeed_xiao_esp32s3/firmware.bin ./XIAOS3Sense-1.3.3-firmware.bin
```

This is the **firmware OTA** file (use `/ota/update` in the UI).

## 3) Build the Filesystem OTA File (LittleFS)

Only required when files in `data/` change (UI, CSS, HTML, etc.).

### VS Code (PlatformIO)
1. Run the PlatformIO **Build Filesystem Image** task.

### CLI
```bash
pio run -t buildfs -e seeed_xiao_esp32s3
```

### Output File
```
.pio/build/seeed_xiao_esp32s3/littlefs.bin
```

### Rename for OTA Upload (recommended)
Example:
```
XIAOS3Sense-1.3.3-littlefs.bin
```

CLI:
```bash
cp .pio/build/seeed_xiao_esp32s3/littlefs.bin ./XIAOS3Sense-1.3.3-littlefs.bin
```

This is the **filesystem OTA** file (use `/ota/fs` in the UI).

## 4) Quick Reference (Which File to Upload?)

- **Firmware update**: `firmware.bin`
- **Filesystem update**: `littlefs.bin` (only when `data/` changed)

## 5) Optional Sanity Checks

- Make sure the `.bin` files are recent (check timestamp).
- If the build fails, fix errors and rerun the build.
