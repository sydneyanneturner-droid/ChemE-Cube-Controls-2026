# ChemE Cube Controls 2026

Arduino firmware that reads two pressure transmitters and two thermocouples and decides whether the vacuum pump may run. The pump runs only while **all four sensors give valid readings inside their limits**. Any invalid reading or limit violation turns the pump off and latches a fault until the readings have stayed safe for several seconds.

The code is a single sketch: `Vacuum Pump Control Source Code (C++)` (save it as a `.ino` file inside a folder of the same name to open it in the Arduino IDE).

Wiring diagram: `ChemE_Cube_wiring_v6.png` (source: `ChemE_Cube_wiring_v6.svg`).

---

## Hardware

| Part | Used for |
|---|---|
| Arduino Uno R4 Minima | Controller, 5 V logic, powered from the 12 V rail on its own `VIN` pin |
| 2 × Adafruit MAX31855 breakout (product 269) | Thermocouple amplifiers |
| 2 × Type J thermocouple (McMaster 3872K119) | Temperature probes |
| 2 × McMaster 6017N11 pressure transmitter | 4–20 mA, 0–15 psig |
| Brushed 12 V DC vacuum pump | The load being controlled |
| Logic-level N-MOSFET (e.g. IRLZ44N, confirm yours) | Low-side pump switch |
| 1N5822 Schottky diode | Flyback diode across the pump |
| 2 × 100 Ω, 2 × 10 kΩ, 2 × 20 kΩ, 1 × 220 Ω, 1 × 10 kΩ resistors | Shunts, dividers, gate resistor, gate pulldown (1 %, ¼ W) |
| 3 A slow-blow fuse, 12 V DC adapter (McMaster 6933K87) | Power input |

Everything runs from a **single 12 V supply**. There is no boost converter: the transmitters are powered from the 12 V rail.

## Pin assignments

| Pin | Function |
|---|---|
| `A0` | Pressure transmitter 1 (through shunt and divider) |
| `A1` | Pressure transmitter 2 (through shunt and divider) |
| `D13` | Thermocouple amps, clock (shared) |
| `D12` | Thermocouple amps, data out (shared) |
| `D10` | Thermocouple amp 1, chip select |
| `D9` | Thermocouple amp 2, chip select |
| `D6` | MOSFET gate (pump on = HIGH), through a 220 Ω resistor with a 10 kΩ pulldown to ground |
| `VIN` (Arduino pin) | 12 V from the power rail |
| `5V` (Arduino pin) | Powers both thermocouple amps, connected to the `VCC` pin on each amp |

The two amps share clock and data-out but each answers only when its own chip select is low, so the four sensors are read **independently**.

---

## How each sensor is read

### Pressure (4–20 mA transmitters)

Each transmitter is a two-wire current loop: `+12 V → transmitter → shunt node → 100 Ω shunt → GND`. A 10 kΩ / 20 kΩ divider on the shunt node steps the voltage down for the Arduino.

```
current (mA)   = (A0 voltage x 30k/20k) / 99.7 ohm x 1000
pressure (psi) = (current - 4) / 16 x 15
```

The 99.7 Ω is the 100 Ω shunt in parallel with the 30 kΩ divider (`SHUNT_EFFECTIVE_OHMS` in the code).

| Loop current | Pressure | Voltage at A0 |
|---|---|---|
| 4 mA | 0 psig | 0.27 V |
| 9.33 mA | 5 psig (trip) | 0.62 V |
| 20 mA | 15 psig | 1.33 V |

A reading is **invalid** if the current is outside 3.6–21 mA (open wire, shorted wire, dead sensor) or the computed pressure is outside −0.5 to 15.5 psi.

The shunt is only 100 Ω so the transmitter keeps about 10 V at full scale (12 V minus 20 mA × 100 Ω). **The 6017N11 must work down to about 10 V; check its input-voltage spec.**

### Temperature (Type J thermocouples on a MAX31855)

The MAX31855 is built for one thermocouple type, and the Adafruit 269 board uses the **K-type chip**. The probes here are **Type J**, so the code converts the K-chip reading to Type J in software:

1. The chip reports `T_chip = V / 41.276 µV/°C + T_coldjunction`. The code recovers the real thermocouple voltage: `V = (T_chip - T_coldjunction) x 41.276 µV/°C`.
2. It adds the Type J voltage of the cold junction (NIST ITS-90 polynomial) and converts the total back to a temperature with the NIST Type J inverse polynomial.

Set `AMP_IS_K_TYPE = false` if your amps use the MAX31855**J** chip (the chip marking ends in J); the chip reading is then used as-is.

A reading is **invalid** if the library reports a fault (open thermocouple, short to GND, short to VCC) or the temperature is outside −50 °C to 150 °C.

**Accuracy:** the datasheet allows about ±2 °C chip error plus ±2 °C on the internal cold-junction sensor, so expect roughly ±2–4 °C in the worst case.

---

## Control logic

