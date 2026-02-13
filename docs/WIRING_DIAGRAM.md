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
GPIO3                ->   Relay IN (8VAC gong)
GPIO5                ->   I2C SDA (add 4.7k pullup to 3V3)
GPIO6                ->   I2C SCL (add 4.7k pullup to 3V3)
GPIO7                ->   MAX98357A BCLK Orange
GPIO8                ->   MAX98357A LRC/WS
GPIO9                ->   MAX98357A DIN
GPIO41               ->   PDM mic DATA (onboard)
GPIO42               ->   PDM mic CLK (onboard)
GPIO43               ->   INMP441 SCK/BCLK (external I2S mic)
GPIO44               ->   INMP441 WS/LRCLK (external I2S mic)
GPIO12               ->   INMP441 SD/DOUT (external I2S mic, back expansion)

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

Relay module (original 8VAC gong)
---------------------------------
VCC -> 3V3 (or 5V if your module accepts 3.3V on IN)
GND -> GND
IN  -> GPIO3
COM/NO -> 8VAC gong circuit (use the gong transformer voltage)

INMP441 I2S Microphone (external, offboard)
----------------------------------------------
VDD   -> 3V3
GND   -> GND
SCK   -> GPIO43 (D6 / TX)
WS    -> GPIO44 (D7 / RX)
SD    -> GPIO12 (D11, back expansion connector)
L/R   -> GND (mono left channel)

Speaker wiring
--------------
MAX98357A L+ -> Speaker +
MAX98357A L- -> Speaker -
NOTE: Do not connect speaker terminals to GND.

Power Supply (Supercapacitor Ride-Through)
-------------------------------------------
8VAC Transformer
    ↓
Fuse (500 mA slow-blow)
    ↓
Bridge Rectifier (DB107 or KBL406) → ~11.3V DC unregulated
    ↓
Bulk Cap (2× 1000µF 16V + 1× 220µF 16V in parallel = ~2220µF)
    ↓
MP1584EN Buck Module → 5V @ 2A (accepts 4.5–28V input; 95% efficient)
    ↓
Supercap (Lumonic 4F 5.5V radial goldcap)
    ↓
MP1584EN Buck Module → 3.3V @ 2A
    ↓
ESP32-S3 + MAX98357A + Relays (3V3 rail)

Supercap notes:
- Single 4F 5.5V goldcap directly on 5V rail (no series connection needed)
- Provides ~18 second ride-through during gong relay activation
- Radial form factor: 24.5 mm Ø × 6 mm height
- 3.8 mm pin pitch (standard breadboard/perfboard compatible)
- No balancing resistors or ideal diode circuit required
```

## Connection Summary

- **Doorbell switch (active-low):** GPIO4 to GND (internal pull-up in firmware).
- **Status LED:** GPIO2 -> 330 ohm resistor -> LED anode -> GND (active-high).
- **Door opener relay (active-high):** GPIO1 -> relay IN (use a relay module or transistor driver).
- **Original 8VAC gong relay (active-high):** GPIO3 -> relay IN (use a relay module rated for AC).
- **MAX98357A I2S DAC:**
  - **BCLK:** GPIO7
  - **LRC/WS:** GPIO8
  - **DIN:** GPIO9
  - **Vin:** 3V3
  - **GND:** GND
  - **SC/SD:** 3V3 (always on)
  - **GAIN:** leave floating (9 dB fixed gain; volume controlled in software via ESP-ADF pipeline)
- **Speaker:** Connect to MAX98357A L+ and L-.
- **I2C header (optional):** GPIO5 (SDA), GPIO6 (SCL) with 4.7k pullups to 3V3 when used.
- **INMP441 I2S Microphone (external):**
  - **SCK/BCLK:** GPIO43 (D6)
  - **WS/LRCLK:** GPIO44 (D7)
  - **SD/DOUT:** GPIO12 (D11, back expansion)
  - **L/R:** GND (mono left)
  - **VDD:** 3V3
  - **GND:** GND
- **Onboard PDM mic:** GPIO42 (CLK), GPIO41 (DATA) - already on the XIAO Sense.
- **Mic source selection:** Configured in setup page (onboard PDM or external I2S INMP441). Stored in NVS.
- **Onboard camera:** Uses dedicated camera pins (see `include/camera_pins.h`).

## Notes

- GPIO7/8/9 are the default SPI pins; avoid SPI when the DAC is wired.
- Current firmware uses I2S1 for both MAX98357A (speaker output) and INMP441 (mic input), so they are mutually exclusive.
- Onboard PDM mic uses I2S0 and does not conflict with MAX98357A speaker playback.
- All ESP32-S3 I2S pins are fully routable via GPIO matrix — no fixed pin assignments.
- Use a relay module or transistor + flyback diode; do not drive a raw relay coil directly.
- The 8VAC gong relay is electrically isolated from the ESP32; only the relay contacts touch the AC circuit.
- Speaker outputs are bridged; never connect either speaker terminal to GND.
- Keep audio wires short to reduce noise.

## Free Header GPIOs

- **GPIO13 (D12, back expansion)** — only remaining free GPIO on headers.
- GPIO43 and GPIO44 are now assigned to the INMP441 external mic.
- GPIO12 is used for INMP441 data line via back expansion.

## Power Supply Details

The system uses an 8VAC transformer as the primary power source, with a supercapacitor ride-through circuit to prevent brownouts when the gong relay activates. See [POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md) for full design and BOM.
