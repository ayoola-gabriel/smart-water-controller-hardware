#include <Arduino.h>
#include <math.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "Sensors.h"

static bool stableRead(uint8_t pin, uint8_t activeState) {
    uint8_t activeCount = 0;
    for (uint8_t i = 0; i < 5; ++i) {
        if (digitalRead(pin) == activeState) activeCount++;
        delayMicroseconds(300);
    }
    return activeCount >= 4;
}

OperatingMode detectOperatingMode() {
    pinMode(PIN_MODE_SELECT, INPUT);
    delay(MODE_SETTLE_MS);
    return digitalRead(PIN_MODE_SELECT) == HIGH ? PROTOTYPE_MODE : REAL_MODE;
}

void beginLevelSensors(OperatingMode mode) {
    pinMode(PIN_SENSOR_COMMON, OUTPUT);
    if (mode == PROTOTYPE_MODE) {
        digitalWrite(PIN_SENSOR_COMMON, LOW);
        // GPIO34/35/36 are input-only and have no internal pull resistors.
        // Fit an external 10k pulldown from each level input to GND.
        pinMode(PIN_LEVEL_LOW, INPUT);
        pinMode(PIN_LEVEL_MID, INPUT);
        pinMode(PIN_LEVEL_FULL, INPUT);
    } else {
        // Float contacts should connect the steady 3V3 common to each input.
        // GPIO34/35/36 have no internal pulls: use external pulldowns.
        digitalWrite(PIN_SENSOR_COMMON, HIGH);
        pinMode(PIN_LEVEL_LOW, INPUT);
        pinMode(PIN_LEVEL_MID, INPUT);
        pinMode(PIN_LEVEL_FULL, INPUT);
    }
}

void updateLevelSensors(OperatingMode mode, WaterData &data) {
    if (mode == PROTOTYPE_MODE) {
        // Briefly energize the common electrode; sample, then remove excitation.
        digitalWrite(PIN_SENSOR_COMMON, HIGH);
        delayMicroseconds(ELECTRODE_PULSE_US);
        data.lowLevel = stableRead(PIN_LEVEL_LOW, ELECTRODE_WET_STATE);
        data.midLevel = stableRead(PIN_LEVEL_MID, ELECTRODE_WET_STATE);
        data.highLevel = false;
        data.fullLevel = stableRead(PIN_LEVEL_FULL, ELECTRODE_WET_STATE);
        digitalWrite(PIN_SENSOR_COMMON, LOW);

        data.levelValid = (!data.fullLevel || data.midLevel) &&
                          (!data.midLevel || data.lowLevel);
        if (!data.levelValid) { data.levelPercent = 0; return; }
        if (data.fullLevel) data.levelPercent = 100;
        else if (data.midLevel) data.levelPercent = 50;
        else if (data.lowLevel) data.levelPercent = 25;
        else data.levelPercent = 0;
        return;
    }

    const bool lowActive = stableRead(PIN_LEVEL_LOW, FLOAT_ACTIVE_STATE);
    const bool midActive = stableRead(PIN_LEVEL_MID, FLOAT_ACTIVE_STATE);
    const bool fullActive = stableRead(PIN_LEVEL_FULL, FLOAT_ACTIVE_STATE);
    data.lowLevel = lowActive;
    data.midLevel = midActive;
    data.highLevel = fullActive;
    data.fullLevel = fullActive;
    data.levelValid = (!midActive || lowActive) && (!fullActive || midActive);
    if (!data.levelValid) {
        data.levelPercent = 0;
        return;
    }
    if (fullActive) data.levelPercent = 100;
    else if (midActive) data.levelPercent = 50;
    else if (lowActive) data.levelPercent = 25;
    else data.levelPercent = 0;
}

void beginTemperatureSensor() {
    pinMode(PIN_NTC, INPUT);
    analogReadResolution(12);
    (void)analogRead(PIN_NTC); // Initialize this ADC channel before configuring attenuation.
    analogSetPinAttenuation(PIN_NTC, ADC_11db);
}

void updateTemperatureSensor(SystemStatus &status) {
    uint32_t sumMilliVolts = 0;
    uint32_t sumRaw = 0;
    for (uint8_t i = 0; i < NTC_ADC_SAMPLES; ++i) {
        sumMilliVolts += analogReadMilliVolts(PIN_NTC);
        sumRaw += analogRead(PIN_NTC);
        delayMicroseconds(250);
    }
    const float nodeV = (sumMilliVolts / static_cast<float>(NTC_ADC_SAMPLES)) / 1000.0f;
    status.temperatureNodeV = nodeV;
    status.temperatureRawAdc = static_cast<uint16_t>(sumRaw / NTC_ADC_SAMPLES);

    // Divider: 3V3 -- NTC -- ADC node -- fixed 10k -- GND.
    // Vnode = Vcc * Rfixed / (Rntc + Rfixed).
    if (nodeV <= 0.02f || nodeV >= (NTC_DIVIDER_SUPPLY_V - 0.02f)) {
        status.temperatureValid = false;
        return;
    }

    // For the documented topology (3V3 -> NTC -> ADC -> fixed resistor -> GND):
    // Rntc = Rfixed * (Vsupply / Vnode - 1).
    const float ntcResistance = NTC_FIXED_RESISTOR_OHMS *
        (NTC_DIVIDER_SUPPLY_V / nodeV - 1.0f);
    status.thermistorResistanceOhms = ntcResistance;
    if (!isfinite(ntcResistance) || ntcResistance <= 0.0f) {
        status.temperatureValid = false;
        return;
    }

    const float inverseT = (1.0f / NTC_NOMINAL_TEMPERATURE_K) +
        (logf(ntcResistance / NTC_NOMINAL_RESISTANCE_OHMS) / NTC_BETA_K);
    const float temperatureK = 1.0f / inverseT;
    const float temperatureC = temperatureK - 273.15f;

    if (!isfinite(temperatureC) || temperatureC < -40.0f || temperatureC > 125.0f) {
        status.temperatureValid = false;
        return;
    }
    status.temperatureC = temperatureC;
    status.temperatureValid = true;
}
