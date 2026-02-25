<!--
 Project: HomeKitKnock-S3
 File: docs/POWER_SUPPLY_DESIGNS.md
 Author: Jesse Greene
 -->

# Power Supply Design Options

This document presents three alternative power supply designs for running the ESP32-S3 doorbell from the existing 8VAC transformer, with ride-through capability to survive gong relay activation.

## Power Budget

### Estimated Current Draw @ 3.3V Rail

| Component | Idle | Active | Peak |
|-----------|------|--------|------|
| ESP32-S3 (Wi-Fi active) | 80 mA | 150 mA | 350 mA |
| Camera (QQVGA streaming) | 0 mA | 100 mA | 150 mA |
| MAX98357A (audio playback) | 5 mA | 150 mA | 300 mA |
| Relay #1 (door opener) | 0 mA | 60 mA | 70 mA |
| Relay #2 (gong) | 0 mA | 60 mA | 70 mA |
| Status LED | 0 mA | 15 mA | 15 mA |
| **Total** | **~100 mA** | **~400 mA** | **~800 mA** |

### Ride-Through Requirements

- **Gong relay activation time:** 1–3 seconds typical
- **Minimum hold-up time:** 3 seconds (conservative)
- **Maximum allowed voltage drop:** 0.3 V (3.3 V → 3.0 V safe for ESP32-S3)
- **Average current during dropout:** 200 mA (idle + some activity)

---

## Option 1: Supercapacitor Ride-Through

Simple, no battery chemistry concerns, long lifetime.

### Block Diagram

```
8VAC Transformer
    ↓
Bridge Rectifier (DF04M)
    ↓
~11.3V DC (unregulated)
    ↓
Bulk Cap (2200 µF)
    ↓
Buck Converter (LM2596 module) → 5V @ 1.5A
    ↓                              ↓
    └────────────────────→ SupercapBank (5V, 4F) 
                                   ↓
                            Buck Converter (MP1584 module) → 3.3V @ 2A
                                   ↓
                            ESP32-S3 System
```

### Design Calculations

Energy required for 3 s ride-through at 200 mA average (5 V rail):
$$E = P \cdot t = (5\,\text{V} \times 0.2\,\text{A}) \times 3\,\text{s} = 3\,\text{J}$$

Supercap energy storage:
$$E = \frac{1}{2} C (V_{\text{max}}^2 - V_{\text{min}}^2)$$

For $V_{\text{max}} = 5.0\,\text{V}$, $V_{\text{min}} = 4.0\,\text{V}$ (before buck dropout), $C = 10\,\text{F}$:
$$E = \frac{1}{2} \times 10 \times (5.0^2 - 4.0^2) = 45\,\text{J}$$

**Result:** 10 F @ 5 V provides 15× margin; sufficient for 45 seconds ride-through.

### BOM: Supercap Version

| Qty | Part | Description | Supplier | Est. Price (EUR) |
|-----|------|-------------|----------|------------------|
| 1 | DF04M | Bridge rectifier 1A 400V | DigiKey/Mouser | 0.50 |
| 1 | 2200µF 16V | Electrolytic cap (low ESR) | DigiKey/Mouser | 0.80 |
| 1 | 4F 5V | Supercap 
| 1 | LM2596 module | Buck 12V→5V @ 1.5A (Vin 4.5–40V) | Amazon/AliExpress | 1.50 |
| 1 | LTC4412 | Ideal diode controller + Schottky MOSFET | DigiKey/Mouser | 2.50 |
| 1 | MP1584 module | Buck 5V→3.3V @ 2A | Amazon/AliExpress | 1.20 |
| 1 | 10 kΩ NTC | Inrush limiter (optional) | DigiKey/Mouser | 0.30 |
| 1 | 1N5819 | Schottky diode 1A 40V (polarity protection) | DigiKey/Mouser | 0.20 |
| | | Fuse holder + 500 mA slow-blow | DigiKey/Mouser | 1.00 |
| | | **Total** | | **~15 EUR** |

### Notes

- Series supercaps: use balancing resistors (100–220 Ω) across each cap.
- Ideal diode allows supercaps to discharge into 3.3V rail when 5V input drops.
- Inrush limiter (NTC) protects the bridge rectifier at power-on.
- Buck modules are pre-built; can substitute discrete LM2596/MP1584 circuits.

---

## Next Steps

1. **Measure actual power consumption:**
   - Use a USB power monitor (e.g., UM25C) or INA219 breakout on the 3.3V rail.
   - Log current during: idle, camera streaming, audio playback, relay activation.
   - Confirm the estimates in the power budget table above.

2. **Prototype on breadboard:**
   - Build Option 1 (Supercap) first as a proof-of-concept.
   - Test gong relay activation while monitoring 3.3V rail with an oscilloscope.
   - Verify no brownouts or resets occur.

3. **Design PCB (optional):**
   - Integrate rectifier + buck + supercap/Li-Po onto a custom board.
   - Add screw terminals for 8VAC input and relay outputs.
   - Include status LEDs for power/charge indicators.

4. **Update wiring diagram:**
   - Add power supply section showing transformer → rectifier → buck → ESP32.
   - Document any GPIO changes (e.g., battery monitor ADC on GPIO1/2).

5. **Safety testing:**
   - Ensure electrical isolation between AC side (gong relay) and DC side (ESP32).
   - Use a fused 8VAC input and TVS diodes on the DC bus.
   - Enclosure must be non-conductive and protect user from AC terminals.

---

## References

- [LM2596 Datasheet (Buck Converter)](https://www.ti.com/lit/ds/symlink/lm2596.pdf)
- [MCP73871 Datasheet (Li-Po Charger + Power-Path)](https://ww1.microchip.com/downloads/en/DeviceDoc/20002090C.pdf)
- [LTC4412 Datasheet (Ideal Diode)](https://www.analog.com/media/en/technical-documentation/data-sheets/4412fb.pdf)
- [MAX98357A Datasheet (I2S DAC)](https://datasheets.maximintegrated.com/en/ds/MAX98357A-MAX98357B.pdf)
- [ESP32-S3 Power Consumption](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) (Section 3.2)

