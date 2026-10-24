#include <SPI.h>
#include <Adafruit_MAX31855.h>
#include <math.h>
#include <avr/wdt.h>   // CHANGED: watchdog timer (resets the Arduino if the loop hangs)

// =====================================================
// PIN ASSIGNMENTS
// =====================================================

// Pressure transmitters: each has its own measurement circuit
const int PRESSURE_PIN_1 = A0;
const int PRESSURE_PIN_2 = A1;

// MAX31855 modules share clock and data-out
const int THERMO_CLK = 13;
const int THERMO_DO  = 12;
const int THERMO_CS_1 = 10;
const int THERMO_CS_2 = 9;

// MOSFET gate controlling the vacuum pump.
// HARDWARE: add a 10k resistor from this gate to GND so the pump
// stays OFF while the Arduino is resetting or if it ever fails.
const int PUMP_PIN = 7;

Adafruit_MAX31855 thermocouple1(
  THERMO_CLK, THERMO_CS_1, THERMO_DO
);

Adafruit_MAX31855 thermocouple2(
  THERMO_CLK, THERMO_CS_2, THERMO_DO
);

// Arduino Uno ADC
// Measure the Arduino's 5V pin with a multimeter and enter the real value
// here for better accuracy (USB-powered boards are often 4.7-5.1 V).
const float ADC_REFERENCE_V = 5.00;

// Each transmitter uses a 250-ohm shunt resistor
// and a 10k/20k voltage divider.
// The loop supply voltage (12 V, 24 V, ...) does NOT appear in the code.
const float SHUNT_RESISTOR_OHMS = 250.0;
const float R_TOP_OHMS = 10000.0;
const float R_BOTTOM_OHMS = 20000.0;

const float PRESSURE_MIN_PSI = 0.0;
const float PRESSURE_MAX_PSI = 15.0;

// Operating limits
const float PRESSURE_LIMIT_PSI = 5.0;
const float TEMPERATURE_LIMIT_C = 50.0;

// CHANGED: hysteresis so the pump does not chatter on and off
// right at the limit. Readings must fall below (limit - margin)
// before the recovery timer starts counting.
const float PRESSURE_RECOVERY_MARGIN_PSI = 0.5;
const float TEMP_RECOVERY_MARGIN_C = 2.0;

// Plausibility limits for the sensors.
// Adjust temperature range to the actual probe/system.
const float MIN_VALID_TEMP_C = -50.0;
const float MAX_VALID_TEMP_C = 150.0;

const float MIN_VALID_CURRENT_MA = 3.6;
const float MAX_VALID_CURRENT_MA = 21.0;

// CHANGED: number of ADC samples averaged per reading
const int ADC_SAMPLES = 8;

// Timing
const unsigned long READ_INTERVAL_MS = 500;
const unsigned long RECOVERY_TIME_MS = 5000;

// Require several consecutive good readings before recovery
const int REQUIRED_GOOD_READINGS = 10;

// =====================================================
// STATE
// =====================================================

unsigned long lastReadTime = 0;
unsigned long safeSince = 0;

bool pumpOn = false;
bool faultLatched = true;  // Start with pump OFF
int consecutiveGoodReadings = 0;

// CHANGED: const char* instead of String (avoids heap fragmentation on the Uno)
const char *faultReason = "Startup: waiting for valid readings";

// =====================================================
// PRESSURE READING
// =====================================================

float readPressure(int pin, bool &valid)
{
  // CHANGED: throw away one reading so the ADC input settles after
  // switching between A0 and A1, then average several samples.
  analogRead(pin);

  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++)
  {
    sum += analogRead(pin);
  }
  float adcValue = sum / (float)ADC_SAMPLES;

  float adcVoltage =
      (adcValue / 1023.0) * ADC_REFERENCE_V;

  // Undo the external voltage divider
  float transmitterVoltage =
      adcVoltage *
      ((R_TOP_OHMS + R_BOTTOM_OHMS) / R_BOTTOM_OHMS);

  // CHANGED: the 30k divider sits in parallel with the shunt and carries
  // a little loop current, so include it in the current calculation.
  float current_mA =
      transmitterVoltage *
      (1.0 / SHUNT_RESISTOR_OHMS +
       1.0 / (R_TOP_OHMS + R_BOTTOM_OHMS)) * 1000.0;

  // Detect open-circuit, short-circuit, or out-of-range current
  if (!isfinite(current_mA) ||
      current_mA < MIN_VALID_CURRENT_MA ||
      current_mA > MAX_VALID_CURRENT_MA)
  {
    valid = false;
    return NAN;
  }

  // Convert 4-20 mA linearly to the configured pressure range
  float pressure =
      PRESSURE_MIN_PSI +
      ((current_mA - 4.0) / 16.0) *
      (PRESSURE_MAX_PSI - PRESSURE_MIN_PSI);

  if (!isfinite(pressure) ||
      pressure < PRESSURE_MIN_PSI - 0.5 ||
      pressure > PRESSURE_MAX_PSI + 0.5)
  {
    valid = false;
    return NAN;
  }

  valid = true;
  return pressure;
}

// =====================================================
// TEMPERATURE READING
// =====================================================

float readTemperature(
  Adafruit_MAX31855 &sensor,
  bool &valid)
{
  float temperature = sensor.readCelsius();

  if (!isfinite(temperature) ||
      temperature < MIN_VALID_TEMP_C ||
      temperature > MAX_VALID_TEMP_C)
  {
    valid = false;
    return NAN;
  }

  valid = true;
  return temperature;
}

