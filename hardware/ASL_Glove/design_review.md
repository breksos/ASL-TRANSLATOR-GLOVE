# ASL Glove Design Review

**Project:** ASL_Glove (KiCad 9, single sheet, 2-layer PCB)
**Date:** 2026-04-03
**Analyzers:** analyze_schematic.py, analyze_pcb.py (--full), analyze_emc.py
**Datasheets:** MPU-6050.pdf (104KB, InvenSense), ESP32-C3_datasheet.pdf (875KB, Espressif)

---

## Critical Findings

| Severity | Issue | Section |
|----------|-------|---------|
| WARNING | CPOUT (MPU-6050 pin 20) has no decoupling capacitor — IMU may not function | Signal Analysis |
| WARNING | GND copper pour zone is defined but NOT filled — no ground plane on board | PCB Layout |
| WARNING | Power traces (+3V3, GND) use 0.2mm width — marginal for ESP32-C3 peak Wi-Fi current | PCB Layout |
| WARNING | No MPNs on any component (0% MPN coverage) — board cannot be ordered without manual lookup | Component Summary |
| SUGGESTION | All signal nets are unnamed — I2C, UART, and config signals have auto-generated names | Schematic Quality |
| SUGGESTION | Missing bulk cap (10µF) near MPU-6050 VDD — datasheet recommends 100nF + 10µF | Signal Analysis |

---

## Component Summary

| Type | Count | References |
|------|-------|-----------|
| IC / Module | 2 | U1 (ESP32-C3-DevKitM-1), U2 (MPU-6050) |
| Capacitor | 4 | C1–C4 |
| Resistor | 3 | R1–R3 |
| Connector | 1 | J1 (Conn_01x02) |
| Jumper | 1 | JP1 (ROLE_SEL) |
| Test point | 4 | TP1–TP4 (TX, RX, GND, 3V3) |
| **Total** | **15** | |

**Nets:** 36 schematic / 37 PCB | **No-connects:** 18 | **Power rails:** +3V3, GND

**Sourcing audit:** 0/10 components have MPNs. All parts require manual MPN assignment before ordering or JLCPCB assembly.

---

## Power Tree

```
J1 (external 3.3V supply)
  └── +3V3 rail ─── U1 3V3 pin (bidirectional — also sources from USB LDO on DevKitM-1)
                ├── U2 VDD (pin 13)
                ├── U2 VLOGIC (pin 8)
                ├── C1 100nF (bulk decoupling)
                ├── C2 10µF  (bulk decoupling)
                ├── C3 100nF (MPU-6050 VDD decoupling)
                ├── C4 100nF (MPU-6050 VDD decoupling)
                ├── R1 4.7kΩ (I2C SDA pull-up)
                ├── R2 4.7kΩ (I2C SCL pull-up)
                └── R3 10kΩ  (JP1 ROLE_SEL pull-up)
```

**Note:** The ESP32-C3-DevKitM-1 module has its own onboard 3.3V LDO from USB VBUS. When the board is powered via USB, U1's 3V3 pins source +3V3. When powered externally via J1, J1 drives the +3V3 rail. Both paths connect to the same rail — avoid powering both simultaneously unless a diode-OR circuit is used.

---

## Analyzer Verification

### Component Count
Schematic: 15 components (excluding power symbols) — PCB: 15 footprints. **Match ✓**

### Component Pinout Verification

| Ref | Value | Datasheet Verified | Key Connections | Status |
|-----|-------|-------------------|-----------------|--------|
| U1 | ESP32-C3-DevKitM-1 | Unverified (PDF unavailable — web page URL only) | GND(1), 3V3(2,3), IO10(11), IO8/SDA(25), IO9/SCL(26), RX(28), TX(29) | Plausible — standard DevKitM-1 pinout |
| U2 | MPU-6050 | Unverified (pdftoppm unavailable) | VDD(13)=3V3, VLOGIC(8)=3V3, GND(18), AD0(9)=GND, FSYNC(11)=GND, SCL(23), SDA(24) | Plausible — matches standard MPU-6050 symbol |
| R1 | 4.7kΩ | Skipped (2-pin passive) | Pin1=3V3, Pin2=SDA | ✓ |
| R2 | 4.7kΩ | Skipped (2-pin passive) | Pin1=3V3, Pin2=SCL | ✓ |
| R3 | 10kΩ | Skipped (2-pin passive) | Pin1=3V3, Pin2=JP1/IO10 | ✓ |
| C1–C4 | 100nF/10µF | Skipped (2-pin passive) | +3V3/GND | ✓ |
| J1 | Conn_01x02 | Skipped | Pin1=+3V3, Pin2=GND | ✓ |
| JP1 | ROLE_SEL | Skipped | Pin1=IO10 via R3, Pin2=GND | ✓ |

**Verification gap:** pdftoppm is not available in this environment, so datasheet PDFs could not be read directly. The MPU-6050 pinout is based on well-known community knowledge of this part (extremely common, stable for 15+ years). U1 pinout is based on the KiCad symbol for the DevKitM-1 dev board module. Both are assessed as **plausible** — but formal datasheet verification is recommended before ordering.

