#include <SPI.h>
#include <Adafruit_MAX31855.h>
#include <math.h>

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
// Must be a PWM pin. D6 runs at ~980 Hz on the Uno (Timer0).
const int PUMP_PIN = 6;

Adafruit_MAX31855 thermocouple1(
  THERMO_CLK, THERMO_CS_1, THERMO_DO
);

Adafruit_MAX31855 thermocouple2(
  THERMO_CLK, THERMO_CS_2, THERMO_DO
);

// Arduino Uno ADC
const float ADC_REFERENCE_V = 5.00;

// Each transmitter uses a 250-ohm shunt resistor
// and a 10k/20k voltage divider.
const float SHUNT_RESISTOR_OHMS = 250.0;
const float R_TOP_OHMS = 10000.0;
const float R_BOTTOM_OHMS = 20000.0;

const float PRESSURE_MIN_PSI = 0.0;
const float PRESSURE_MAX_PSI = 15.0;

// =====================================================
// FLOW / SPEED SETTINGS
// =====================================================
// There is no flow sensor, so flow is set open-loop by PWM duty.
// TARGET_DUTY_PCT MUST be calibrated: run the pump at several duty
// values with a flowmeter/rotameter on the line and pick the duty
// that gives 10 L/min at normal operating conditions.
const float TARGET_FLOW_LPM = 10.0;   // for display only
const int TARGET_DUTY_PCT = 80;       // <-- CALIBRATE: duty for 10 L/min

// Lowest duty the pump still turns reliably at. Below this a brushed
// motor can stall and overheat. CALIBRATE on the real pump.
const int MIN_DUTY_PCT = 35;

// How fast the speed changes, per reading (every READ_INTERVAL_MS).
// Slow down quickly, speed back up gently.
const int RAMP_DOWN_STEP_PCT = 5;
const int RAMP_UP_STEP_PCT = 2;

// =====================================================
// OPERATING LIMITS
// =====================================================

// Throttle thresholds: above these, the pump slows down.
const float THROTTLE_PRESSURE_PSI = 4.0;
const float THROTTLE_TEMP_C = 45.0;

// Hysteresis: the pump only speeds back up once readings drop
// this far below the throttle threshold. Prevents hunting.
const float PRESSURE_HYSTERESIS_PSI = 0.5;
const float TEMP_HYSTERESIS_C = 2.0;

// Hard trip limits: above these, the pump shuts OFF and latches.
const float PRESSURE_TRIP_PSI = 5.0;
const float TEMPERATURE_TRIP_C = 50.0;

// Plausibility limits for the sensors.
// Adjust temperature range to the actual probe/system.
const float MIN_VALID_TEMP_C = -50.0;
const float MAX_VALID_TEMP_C = 150.0;

const float MIN_VALID_CURRENT_MA = 3.6;
const float MAX_VALID_CURRENT_MA = 21.0;

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

int dutyPct = 0;            // current pump duty, 0 = OFF
bool faultLatched = true;   // Start with pump OFF
int consecutiveGoodReadings = 0;

const char *faultReason = "Startup: waiting for valid readings";
const char *controlMode = "OFF";

// =====================================================
// PRESSURE READING
// =====================================================