The sketch reads all four sensors every 500 ms, then applies these rules in order:

1. **Any invalid reading** (any of the four): fault latched, pump OFF.
2. **Any limit exceeded**: pressure above **5 psig** on either transmitter, or temperature above **50 °C** on either thermocouple. Fault latched, pump OFF.
3. **Fault latched, readings valid and within limits**: the pump stays OFF until **10 consecutive good readings** and at least **5 seconds** of safe readings have passed. Then the fault clears and the pump turns on. Any bad reading restarts this count.
4. **Otherwise**: pump ON.

At power-up the fault starts **latched**, the pump is held OFF, and it only starts after the recovery period above with all sensors valid.

Limits are strict "greater than" comparisons, so exactly 5.0 psig or 50.0 °C does not trip.

### What happens on a failure

| Failure | Result |
|---|---|
| Thermocouple unplugged or shorted | That reading is invalid, pump OFF |
| Pressure transmitter wire open or transmitter dead | Loop current under 3.6 mA, invalid, pump OFF |
| Pressure transmitter wire shorted | Current over 21 mA, invalid, pump OFF |
| Arduino reset or brownout | Gate pin is pulled low by the 10 kΩ pulldown, pump OFF |

## Configuration (constants near the top of the sketch)

| Constant | Value | Meaning |
|---|---|---|
| `PRESSURE_LIMIT_PSI` | 5.0 | Trip above this pressure |
| `TEMPERATURE_LIMIT_C` | 50.0 | Trip above this temperature |
| `MIN_VALID_TEMP_C` / `MAX_VALID_TEMP_C` | −50 / 150 | Plausible temperature range |
| `MIN_VALID_CURRENT_MA` / `MAX_VALID_CURRENT_MA` | 3.6 / 21 | Plausible loop current |
| `READ_INTERVAL_MS` | 500 | Time between readings |
| `RECOVERY_TIME_MS` | 5000 | Minimum safe time before recovery |
| `REQUIRED_GOOD_READINGS` | 10 | Good readings needed before recovery |
| `SHUNT_RESISTOR_OHMS` | 100 | Shunt value (update if you change the resistor) |
| `R_TOP_OHMS` / `R_BOTTOM_OHMS` | 10 000 / 20 000 | Divider values |
| `AMP_IS_K_TYPE` | true | Convert a K-chip reading to Type J |

## Serial output

At 9600 baud, once per reading:

```
----------------------------
Pressure 1: 1.97
Pressure 2: 2.07
Temperature 1: 29.94
Temperature 2: 30.93
Pump: ON
Fault state: CLEAR
Reason: Recovered: readings valid and within limits
```

A bad sensor prints `INVALID` in place of its value.

---

## Software setup

1. Install the **Arduino UNO R4 Boards** package and select *Arduino UNO R4 Minima*.
2. Install **Adafruit MAX31855 library** from the Library Manager (it also installs Adafruit BusIO).
3. Open the sketch, upload, and open the Serial Monitor at 9600 baud.

## Bring-up checklist

Do these with the pump circuit **unpowered** (or the pump disconnected) first.

1. **Chip type:** confirm the marking on each amp. K means `AMP_IS_K_TYPE = true`.
2. **Thermocouples:** both should read within a couple of degrees of each other and of a room thermometer.
3. **Open thermocouple:** unplug each one in turn. The reading should show `INVALID`.
4. **Pressure circuit:** with a transmitter connected and powered, A0 should read about 0.27 V at 0 psig. With its minus lead unplugged it should read 0 V and show `INVALID`.
5. **Trip points:** raise pressure above 5 psig and warm a probe above 50 °C. Each should turn the pump off.
6. **Pump start:** watch the Serial Monitor at the moment the pump starts. The pump shares the 12 V rail with the sensors, and a voltage dip could make a transmitter read out of range and trip the pump. If this happens, report it before changing the code.

## Known limitations

- **Software protection only.** Do not rely on this sketch as the sole safety device. There is no hardware watchdog or independent cutoff.
- **Disagreement check not implemented.** `PRESSURE_DISAGREEMENT_PSI` and `TEMP_DISAGREEMENT_C` are defined in the code but not used. Duplicate sensors are not compared with each other.
- **No hysteresis band.** After a trip, the pump restarts once readings are back within the limits for the recovery period, so a value hovering at a limit can cause repeated stop and start cycles.
- **Transmitter supply voltage is unverified.** The 12 V loop design depends on the 6017N11 working at about 10 V.
- **Pump current is unverified.** Check the pump's running and startup current against the 3 A fuse.
- **A disconnected thermocouple amp is not reliably detected.** If the data, clock or chip-select wire to an amp comes loose, the floating data line may read as a fault (pump OFF) or as all zeros, which looks like a valid 0 °C. Check each amp's wiring during bring-up. A plausibility check could be added if you want this covered.
- **Temperature accuracy** is limited by the K-chip-to-Type-J conversion (see above).
- **10-bit ADC.** About 0.07 psi per count.
