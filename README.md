# Arduino Uno Vacuum Pump Controller

## Overview

This project controls a 12 V vacuum pump using an Arduino Uno,
two pressure transmitters, and two Type J thermocouples.

The controller drives the pump with PWM through an external
N-channel MOSFET. It runs the pump at the duty cycle calibrated
for 10 L/min. When any pressure or temperature reading rises
above its throttle threshold, the pump slows down step by step.
Once readings fall back below the threshold (minus a hysteresis
band), it ramps back up to the 10 L/min duty.

The pump is commanded OFF (hard trip) if:
- Either pressure sensor exceeds the pressure trip limit.
- Either temperature sensor exceeds the temperature trip limit.
- A sensor reading is invalid or outside its configured range.

The controller requires sustained valid readings within limits
before automatically restarting the pump. Restarts begin at the
minimum duty (soft start) and ramp up.

## Sketch Location

Open `VacuumPumpControl/VacuumPumpControl.ino` in the Arduino IDE.
Install the "Adafruit MAX31855 library" (and its dependency
Adafruit BusIO) from the Library Manager. Board: Arduino Uno.

## Hardware

- Arduino Uno
- 2 pressure transmitters with 4–20 mA outputs
- 2 Type J thermocouples
- 2 MAX31855 (K version) thermocouple interface modules

## Type J Probes on MAX31855K Boards

The standard MAX31855 assumes a Type K thermocouple. It does no
linearization; it reports cold-junction temperature plus the
thermocouple voltage divided by 41.276 uV/C. The sketch reads both
the reported value and the board's cold-junction temperature,
recovers the raw thermocouple voltage, and converts it with the
NIST ITS-90 Type J polynomials (`readTemperature()`).

Without this correction a Type J probe reads high: about 7 C high
at 50 C and about 40 C high at 150 C.

Limits of the workaround:
- Chip resolution is 0.25 C in K terms (about 0.2 C in J terms).
- The chip's own accuracy spec (about +/-2 C) still applies.
- Keep the board's thermocouple terminals at the same temperature
  as the chip (out of drafts and away from heat sources), because
  the cold-junction reading is the chip's internal temperature.
- Check both channels against a reference thermometer (ice
  bath and warm water) before relying on the readings.
- 2 separate 250-ohm shunt resistors
- 2 voltage-divider circuits per the wiring design
- Logic-level N-channel MOSFET suitable for the pump current
- 12 V vacuum pump and suitable power supply
- Flyback diode rated for the pump's current
- Suitable wiring, connectors, and protective components

## Pin Assignments

| Component | Arduino Pin |
|-----------|-------------|
| Pressure sensor 1 | A0 |
| Pressure sensor 2 | A1 |
| MAX31855 clock | D13 |
| MAX31855 data out | D12 |
| Thermocouple 1 chip select | D10 |
| Thermocouple 2 chip select | D9 |
| MOSFET gate control (PWM) | D6 |

The MAX31855 modules share clock and data-out connections.
Each module has a separate chip-select connection.

## Configuration

The default code settings are:

- Target flow: 10 L/min at `TARGET_DUTY_PCT` (default 80%, MUST be
  calibrated with a flowmeter; there is no flow sensor, so flow
  control is open-loop)
- Minimum duty: 35% (`MIN_DUTY_PCT`, calibrate so the pump never
  stalls)
- Ramp: down 5% / up 2% per reading
- Throttle thresholds: 4.0 psig, 45.0 degrees C
- Hysteresis: 0.5 psi, 2.0 degrees C
- Hard trip limits (pump OFF): 5.0 psig, 50.0 degrees C
- Sensor read interval: 500 milliseconds
- Recovery period: 5 seconds
- Required consecutive good readings: 10

The default pressure calibration is 0 to +15 psig.

IMPORTANT: A 0–15 psig transmitter cannot necessarily measure
negative gauge pressure. Use a transmitter rated for the vacuum
range required by the system, and configure the pressure
calibration to match its datasheet and calibration results.

Do not change the software range to a negative value unless
the actual sensor supports that range.

## How the Controller Works

1. The Arduino reads both pressure sensors and both temperature
   sensors every 500 ms.
2. It checks readings for invalid values and implausible ranges.
3. If a trip limit is exceeded or a fault is detected, the pump is
   commanded OFF and the fault is latched.
4. The pump remains OFF while the fault persists.
5. After the readings are valid and remain within limits for the
   recovery period, the pump restarts at minimum duty.
6. In normal operation the highest pressure and highest
   temperature are compared with the throttle thresholds:
   - Above threshold: duty drops 5% per reading, down to the
     minimum duty.
   - Below threshold minus hysteresis: duty rises 2% per reading,
     up to the 10 L/min target duty.
   - Inside the hysteresis band: duty holds.

Negative gauge pressure is acceptable when the pressure sensor
is designed and calibrated to measure vacuum.

## Sensor Fault Detection

The code checks for:
- Invalid temperature readings, including NaN.
- Temperatures outside the configured valid range.
- Pressure transmitter current outside configured bounds.
- Pressure readings outside the configured calibrated range.
- Pressure or temperature limits being exceeded.

These checks cannot detect every possible sensor failure.
A sensor can provide a plausible but incorrect value.

## Serial Monitor

Open the Arduino IDE Serial Monitor at 9600 baud.

The program prints:
- Pressure sensor 1 reading and status
- Pressure sensor 2 reading and status
- Temperature sensor 1 reading and status
- Temperature sensor 2 reading and status
- Pump duty (%) and target
- Control mode (soft start, ramping up, throttling, holding, off)
- Fault latch status
- Current fault reason

Use this output during commissioning and troubleshooting.

## Calibration

Before relying on the controller:

1. Verify the actual transmitter pressure range and output.
2. Verify the shunt resistor and voltage-divider values.
3. Confirm that the Arduino analog inputs cannot exceed safe
   voltage limits.
4. Compare each pressure reading with a known reference.
5. Compare both temperature readings with a suitable reference.
6. Test disconnected and faulty sensors without running the pump.
7. Confirm the pump switches OFF for every simulated fault.
8. Verify the recovery behavior before normal operation.

To set the 10 L/min duty: run the pump with a flowmeter in the
line at normal operating conditions, adjust `TARGET_DUTY_PCT`
until it reads 10 L/min, and re-upload. Find the lowest duty at
which the pump still starts and turns reliably, and set
`MIN_DUTY_PCT` a little above it.

The Arduino Uno's ADC reference and resistor tolerances can
introduce measurement errors. Calibrate each channel.

## Safety

This software is not a certified safety controller.

Use independent pressure protection appropriate to the system,
including a properly rated pressure-relief or vacuum-protection
arrangement where required.

Do not rely exclusively on software to prevent excessive pressure
or temperature. A failed MOSFET may leave the pump energized.

Ensure the MOSFET is rated for the pump's startup current, use
appropriate electrical protection, and connect grounds correctly
where required by the chosen circuit.

Test the controller without a live pressurized system before
commissioning it.
