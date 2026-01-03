# ESP32-S3 Doorbell Firmware (PlatformIO)

![License](https://img.shields.io/github/license/JFG3rd/HomeKitKnock-S3)

Bootstrap README for the ESP32-S3 firmware in this folder.

## Prerequisites
- VS Code with the PlatformIO extension, or the PlatformIO CLI (`pio`).
- USB data cable for the Seeed Studio XIAO ESP32-S3 Sense.

## Quick Start
1. Open this folder in VS Code (PlatformIO will detect the project).
2. Build:
   - VS Code: PlatformIO "Build" task
   - CLI: `pio run`
3. Upload:
   - VS Code: PlatformIO "Upload" task
   - CLI: `pio run -t upload`
4. Serial monitor (adjust baud if needed):
   - VS Code: PlatformIO "Monitor" task
   - CLI: `pio device monitor -b 115200`

## PlatformIO Configuration
The environment is defined in `platformio.ini` and targets:
- `board = seeed_xiao_esp32s3`
- `framework = arduino`

Key flags for the ESP32-S3 Sense:
- PSRAM enabled via `-DBOARD_HAS_PSRAM`
- `board_build.psram_mode = opi`
- `board_build.flash_mode = qio`

## Project Layout
- `src/` firmware source (`src/main.cpp`)
- `include/` headers
- `lib/` local libraries
- `test/` tests

## Notes
- If uploads fail, select the correct serial port in PlatformIO.
- Keep `platformio.ini` aligned with the board memory configuration (PSRAM and flash mode).

## Contributing
See `CONTRIBUTING.md`.

## License
Apache-2.0. See `LICENSE`.