float readPressure(int pin, bool &valid)
{
  int adcValue = analogRead(pin);

  float adcVoltage =
      (adcValue / 1023.0) * ADC_REFERENCE_V;

  // Undo the external voltage divider
  float transmitterVoltage =
      adcVoltage *
      ((R_TOP_OHMS + R_BOTTOM_OHMS) / R_BOTTOM_OHMS);

  // Convert voltage across the shunt to current
  float current_mA =
      (transmitterVoltage / SHUNT_RESISTOR_OHMS) * 1000.0;

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

// The probes are Type J, but the MAX31855 (K version) assumes Type K.
// The chip does no linearization: it reports
//   T_reported = T_coldJunction + V_thermocouple / 41.276 uV/C
// so we can recover the raw thermocouple voltage and convert it with
// the NIST ITS-90 Type J polynomials instead.
const float MAX31855K_UV_PER_C = 41.276;

// NIST ITS-90 Type J: temperature (C) -> EMF (mV), valid -210..760 C
float typeJ_mV(float t)
{
  const float c[] = {
    0.0, 5.0381187815E-02, 3.0475836930E-05, -8.5681065720E-08,
    1.3228195295E-10, -1.7052958337E-13, 2.0948090697E-16,
    -1.2538395336E-19, 1.5631725697E-23
  };
  float e = 0.0;
  for (int i = 8; i >= 0; i--) e = e * t + c[i];
  return e;
}

// NIST ITS-90 Type J inverse: EMF (mV) -> temperature (C)
float typeJ_C(float mV)
{
  // -210..0 C (-8.095..0 mV)
  const float neg[] = {
    0.0, 1.9528268E+01, -1.2286185E+00, -1.0752178E+00,
    -5.9086933E-01, -1.7256713E-01, -2.8131513E-02,
    -2.3963370E-03, -8.3823321E-05
  };
  // 0..760 C (0..42.919 mV)
  const float pos[] = {
    0.0, 1.978425E+01, -2.001204E-01, 1.036969E-02,
    -2.549687E-04, 3.585153E-06, -5.344285E-08, 5.099890E-10
  };
  float t = 0.0;
  if (mV < 0.0)
    for (int i = 8; i >= 0; i--) t = t * mV + neg[i];
  else
    for (int i = 7; i >= 0; i--) t = t * mV + pos[i];
  return t;
}

float readTemperature(
  Adafruit_MAX31855 &sensor,
  bool &valid)
{
  // readCelsius() returns NAN on open/short faults
  float reportedK = sensor.readCelsius();
  float coldJunction = sensor.readInternal();

  // Back out the measured thermocouple voltage (mV)
  float tcVoltage_mV =
      (reportedK - coldJunction) * MAX31855K_UV_PER_C / 1000.0;

  // Add cold-junction compensation using the Type J curve
  float temperature = typeJ_C(tcVoltage_mV + typeJ_mV(coldJunction));

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

void setPumpDuty(int pct)
{
  if (pct <= 0)
  {
    dutyPct = 0;
    analogWrite(PUMP_PIN, 0);   // analogWrite(0) drives the pin fully LOW
    return;
  }

  dutyPct = constrain(pct, MIN_DUTY_PCT, 100);
  analogWrite(PUMP_PIN, map(dutyPct, 0, 100, 0, 255));
}

void pumpOff()
{
  setPumpDuty(0);
}

void tripPump(const char *reason)
{
  faultLatched = true;
  consecutiveGoodReadings = 0;
  safeSince = 0;
  faultReason = reason;
  controlMode = "OFF (TRIPPED)";
  pumpOff();
}

// =====================================================
// MAIN CONTROL LOGIC
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
    tripPump("Invalid or out-of-range sensor reading");
    return;
  }

  // The worst (highest) reading of each pair drives control.
  float pMax = max(p1, p2);
  float tMax = max(t1, t2);

  // Hard limit: shut the pump OFF and latch.
  if (pMax > PRESSURE_TRIP_PSI || tMax > TEMPERATURE_TRIP_C)
  {
    tripPump("Pressure or temperature trip limit exceeded");
    return;
  }

  // Once tripped, require sustained safe readings before restart.
  if (faultLatched)
  {
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
      controlMode = "SOFT START";
      setPumpDuty(MIN_DUTY_PCT);   // soft start, ramp up from here
    }
    else
    {
      controlMode = "OFF (RECOVERING)";
      pumpOff();
    }

    return;
  }

  // Normal operation: throttle on high readings, otherwise
  // ramp back toward the 10 L/min target duty.
  bool overThrottle =
      pMax > THROTTLE_PRESSURE_PSI ||
      tMax > THROTTLE_TEMP_C;

  bool clearOfThrottle =
      pMax < THROTTLE_PRESSURE_PSI - PRESSURE_HYSTERESIS_PSI &&
      tMax < THROTTLE_TEMP_C - TEMP_HYSTERESIS_C;

  if (overThrottle)
  {
    controlMode = (dutyPct <= MIN_DUTY_PCT)
                    ? "THROTTLED (AT MIN)"
                    : "THROTTLING DOWN";
    setPumpDuty(dutyPct - RAMP_DOWN_STEP_PCT);
  }
  else if (clearOfThrottle)
  {
    controlMode = (dutyPct >= TARGET_DUTY_PCT)
                    ? "NORMAL (TARGET FLOW)"
                    : "RAMPING UP";
    setPumpDuty(min(dutyPct + RAMP_UP_STEP_PCT, TARGET_DUTY_PCT));
  }
  else
  {
    // Inside the hysteresis band: hold current speed.
    controlMode = "HOLDING";
    setPumpDuty(dutyPct);
  }
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  pinMode(PUMP_PIN, OUTPUT);
  pumpOff();

  Serial.begin(9600);

  thermocouple1.begin();
  thermocouple2.begin();
  delay(500);   // MAX31855 needs time after power-up before first read

  Serial.println("Vacuum pump controller starting.");
  Serial.println("Pump held OFF until sensors are validated.");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
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
  Serial.println(F("----------------------------"));

  Serial.print(F("Pressure 1 (psig): "));
  if (p1Valid) Serial.println(p1, 2); else Serial.println(F("INVALID"));

  Serial.print(F("Pressure 2 (psig): "));
  if (p2Valid) Serial.println(p2, 2); else Serial.println(F("INVALID"));

  Serial.print(F("Temperature 1 (C): "));
  if (t1Valid) Serial.println(t1, 2); else Serial.println(F("INVALID"));

  Serial.print(F("Temperature 2 (C): "));
  if (t2Valid) Serial.println(t2, 2); else Serial.println(F("INVALID"));

  Serial.print(F("Pump duty: "));
  Serial.print(dutyPct);
  Serial.print(F("% (target "));
  Serial.print(TARGET_DUTY_PCT);
  Serial.print(F("% = nominal "));
  Serial.print(TARGET_FLOW_LPM, 1);
  Serial.println(F(" L/min)"));

  Serial.print(F("Mode: "));
  Serial.println(controlMode);

  Serial.print(F("Fault state: "));
  Serial.println(faultLatched ? F("LATCHED") : F("CLEAR"));

  Serial.print(F("Reason: "));
  Serial.println(faultReason);
}
