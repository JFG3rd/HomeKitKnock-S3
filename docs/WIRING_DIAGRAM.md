<!--
 Project: HomeKitKnock-S3
 File: docs/WIRING_DIAGRAM.md
 Author: Jesse Greene
 -->

# Wiring Diagram (Rev A)

This diagram shows the current wiring for the ESP32-S3 Sense doorbell build.

```
Wiring map (signal -> destination)
----------------------------------
XIAO ESP32-S3 Sense   ->   Connection
3V3                  ->   3V3 rail -> MAX98357A Vin, Relay VCC, I2C VCC
GND                  ->   GND rail -> MAX98357A GND, Relay GND, I2C GND
GPIO4                ->   Doorbell switch -> GND
GPIO2                ->   330R -> LED anode -> GND
GPIO1                ->   Relay IN (door opener)
GPIO5                ->   I2C SDA (add 4.7k pullup to 3V3)
GPIO6                ->   I2C SCL (add 4.7k pullup to 3V3)
GPIO7                ->   MAX98357A BCLK Orange
GPIO8                ->   MAX98357A LRC/WS
GPIO9                ->   MAX98357A DIN
GPIO41               ->   PDM mic DATA (onboard)
GPIO42               ->   PDM mic CLK (onboard)

MAX98357A I2S DAC
----------------
Vin   -> 3V3
GND   -> GND
BCLK  -> GPIO7
LRC   -> GPIO8
DIN   -> GPIO9
SC/SD -> 3V3 (always on)
GAIN  -> float/strap per datasheet

Relay module (door opener)
--------------------------
VCC -> 3V3 (or 5V if your module accepts 3.3V on IN)
GND -> GND
IN  -> GPIO1
COM/NO -> door strike circuit (use strike supply voltage)

Speaker wiring
--------------
MAX98357A L+ -> Speaker +
MAX98357A L- -> Speaker -
NOTE: Do not connect speaker terminals to GND.
```

## Connection Summary

- **Doorbell switch (active-low):** GPIO4 to GND (internal pull-up in firmware).
- **Status LED:** GPIO2 -> 330 ohm resistor -> LED anode -> GND (active-high).
- **Door opener relay (active-high):** GPIO1 -> relay IN (use a relay module or transistor driver).
- **MAX98357A I2S DAC:**
  - **BCLK:** GPIO7
  - **LRC/WS:** GPIO8
  - **DIN:** GPIO9
  - **Vin:** 3V3
  - **GND:** GND
  - **SC/SD:** 3V3 (always on)
  - **GAIN:** leave floating or strap per MAX98357A datasheet
- **Speaker:** Connect to MAX98357A L+ and L-.
- **I2C header (optional):** GPIO5 (SDA), GPIO6 (SCL) with 4.7k pullups to 3V3 when used.
- **Onboard PDM mic:** GPIO42 (CLK), GPIO41 (DATA) - already on the XIAO Sense.
- **Onboard camera:** Uses dedicated camera pins (see `include/camera_pins.h`).

## Notes

- GPIO7/8/9 are the default SPI pins; avoid SPI when the DAC is wired.
- Use a relay module or transistor + flyback diode; do not drive a raw relay coil directly.
- Speaker outputs are bridged; never connect either speaker terminal to GND.
- Keep audio wires short to reduce noise.
