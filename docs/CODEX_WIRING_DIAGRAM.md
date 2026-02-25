<!--
 Project: HomeKitKnock-S3
 File: docs/CODEX_WIRING_DIAGRAM.md
 -->

# CODEX Wiring Diagram (Required Wiring Only)

This is the complete required wiring for the ESP32-S3 doorbell build.

## 1) ESP32-S3 Core IO

- **GPIO4** -> Doorbell switch (other side to GND, active-low)
- **GPIO2** -> Status LED (through resistor)
- **GPIO1** -> Door opener relay IN
- **GPIO3** -> Original gong relay IN
- **GPIO5** -> I2C SDA (optional peripherals)
- **GPIO6** -> I2C SCL (optional peripherals)

## 2) Shared I2S Audio Bus (Required)

### Clock lines (shared)
- **GPIO7** -> MAX98357A **BCLK** and INMP441 **SCK/BCLK**
- **GPIO8** -> MAX98357A **LRC/WS** and INMP441 **WS/LRCLK**

### Data lines (separate)
- **GPIO9** -> MAX98357A **DIN** (ESP32 TX)
- **GPIO12** <- INMP441 **SD/DOUT** (ESP32 RX)

## 3) MAX98357A Connections

- **VIN** -> 3V3
- **GND** -> GND
- **BCLK** -> GPIO7
- **LRC/WS** -> GPIO8
- **DIN** -> GPIO9
- **SD/SC** -> 3V3 (always enabled)
- **L+ / L-** -> Speaker + / Speaker -

## 4) INMP441 Connections

- **VDD** -> 3V3
- **GND** -> GND
- **SCK/BCLK** -> GPIO7
- **WS/LRCLK** -> GPIO8
- **SD/DOUT** -> GPIO12
- **L/R** -> GND (left channel)

## 5) Relay Wiring

### Door opener relay
- **VCC** -> 3V3 (or relay module logic supply)
- **GND** -> GND
- **IN** -> GPIO1
- **COM/NO** -> Door strike circuit

### Original 8VAC gong relay
- **VCC** -> 3V3 (or relay module logic supply)
- **GND** -> GND
- **IN** -> GPIO3
- **COM/NO** -> 8VAC gong circuit

## 6) Power and Ground

- All logic grounds must be common.
- Power MAX98357A and INMP441 from 3V3.
- Keep I2S wires short and cleanly routed.

## 7) Required Disconnects

- INMP441 **must not** be connected to GPIO43/GPIO44.
- Speaker terminals **must not** be connected to GND.
