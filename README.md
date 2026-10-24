# Arduino Uno Vacuum Pump Controller

## Overview

This project controls a 12 V vacuum pump using an Arduino Uno,
two pressure transmitters, and two Type K thermocouples.

The controller monitors pressure and temperature and switches
the pump through an external N-channel MOSFET.

The pump is commanded OFF if:
- Either pressure sensor exceeds the pressure limit.
- Either temperature sensor exceeds the temperature limit.
- A sensor reading is invalid or outside its configured range.

The controller requires sustained valid readings within limits
before automatically restarting the pump.

## Hardware

- Arduino Uno
- 2 pressure transmitters with 4–20 mA outputs
- 2 Type K thermocouples
- 2 MAX31855 thermocouple interface modules
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
| MOSFET gate control | D7 |

The MAX31855 modules share clock and data-out connections.
Each module has a separate chip-select connection.

## Configuration

The default code settings are:

- Pressure limit: 5.0 psig
- Temperature limit: 50.0 degrees C
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
   sensors.
2. It checks readings for invalid values and implausible ranges.
3. If any limit is exceeded or a fault is detected, the pump is
   commanded OFF and the fault is latched.
4. The pump remains OFF while the fault persists.
5. After the readings are valid, agree sufficiently, and remain
   within limits for the recovery period, the controller permits
   the pump to restart.

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
- Pump ON/OFF state
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