// =====================================================
// PUMP CONTROL
// =====================================================

void pumpOff()
{
  digitalWrite(PUMP_PIN, LOW);
  pumpOn = false;
}

void pumpOnNow()
{
  digitalWrite(PUMP_PIN, HIGH);
  pumpOn = true;
}

// CHANGED: one place to trip the system
void tripFault(const char *reason)
{
  faultLatched = true;
  consecutiveGoodReadings = 0;
  safeSince = 0;
  faultReason = reason;
  pumpOff();
}

// =====================================================
// MAIN SAFETY LOGIC
// =====================================================

void updateControl(
  float p1, float p2,
  float t1, float t2,
  bool p1Valid, bool p2Valid,
  bool t1Valid, bool t2Valid)
{
  // Any invalid sensor reading means pump OFF
  if (!p1Valid || !p2Valid || !t1Valid || !t2Valid)
  {
    tripFault("Invalid or out-of-range sensor reading");
    return;
  }

  // Any sensor exceeding its limit trips the system.
  // Negative pressure is allowed IF the transmitter
  // is rated and configured for a negative pressure range.
  if (p1 > PRESSURE_LIMIT_PSI ||
      p2 > PRESSURE_LIMIT_PSI ||
      t1 > TEMPERATURE_LIMIT_C ||
      t2 > TEMPERATURE_LIMIT_C)
  {
    tripFault("Pressure or temperature limit exceeded");
    return;
  }

  // Sensors 1 and 2 are on two different reactors, so their readings
  // are NOT compared against each other. Each channel is checked only
  // against its own limits.

  // Once tripped, require sustained safe readings
  if (faultLatched)
  {
    // CHANGED: hysteresis. Readings must be clearly below the limits,
    // not just barely under them, before recovery counting starts.
    bool belowRecoveryLimits =
        p1 <= PRESSURE_LIMIT_PSI - PRESSURE_RECOVERY_MARGIN_PSI &&
        p2 <= PRESSURE_LIMIT_PSI - PRESSURE_RECOVERY_MARGIN_PSI &&
        t1 <= TEMPERATURE_LIMIT_C - TEMP_RECOVERY_MARGIN_C &&
        t2 <= TEMPERATURE_LIMIT_C - TEMP_RECOVERY_MARGIN_C;

    if (!belowRecoveryLimits)
    {
      consecutiveGoodReadings = 0;
      safeSince = 0;
      faultReason = "Waiting: readings must fall below recovery thresholds";
      pumpOff();
      return;
    }

    if (consecutiveGoodReadings == 0)
    {
      safeSince = millis();
    }

    consecutiveGoodReadings++;

    if (consecutiveGoodReadings >= REQUIRED_GOOD_READINGS &&
        millis() - safeSince >= RECOVERY_TIME_MS)
    {
      faultLatched = false;
      faultReason = "Recovered: readings valid and within limits";
      consecutiveGoodReadings = 0;
      pumpOnNow();
    }
    else
    {
      faultReason = "Counting good readings before restart";
      pumpOff();
    }

    return;
  }

  // Normal operation
  pumpOnNow();
}

// =====================================================
// SERIAL OUTPUT
// =====================================================

// CHANGED: prints without building String objects
void printReading(const char *label, float value, bool valid)
{
  Serial.print(label);
  if (valid)
  {
    Serial.println(value, 2);
  }
  else
  {
    Serial.println("INVALID");
  }
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  // CHANGED: write LOW before making the pin an output so it never
  // glitches HIGH at startup.
  digitalWrite(PUMP_PIN, LOW);
  pinMode(PUMP_PIN, OUTPUT);
  pumpOff();

  Serial.begin(9600);

  Serial.println("Vacuum pump controller starting.");
  Serial.println("Pump held OFF until sensors are validated.");

  // CHANGED: if the loop ever stops running for 2 seconds, the Arduino
  // resets and the gate pull-down turns the pump OFF.
  wdt_enable(WDTO_2S);
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  wdt_reset();  // CHANGED: tell the watchdog the loop is still running

  if (millis() - lastReadTime < READ_INTERVAL_MS)
  {
    return;
  }

  lastReadTime = millis();

  bool p1Valid = false;
  bool p2Valid = false;
  bool t1Valid = false;
  bool t2Valid = false;

  float p1 = readPressure(PRESSURE_PIN_1, p1Valid);
  float p2 = readPressure(PRESSURE_PIN_2, p2Valid);

  float t1 = readTemperature(thermocouple1, t1Valid);
  float t2 = readTemperature(thermocouple2, t2Valid);

  updateControl(
    p1, p2, t1, t2,
    p1Valid, p2Valid, t1Valid, t2Valid
  );

  // Diagnostic output
  Serial.println("----------------------------");

  printReading("Pressure 1: ", p1, p1Valid);
  printReading("Pressure 2: ", p2, p2Valid);
  printReading("Temperature 1: ", t1, t1Valid);
  printReading("Temperature 2: ", t2, t2Valid);

  Serial.print("Pump: ");
  Serial.println(pumpOn ? "ON" : "OFF");

  Serial.print("Fault state: ");
  Serial.println(faultLatched ? "LATCHED" : "CLEAR");

  Serial.print("Reason: ");
  Serial.println(faultReason);
}
