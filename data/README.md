# Data Directory

This directory contains static files that can be served by the ESP32-S3 web server.

## Files

### style.css
Main stylesheet for the web interface. Includes:
- Responsive design for mobile and desktop
- Dark mode support with toggle
- WiFi setup page styling
- Log container with scrolling
- Button and form styling
- Adapted from the JFG Gesture Controller project

## Usage

### LittleFS (Current)
Upload files to the ESP32-S3 flash filesystem:
1. Run: `pio run -t uploadfs`
2. Files will be available at their respective URLs (e.g., `/style.css`)

## PlatformIO Config
Ensure the filesystem is set to LittleFS in `platformio.ini`:
```ini
board_build.filesystem = littlefs
```
