<!--
 Project: HomeKitKnock-S3
 File: docs/WIRING_DIAGRAM.md
 Author: Jesse Greene
 -->

# Wiring Diagram (Rev A)

This diagram shows the current wiring for the ESP32-S3 Sense doorbell build.

```
                          3V3 rail -------------------+--------------------+
                                                     |                    |
                                                     |                    |
                                            +--------v--------+   +-------v------+
                                            | MAX98357A I2S   |   | I2C Header   |
                                            | DAC Amp         |   | (optional)   |
                                            |                 |   |              |
         +-------------------------------+  | Vin  <----------+   | SDA <--- GPIO5
         | XIAO ESP32-S3 Sense           |  | GND  <----------+---+ SCL <--- GPIO6
         |                               |  | BCLK <--- GPIO7 |      (add 4.7k pullups to 3V3)
         | 3V3 --------------------------+--| LRC  <--- GPIO8 |
         | GND ------------------------------| DIN  <--- GPIO9 |
         | GPIO4  --[doorbell switch]-- GND  | SC   <----------+ (tie to 3V3)
         | GPIO2  --[330R]-->|-- GND         | GAIN (float/strap per datasheet)
         | GPIO5  --------------------------+------------------+
         | GPIO6  --------------------------+
         | GPIO7  --------------------------+
         | GPIO8  --------------------------+
         | GPIO9  --------------------------+
         | GPIO41 (PDM DATA, onboard mic)   |
         | GPIO42 (PDM CLK, onboard mic)    |
         | Camera pins (onboard OV2640)     |
         +-------------------------------+

Speaker wiring:
MAX98357A L+  ----------------->  Speaker +
MAX98357A L-  ----------------->  Speaker -
NOTE: Do not connect speaker terminals to GND.
```

## Connection Summary

- **Doorbell switch (active-low):** GPIO4 to GND (internal pull-up in firmware).
- **Status LED:** GPIO2 -> 330 ohm resistor -> LED anode -> GND (active-high).
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
- Speaker outputs are bridged; never connect either speaker terminal to GND.
- Keep audio wires short to reduce noise.
