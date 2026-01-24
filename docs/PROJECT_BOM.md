<!--
 Project: HomeKitKnock-S3
 File: docs/PROJECT_BOM.md
 Author: Jesse Greene
 -->

# Bill of Materials (BOM) — Complete Project

Complete parts list for the ESP32-S3 doorbell with supercapacitor-based power supply, sourcing suggestions, and estimated total cost.

---

## 1. Core Compute & Sensing

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Seeed Studio XIAO ESP32-S3 Sense | Microcontroller | [Seeed](https://www.seeedstudio.com/XIAO-ESP32-S3-Sense-p-5639.html), [DigiKey](https://www.digikey.com/), Amazon | Includes OV2640 camera, PDM mic, 8 MB flash, PSRAM | 25.00 |
| 1 | USB Type-C cable (data-capable) | Connectivity | Local / Amazon | For programming and serial monitor | 3.00 |

**Subtotal:** €28.00

---

## 2. Audio Hardware

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Adafruit MAX98357A I2S DAC Breakout | Audio DAC | [Adafruit](https://www.adafruit.com/product/3006), [DigiKey](https://www.digikey.com/), Amazon | 3.2W class-D amp, 3V3–5V, I2S audio | 8.00 |
| 1 | Small speaker (4–8 Ω, 0.5–1W) | Speaker | Local / Amazon | 40–50 mm diameter, 3–5 cm depth | 2.50 |
| 1 | Microphone (PDM) | Microphone | Onboard (XIAO) | Seeed XIAO ESP32-S3 Sense includes PDM mic | 0.00 |

**Subtotal:** €10.50

---

## 3. Button, LED & Mechanical

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Momentary push button (normally open) | Switch | Local / Amazon | 12 mm diameter, SPST | 1.00 |
| 1 | Red LED (3 mm) | LED | Local / Amazon | Indicator (online/activity status) | 0.20 |
| 1 | Resistor 330 Ω, 1/4W | Resistor | Local / Amazon | LED current limit | 0.05 |
| 1 | Plastic or aluminum enclosure | Enclosure | Local / Amazon | IP54 or better, ~150×100×80 mm | 8.00 |
| 1 | Weatherproof IP67 cable gland (6–8 mm) | Connector | Local / Amazon | For 8VAC transformer input | 1.50 |

**Subtotal:** €10.75

---

## 4. Relay Modules & Drivers

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | 1-channel 5V relay module (SRD-05VDC-SL-C) | Relay | Amazon / AliExpress | 5V coil, 10A @ 250VAC contacts; use for door opener | 2.00 |
| 1 | 1-channel 5V relay module (SRD-05VDC-SL-C) | Relay | Amazon / AliExpress | 5V coil, 10A @ 250VAC contacts; use for 8VAC gong | 2.00 |
| 2 | Transistor BJT NPN (2N2222 or 2N3904) | Driver | Local / Amazon | Optional; for direct GPIO→relay drive | 0.20 |
| 2 | Resistor 10 kΩ, 1/4W | Resistor | Local / Amazon | Base resistor for transistor drivers | 0.10 |
| 2 | Diode 1N4007 | Diode | Local / Amazon | Flyback / clamp diode across relay coils | 0.20 |

**Subtotal:** €4.70

---

## 5. Power Supply — Supercapacitor Ride-Through

### 5A. Bridge Rectifier & First-Stage Filtering

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | DB107 Bridge Rectifier (DIP) | Rectifier | DigiKey / Mouser / Amazon | **Recommended:** 1A @ 1000V; DIP package; very common & cheap | 0.30 |
| 1 | KBL406 Bridge Rectifier (case) | Rectifier | DigiKey / Mouser / Amazon | **Higher current:** 4A @ 600V; larger case; excellent availability | 0.50 |
| 1 | DF04M Bridge Rectifier | Rectifier | DigiKey / Mouser | 1A @ 400V; compact SMD (original spec) | 0.50 |
| 2 | Electrolytic capacitor 1000 µF 16V | Capacitor | Local (on hand) | Bulk filtering (in parallel) | 0.00 |
| 1 | Electrolytic capacitor 220 µF 16V | Capacitor | Local (on hand) | Bulk filtering (in parallel); total = ~2220µF | 0.00 |
| 1 | Fuse 500 mA slow-blow + fuse holder | Fuse | Local / Amazon | Input protection; 8VAC line | 1.00 |
| 1 | NTC thermistor 10 kΩ | Inrush limiter | DigiKey / Mouser | Protects bridge on power-on | 0.30 |

**Subtotal:** €2.10 (DB107, using existing caps)

### 5B. Supercapacitor

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Lumonic 4F 5.5V goldcap (radial) | Supercap | [Amazon.de](https://www.amazon.de/Lumonic-Supercap-Direct-Capacity-Monoblock/dp/B0FZVKCV9K/) | **Single unit:** 4F @ 5.5V DC; radial 3.8 mm pitch; 24.5 mm Ø × 6 mm H; ~18s ride-through | 3.50 |

**Supercap circuit notes:**
- Connect directly across 5V rail (after first buck stage)
- No series connection or balancing resistors needed
- No ideal diode circuit required (capacitor naturally blocks reverse current)
- Provides ~18 second hold-up at 200 mA load

**Subtotal:** €3.50

### 5C. Buck Converters

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 2 | MP1584EN buck module | Buck converter | [Amazon.de](https://www.amazon.de/-/en/DC-DC-Converter-MP1584EN-Adjustable-Regulator/dp/B0FZK4249L/), AliExpress | **Recommended:** 2A @ 5V (stage 1) + 2A @ 3.3V (stage 2); 95% efficient; compact | 2× 1.50 = 3.00 |

**Alternative (if only LM2596 available):**
| 1 | LM2596 12V→5V buck module | Buck converter | Amazon / AliExpress | 1.5A output; 88% efficient; larger footprint | 1.50 |
| 1 | MP1584 5V→3.3V buck module | Buck converter | Amazon / AliExpress | 2A output | 1.20 |

**Subtotal (MP1584 both stages):** €3.00  
**Subtotal (LM2596 + MP1584):** €2.70

### 5D. Protection & Miscellaneous

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Schottky diode 1N5819 | Diode | Local / Amazon | Polarity protection on 5V rail | 0.20 |
| 1 | Electrolytic capacitor 100 µF 6.3V | Capacitor | Local / Amazon | Local bypass on 5V rail | 0.20 |
| 1 | Electrolytic capacitor 100 µF 6.3V | Capacitor | Local / Amazon | Local bypass on 3.3V rail | 0.20 |
| 1 | TVS diode BZX55C5V1 (5.1V) | TVS | DigiKey / Mouser | Optional; surge protection on 5V rail | 0.30 |

**Subtotal:** €0.90

### Power Supply Total: €6.80 (single supercap, no ideal diode circuit)

---

## 6. Interconnects & Passive Components

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Breadboard or perfboard (for prototyping) | Breadboard | Local / Amazon | 400–830 hole proto board | 2.00 |
| 10 | Dupont jumper wires (male–male) | Wire | Local / Amazon | 10–20 cm assorted | 1.00 |
| 5 | Dupont connectors (male) | Connector | Local / Amazon | For relay modules, buttons, etc. | 0.50 |
| 1 | Shrink tubing assortment | Tubing | Local / Amazon | For wire insulation | 1.00 |
| 1 | Solder + soldering iron | Tool | Local | For board assembly | 5.00 |

**Subtotal:** €9.50

---

## 7. Optional: Scrypted & Home Server Ecosystem

| Qty | Part Number / Description | Category | Supplier | Notes | Est. Price (EUR) |
|-----|---------------------------|----------|----------|-------|------------------|
| 1 | Raspberry Pi 5 (8 GB) | SBC | [Raspberry Pi](https://www.raspberrypi.com/), local | Scrypted host (NVR + HomeKit bridge) | 80.00 |
| 1 | SSD 500GB–1TB NVMe | Storage | Local | For Scrypted video recording | 30.00 |
| 1 | PoE+ injector or 27W power supply | Power | Local | RP5 power; PoE option available | 15.00 |

**Subtotal (if including Scrypted server):** €125.00

---

## 8. Optional: TR-064 / FRITZ!Box Integration

No additional hardware needed; uses existing FRITZ!Box on the home network.

---

## Cost Summary

### Minimum Build (Doorbell + Power Supply)

| Category | Cost (EUR) |
|----------|-----------|
| Core compute | 28.00 |
| Audio hardware | 10.50 |
| Button/LED/enclosure | 10.75 |
| Relays & drivers | 4.70 |
| **Power supply (DB107, MP1584×2, 4F supercap, existing caps)** | **€6.80** |
| Interconnects & tools | 9.50 |
| **Subtotal** | **€70.25** |
| **Contingency (10%)** | **€7.03** |
| **Estimated Total** | **~€77.28** |

### With Scrypted Home Server

| Category | Cost (EUR) |
|----------|-----------|
| Doorbell build (above) | 78.55 |
| Raspberry Pi 5 + SSD + PSU | 125.00 |
| **Estimated Total** | **~€203.55** |

---

## Sourcing Tips

### EU/Germany Retailers
- **DigiKey EU** ([digikey.com](https://www.digikey.com/))
  - LM2596, MP1584, LTC4412, supercaps, bridge rectifiers
  - Fast shipping, reliable stock
  - ~€7–15 minimum order; shipping €3–8

- **Mouser Electronics** ([mouser.com](https://www.mouser.com/))
  - ICs, passives, supercaps
  - Good for bulk orders

- **Seeed Studio** ([seeedstudio.com](https://www.seeedstudio.com/))
  - XIAO ESP32-S3 Sense, MAX98357A breakout
  - Ships from both US and China warehouses

- **Amazon.de** / **AliExpress**
  - Buck modules (LM2596, MP1584), relay modules, buttons, connectors
  - Cheap but slower shipping

### US/International Retailers
- **Adafruit** ([adafruit.com](https://www.adafruit.com/))
  - MAX98357A, XIAO boards, quality components
  - Ships worldwide

- **SparkFun** ([sparkfun.com](https://www.sparkfun.com/))
  - Breakout boards, sensors, good documentation

---

## Assembly Checklist

- [ ] **Breadboard layout:** Sketch the supercapacitor circuit (bridge → bulk cap → buck1 → supercap + ideal diode → buck2 → rail)
- [ ] **Test power supply:** Power 8VAC input; verify ~5V at first buck output and ~3.3V at second stage (unloaded)
- [ ] **Load test:** Apply 200 mA dummy load to 5V rail; monitor voltage droop
- [ ] **Supercap ride-through test:** 
  - Load 3.3V rail with 200 mA
  - Disconnect 5V input momentarily
  - Verify 3.3V stays above 3.0V for ≥3 seconds
- [ ] **Solder supercap balancing resistors** (100–220 Ω across each cap in series)
- [ ] **Mount ESP32-S3 + headers** on proto board
- [ ] **Wire GPIO:**
  - GPIO4 → button → GND
  - GPIO2 → 330 Ω → LED → GND
  - GPIO1 → relay1 (door opener)
  - GPIO3 → relay2 (gong)
  - GPIO7/8/9 → MAX98357A
- [ ] **Wire MAX98357A:** BCLK→7, LRC→8, DIN→9, 3V3, GND
- [ ] **Test firmware build & upload:** `pio run -t upload`
- [ ] **Verify serial output** at 115200 baud; check boot logs
- [ ] **Test doorbell button** & relay activation
- [ ] **Mount in enclosure** with proper strain relief on cables

---

## Notes

1. **Power supply:** Options 1 (supercap, €15) is recommended for short brownout protection. Alternative Li-Po designs (€17) and solar options (€30) described in [POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md).

2. **Supercapacitor lifespan:** 10+ years with proper balancing; no chemistry concerns.

3. **Relay modules:** Both 5V modules work fine with XIAO GPIO (3.3V) input; most modules have Schmitt-trigger logic that accepts 3.3V as high. For guaranteed operation, substitute 3.3V-rated relay modules (harder to find) or use a transistor buffer.

4. **Transformer:** Use existing 8VAC doorbell transformer (typically 16–24 VA). If unavailable, add a ~20 VA 8VAC/12VAC transformer (~€20–30).

5. **Scrypted:** Optional; Scrypted provides HomeKit bridge + NVR video recording. Without it, the ESP32 still works as a standalone camera but HomeKit integration requires additional setup.

6. **Shipping:** Budget 1–4 weeks for international parts; plan ahead for DigiKey/Mouser (5–7 days EU) vs. AliExpress (2–3 weeks).

---

## References

- [POWER_SUPPLY_DESIGNS.md](POWER_SUPPLY_DESIGNS.md) – Full power supply schematic & calculations
- [WIRING_DIAGRAM.md](WIRING_DIAGRAM.md) – Pin map & connections
- [esp32-s3-doorbell-architecture.md](esp32-s3-doorbell-architecture.md) – System overview
- [LM2596 Datasheet](https://www.ti.com/lit/ds/symlink/lm2596.pdf)
- [MP1584 Datasheet](https://www.monolithic-power.com/)
- [LTC4412 Ideal Diode](https://www.analog.com/media/en/technical-documentation/data-sheets/4412fb.pdf)

