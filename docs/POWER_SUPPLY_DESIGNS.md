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
    └──→ Supercap Bank (2.7V, 10F total) + Ideal Diode (LTC4412)
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
| 2 | 5.5F 2.7V | Supercap (series = 10F @ 5.4V) | DigiKey/Mouser | 2× 3.50 = 7.00 |
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

## Option 2: Li-Po Battery with Power-Path

Compact, high energy density, enables multi-hour ride-through.

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
    ↓
MCP73871 (Li-Po charger + power-path)
    ↓                   ↓
   5V out          Li-Po 3.7V 2000 mAh
    ↓
Buck Converter (MP1584 module) → 3.3V @ 2A
    ↓
ESP32-S3 System
```

### Design Calculations

Battery capacity for 3 s ride-through at 200 mA average (3.3V rail):
$$Q = I \cdot t = 0.2\,\text{A} \times \frac{3}{3600}\,\text{h} = 0.17\,\text{mAh}$$

A 500 mAh Li-Po provides 2,941× margin (or ~4 hours runtime at 200 mA).

Charge time from empty (500 mAh @ 250 mA charge current):
$$t = \frac{500}{250} \times 1.2 = 2.4\,\text{hours}$$

### BOM: Li-Po Version

| Qty | Part | Description | Supplier | Est. Price (EUR) |
|-----|------|-------------|----------|------------------|
| 1 | DF04M | Bridge rectifier 1A 400V | DigiKey/Mouser | 0.50 |
| 1 | 2200µF 16V | Electrolytic cap (low ESR) | DigiKey/Mouser | 0.80 |
| 1 | LM2596 module | Buck 12V→5V @ 1.5A | Amazon/AliExpress | 1.50 |
| 1 | MCP73871 | Li-Po charger + power-path IC | DigiKey/Mouser | 3.50 |
| 1 | Li-Po 3.7V 2000mAh | JST connector (103450 size) | Amazon/AliExpress | 8.00 |
| 2 | 10 µF 6.3V X5R | Ceramic caps (MCP73871 I/O) | DigiKey/Mouser | 2× 0.20 = 0.40 |
| 3 | Resistor | PROG, ISET2 (see MCP73871 datasheet) | DigiKey/Mouser | 0.15 |
| 1 | MP1584 module | Buck 5V→3.3V @ 2A | Amazon/AliExpress | 1.20 |
| 1 | 1N5819 | Schottky diode 1A 40V | DigiKey/Mouser | 0.20 |
| | | Fuse holder + 500 mA slow-blow | DigiKey/Mouser | 1.00 |
| | | **Total** | | **~17 EUR** |

### Notes

- MCP73871 provides seamless power-path: system runs from 5V input; battery charges in background; if input drops, battery takes over instantly.
- Set charge current to 250–500 mA (adjust PROG resistor) based on transformer VA rating.
- Li-Po requires protection circuit (most packs include PCM).
- Optional: add a fuel gauge IC (e.g., MAX17043) for battery % monitoring.

---

## Option 3: Solar Panel + Mini Powerbank

Off-the-shelf solution using consumer products; no custom PCB design.

### Concept

- Use a 5V solar panel (5W, e.g., the linked Amazon DE product) to charge a mini USB powerbank.
- Powerbank has built-in Li-Po battery (2000–5000 mAh) + charge controller + 5V output.
- Powerbank continuously powers the ESP32-S3 via its 5V USB output → buck to 3.3V.
- Solar panel keeps powerbank topped off during the day.
- At night or during prolonged cloudy periods, the battery provides multi-hour runtime.

### Block Diagram

```
Solar Panel (5V 1A)
    ↓
Mini Powerbank (2000–5000 mAh)
    ↓
5V USB output
    ↓
Buck Converter (MP1584 module) → 3.3V @ 2A
    ↓
ESP32-S3 System
```

### Design Calculations

Average daily consumption (assume 50% duty cycle at 200 mA, 50% at 100 mA):
$$Q_{\text{daily}} = (0.2 \times 12 + 0.1 \times 12) = 3.6\,\text{Ah} @ 3.3\text{V} = 11.88\,\text{Wh}$$

At 5V output:
$$Q_{\text{daily}} = \frac{11.88}{5} = 2.38\,\text{Ah} @ 5\text{V}$$

A 5000 mAh powerbank provides 2.1 days of runtime at this consumption.

Solar panel energy (5W, 5 hours effective sun):
$$E_{\text{solar}} = 5\,\text{W} \times 5\,\text{h} = 25\,\text{Wh}$$

This is 2.1× daily consumption → sufficient margin for cloudy days.

### BOM: Solar + Powerbank Version

| Qty | Part | Description | Supplier | Est. Price (EUR) |
|-----|------|-------------|----------|------------------|
| 1 | Solar panel 5V 5W | Monocrystalline, weatherproof | [Amazon DE](https://www.amazon.de/dp/B0F2TFQQQC) | 15.00 |
| 1 | Mini powerbank 5000mAh | USB-A output, micro-USB/USB-C input | Amazon/AliExpress | 12.00 |
| 1 | MP1584 module | Buck 5V→3.3V @ 2A | Amazon/AliExpress | 1.20 |
| 1 | USB-A breakout cable | Male USB-A to screw terminals | Amazon/AliExpress | 2.00 |
| | | **Total** | | **~30 EUR** |

### Notes

- **Pros:** No custom power design; powerbank handles all charging/protection; solar panel is weatherproof.
- **Cons:** Higher cost; bulkier; powerbank may have efficiency losses; limited outdoor mounting options.
- **Mounting:** Solar panel must be outdoors (south-facing for Northern Hemisphere); run cable indoors to powerbank + ESP32.
- **Powerbank selection:** Choose one with "pass-through charging" (powers output while charging) and low standby drain.
- **Lifespan:** Li-Po in powerbank has finite cycle life (~500 charge cycles); expect replacement every 2–3 years.

---

## Comparison & Recommendations

| Criterion | Supercap | Li-Po Power-Path | Solar + Powerbank |
|-----------|----------|------------------|-------------------|
| **Cost** | ~15 EUR | ~17 EUR | ~30 EUR |
| **Complexity** | Moderate (balancing resistors) | Moderate (SMD soldering for MCP73871) | Low (plug-and-play) |
| **Ride-Through** | 3–45 s | 3 s – 4 hours | 1–2 days |
| **Lifespan** | 10+ years | 2–5 years (Li-Po aging) | 2–3 years (powerbank replacement) |
| **Form Factor** | Compact | Compact | Bulky (solar panel + powerbank) |
| **Safety** | Very safe (no Li chemistry) | Li-Po fire risk if abused | Li-Po in powerbank (has protection) |
| **Use Case** | Short dropouts (gong only) | Extended outages (hours) | Off-grid / no AC power |

### Recommended Approach

1. **For typical doorbell use (8VAC transformer available):** Use **Option 1 (Supercap)**.
   - Cheapest, safest, longest lifespan.
   - 45 s ride-through is more than enough for gong relay actuation.

2. **For extended backup (power outages common):** Use **Option 2 (Li-Po Power-Path)**.
   - Provides hours of runtime during blackouts.
   - Automatic failover with MCP73871.

3. **For off-grid or outdoor installations:** Use **Option 3 (Solar + Powerbank)**.
   - No AC transformer needed.
   - Solar panel handles daily recharge; battery provides overnight operation.

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