### Connector Pin Tables

**J1 (Power input):**
| Pin | Net | Function |
|-----|-----|---------|
| 1 | +3V3 | 3.3V supply input |
| 2 | GND | Ground |

**JP1 (ROLE_SEL jumper):**
| Pin | Net | Function |
|-----|-----|---------|
| 1 | IO10 (via R3 10kΩ to +3V3) | Role select signal — HIGH when open, LOW when shorted to GND |
| 2 | GND | Ground |

**Test points:**
| Ref | Net | Label |
|-----|-----|-------|
| TP1 | U1.TX | UART TX |
| TP2 | U1.RX | UART RX |
| TP3 | GND | Ground |
| TP4 | +3V3 | 3.3V supply |

### Net Tracing

**+3V3 power rail:** J1.1 → U1.2/3 → U2.VDD(13) → U2.VLOGIC(8) → C1/C2/C3/C4 → R1.1 → R2.1 → R3.1 → TP4. Continuous, no gaps. ✓

**GND:** J1.2 → U1.GND(1,6,8,12,15,16,19,24,27,30) → U2.GND(18) → U2.AD0(9) → U2.FSYNC(11) → C1/C2/C3/C4 → JP1.2 → TP3. Continuous. ✓

**I2C SDA:** R1.2 → U2.SDA(24) → U1.IO8(25). ✓

**I2C SCL:** R2.2 → U2.SCL(23) → U1.IO9(26). ✓

**UART TX:** U1.TX(29) → TP1. ✓

**UART RX:** U1.RX(28) → TP2. ✓

**ROLE_SEL:** +3V3 → R3(10kΩ) → U1.IO10(11) → JP1.1 (and JP1.2=GND). When JP1 open: IO10 pulled HIGH. When shorted: IO10 pulled LOW. ✓

### PCB Verification

- Footprint count: 15 schematic = 15 PCB ✓
- Board dimensions: 32.0 × 34.0mm
- Routing: complete (0 unrouted nets)
- Vias: **0** (no vias at all)

---

## Signal Analysis Review

### I2C Bus

Detected correctly: ESP32-C3 (master) ↔ MPU-6050 (slave, AD0=GND → address 0x68).

| Parameter | Value | Assessment |
|-----------|-------|-----------|
| SDA pull-up | R1 = 4.7kΩ to +3V3 | ✓ Standard — appropriate for 100/400kHz I2C |
| SCL pull-up | R2 = 4.7kΩ to +3V3 | ✓ Standard |
| Bus capacitance (estimated) | ~10pF | Low (2 devices, short traces) — rise time well within spec |
| I2C address | 0x68 (AD0=GND) | ✓ |

**I2C GPIO assignment:** IO8 (SDA) and IO9 (SCL). On ESP32-C3, I2C can be mapped to any GPIO via the `i2c_param_config()` driver call. Both GPIO8 and GPIO9 are valid for I2C — no issues.

### MPU-6050 Application Circuit

The MPU-6050 requires specific external components beyond VDD/GND decoupling:

| Requirement | Schematic | Status |
|-------------|-----------|--------|
| VDD decoupling 100nF | C3 or C4 (100nF, ~5mm) | ✓ Present |
| VDD bulk cap 10µF | Not present | **WARNING** — datasheet recommends 100nF + 10µF |
| VLOGIC decoupling 100nF | C3/C4 shared rail | Acceptable (VLOGIC=VDD=3.3V here) |
| CPOUT cap 2.2nF to GND | **Not present** | **WARNING — required for internal charge pump** |
| REGOUT cap 100nF | NC (pin 20 floats) | **WARNING** — floated; should have cap to GND |

The CPOUT / REGOUT omission is the highest-risk issue in this design. Without the charge pump capacitor, the MPU-6050's internal oscillator may not start reliably, resulting in no IMU data.

### Decoupling Coverage

| IC | Closest Cap | Distance | Assessment |
|----|-------------|----------|-----------|
| U1 (ESP32-C3-DevKitM-1) | C1 (100nF) + C2 (10µF) | 9.06mm | Module has onboard decoupling — external caps serve as rail bulk supply. Acceptable for a dev board module. |
| U2 (MPU-6050) | C4 (100nF) | 4.94mm | Borderline — under 5mm is acceptable but closer is better. Missing 10µF bulk. |

### Protection Devices

No ESD or TVS protection on any connector. J1 (power input) and JP1 (external header) are exposed interfaces with no protection. For a glove device that will be plugged/unplugged in the field, TVS protection on J1 is recommended.

---

## PCB Layout Review

### Ground Plane — CRITICAL GAP

The GND zone is **defined** in the PCB but **not filled** (`is_filled: false`). There is no copper pour on either layer for GND. This has significant consequences:

