<!--
 Project: HomeKitKnock-S3
 File: docs/OTA_UPDATE_FILE.md
 Author: Jesse Greene
 -->

# OTA Update File Creation (Actionable Steps)

This guide shows how to **generate the OTA update files** (.bin) using the
`tools/build_ota.py` helper. The script builds firmware + filesystem images,
renames them with the version string, and copies them into `dist/ota/` for easy upload.

## 1) Bump the Firmware Version (recommended)

Update the version string so the UI and logs reflect the new build:

- Edit `platformio.ini`
- Set `custom_fw_version` to the new version (example: `1.3.3`)

## 2) Build OTA Files with the Helper Script (recommended)

### CLI
```bash
python3 tools/build_ota.py
```

### Output Files (default)
```
dist/ota/XIAOS3Sense-<version>-firmware.bin
dist/ota/XIAOS3Sense-<version>-littlefs.bin
```

These are the **OTA upload files**:
- Firmware: use `/ota/update`
- Filesystem: use `/ota/fs` (only if `data/` changed)

### Options
```bash
python3 tools/build_ota.py --env seeed_xiao_esp32s3
python3 tools/build_ota.py --prefix XIAOS3Sense
python3 tools/build_ota.py --version 1.3.3
python3 tools/build_ota.py --skip-firmware
python3 tools/build_ota.py --skip-fs
```

## 3) Manual Build (fallback)

If you prefer manual PlatformIO commands, use the steps below.

### Build Firmware (Flash)
```bash
pio run -e seeed_xiao_esp32s3
```

### Build Filesystem (LittleFS)
```bash
pio run -t buildfs -e seeed_xiao_esp32s3
```

### Output Files
```
.pio/build/seeed_xiao_esp32s3/firmware.bin
.pio/build/seeed_xiao_esp32s3/littlefs.bin
```

### Rename for OTA Upload (optional)
Use a descriptive filename that includes the project name + version:

Example:
```
XIAOS3Sense-1.3.3-firmware.bin
```

Examples:
```bash
cp .pio/build/seeed_xiao_esp32s3/firmware.bin ./XIAOS3Sense-1.3.3-firmware.bin
cp .pio/build/seeed_xiao_esp32s3/littlefs.bin ./XIAOS3Sense-1.3.3-littlefs.bin
```

## 4) Quick Reference (Which File to Upload?)

- **Firmware update**: `firmware.bin`
- **Filesystem update**: `littlefs.bin` (only when `data/` changed)

## 5) Optional Sanity Checks

- Make sure the `.bin` files are recent (check timestamp).
- If the build fails, fix errors and rerun the build.
