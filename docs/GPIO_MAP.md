<!--
 Project: HomeKitKnock-S3
 File: docs/GPIO_MAP.md
 Author: Jesse Greene
 -->

# GPIO Map - XIAO ESP32-S3 Sense

This document captures the header pin mapping and current project usage.
For a complete wiring diagram, see `docs/WIRING_DIAGRAM.md`.
Source mapping: PlatformIO XIAO_ESP32S3 variant (`pins_arduino.h`).

## Header Pin Mapping (Arduino Label -> GPIO)
- D0 / A0 -> GPIO1
- D1 / A1 -> GPIO2
- D2 / A2 -> GPIO3
- D3 / A3 -> GPIO4
- D4 / A4 -> GPIO5
- D5 / A5 -> GPIO6
- D6 -> GPIO43 (UART TX)
- D7 -> GPIO44 (UART RX)
- D8 / A8 -> GPIO7
- D9 / A9 -> GPIO8
- D10 / A10 -> GPIO9

## Current Project Assignments
- Doorbell button: GPIO4 (active-low, internal pull-up)
- Status LED: GPIO2 (active-high, 330 ohm to LED)
- Door opener relay: GPIO1 (active-high)
- Original 8VAC gong relay: GPIO3 (active-high)
- I2C (reserved): GPIO5 = SDA, GPIO6 = SCL
- I2S DAC (audio out): GPIO7 = BCLK, GPIO8 = LRCLK/WS, GPIO9 = DIN
- PDM mic (onboard): GPIO42 = CLK, GPIO41 = DATA
- Camera pins: GPIO10/11/12/13/14/15/16/17/18/38/39/40/47/48 (see `include/camera_pins.h`)
- Onboard LED (not on header): GPIO21 (variant default)

## Free Header GPIOs (with current wiring)
- GPIO43 (D6) and GPIO44 (D7) if UART TX/RX is not needed

## Suggested Usage for Free GPIOs
- External UART device: GPIO43/GPIO44 (if you are not using serial TX/RX)

## Notes
- GPIO7/8/9 are the default SPI pins; avoid SPI use if I2S DAC is wired.
- Camera pins are dedicated; avoid reassigning unless you remove the camera.
- If repurposing pins, verify against the Seeed schematic and ESP32-S3 datasheet.