1. **EMC**: Return currents take longer paths through traces instead of a solid plane — increases radiated emissions
2. **Signal integrity**: I2C and UART signals lack controlled return paths
3. **Noise**: MPU-6050 is sensitive to power supply noise; a ground plane significantly reduces it

**Fix:** In KiCad, press `B` (Edit → Fill All Zones) before generating Gerbers. The zone outline is already drawn.

### Power Trace Width

| Net | Current trace width | Est. peak current | IPC-2221 minimum (1oz, external) | Status |
|-----|--------------------|--------------------|----------------------------------|--------|
| +3V3 | 0.2mm | ~150mA (ESP32 Wi-Fi peak) | ~0.3mm for 150mA, 10°C rise | Marginal |
| GND | 0.2mm | ~150mA | ~0.3mm | Marginal |

The ESP32-C3 module draws up to ~150mA peak during Wi-Fi TX bursts. The current 0.2mm traces are at the minimum DRC rule limit and within acceptable range for average current (the module's onboard LDO handles instantaneous peaks), but widening to 0.5mm would reduce voltage droop and improve reliability.

### Via Stitching

**0 vias on the entire board.** Since all components are on F.Cu and there's no B.Cu routing, vias are not needed for signal routing. However, ground stitching vias connecting the GND zone (once filled) to a B.Cu GND pour would significantly improve EMC. See EMC findings.

### Placement

| Issue | Detail |
|-------|--------|
| J1 edge clearance | 0.7mm — passes DRC (0.5mm minimum), but tight for a connector |
| Component density | 1.4 components/cm² — moderate density, no courtyard overlaps |
| All components front side | ✓ — simplifies assembly |

### DFM (JLCPCB Standard Tier)

| Metric | Value | Limit | Status |
|--------|-------|-------|--------|
| Min track width | 0.2mm | 0.127mm | ✓ |
| Min spacing | 0.3mm | 0.127mm | ✓ |
| Board size | 32×34mm | Up to 500×500mm | ✓ |
| DRC violations | 0 | 0 | ✓ |

---

## EMC Summary (from analyze_emc.py)

**Score: 87/100** — see `emc_analysis.json` for full findings. Key items:

| Severity | Rule | Finding |
|----------|------|---------|
| HIGH | DC-001 | Decoupling caps 9.1mm from U1 — acceptable for module, monitor if noise issues |
| MEDIUM | VS-001 | No ground stitching vias |
| LOW | IO-001 | No filtering on J1 or JP1 connectors |
| INFO | EE-001 | Board resonance at 2102/2233 MHz — near 2.4GHz Wi-Fi band |

---

## Issues Found — Prioritized

| Priority | Severity | Issue | Fix |
|----------|----------|-------|-----|
| 1 | WARNING | MPU-6050 CPOUT/REGOUT has no capacitor — IMU may not work | Add 2.2nF to CPOUT (pin 20) and 100nF to REGOUT (pin 10), both to GND |
| 2 | WARNING | GND copper pour not filled | Press `B` in KiCad PCB editor before exporting Gerbers |
| 3 | WARNING | Power traces only 0.2mm | Widen +3V3 and GND traces to 0.5mm |
| 4 | WARNING | 0% MPN coverage | Add MPN properties to all components; especially U1, U2 |
| 5 | SUGGESTION | Missing bulk cap near MPU-6050 | Add 10µF capacitor near U2 VDD |
| 6 | SUGGESTION | All signal nets unnamed | Add net labels: SDA, SCL, TX, RX, ROLE_SEL |
| 7 | SUGGESTION | No ESD protection on J1 | Add TVS (e.g., PRTR5V0U2X) on J1 power pins |
| 8 | SUGGESTION | GND PWR_FLAG missing | Add PWR_FLAG to GND net to suppress ERC warning |
| 9 | SUGGESTION | No ground stitching vias | Add stitching vias around board edges after filling GND zone |

---

## Positive Findings

- Routing is complete — no unrouted nets
- I2C pull-up values (4.7kΩ) are correct for 3.3V bus at 100/400kHz
- MPU-6050 I2C address (AD0=GND → 0x68) is explicitly set
- FSYNC tied to GND — correct for standalone IMU operation
- Test points provided for UART TX/RX, GND, and 3V3 — good for debug
- DFM passes JLCPCB standard tier — manufacturable as-is
- ROLE_SEL jumper has proper pull-up — clean logic level configuration

---

## Generated Files

| File | Purpose | Regenerable |
|------|---------|------------|
| `schematic_analysis.json` | Schematic analyzer output | Yes |
| `pcb_analysis.json` | PCB analyzer output (--full) | Yes |
| `emc_analysis.json` | EMC pre-compliance analysis | Yes |
| `datasheets/MPU-6050.pdf` | InvenSense MPU-6050 datasheet | Yes |
| `datasheets/ESP32-C3_datasheet.pdf` | Espressif ESP32-C3 datasheet | Yes |
| `datasheets/index.json` | Datasheet manifest | Yes |
| `design_review.md` | This report | Yes |
