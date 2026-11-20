#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "CurrentSensor.h"

static Preferences prefs;
static float prototypeZeroV = PROTOTYPE_ZERO_OFFSET_V_DEFAULT;
static float currentA = 0.0f;
static uint32_t lastSampleUs = 0;
static uint32_t windowStartMs = 0;
static uint32_t sampleCount = 0;
static double sumV = 0.0;
static double sumV2 = 0.0;

void beginCurrentSensor() {
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_PUMP_CURRENT, ADC_11db);
    prefs.begin("waterctl", false);
    prototypeZeroV = prefs.getFloat("protoZero", PROTOTYPE_ZERO_OFFSET_V_DEFAULT);
    lastSampleUs = micros();
    windowStartMs = millis();
}

void updateCurrentSensor(OperatingMode mode) {
    const uint32_t nowUs = micros();
    // Prototype DC current: frequent averaging; real ACS712: sample fast enough
    // to capture the 50Hz AC waveform for RMS calculation.
    const uint32_t intervalUs = (mode == REAL_MODE) ? 500 : 1000;
    if ((uint32_t)(nowUs - lastSampleUs) < intervalUs) return;
    lastSampleUs = nowUs;

    const float adcV = analogReadMilliVolts(PIN_PUMP_CURRENT) / 1000.0f;
    if (mode == PROTOTYPE_MODE) {
        sumV += adcV;
        sampleCount++;
        if ((uint32_t)(millis() - windowStartMs) >= 200 && sampleCount > 0) {
            const float averageV = (float)(sumV / sampleCount);
            const float shuntV = fmaxf(0.0f, averageV - prototypeZeroV);
            currentA = shuntV / SHUNT_OHMS; // No LM358 amplifier in Revision 6.
            sumV = 0.0;
            sumV2 = 0.0;
            sampleCount = 0;
            windowStartMs = millis();
        }
        return;
    }

    // ACS712 AC RMS: standard deviation of the sampled waveform, then undo
    // the external divider ratio to recover sensor-output RMS voltage.
    sumV += adcV;
    sumV2 += (double)adcV * adcV;
    sampleCount++;
    if ((uint32_t)(millis() - windowStartMs) >= 200 && sampleCount > 50) {
        const double mean = sumV / sampleCount;
        double variance = (sumV2 / sampleCount) - (mean * mean);
        if (variance < 0.0) variance = 0.0;
        const float adcRmsV = sqrtf((float)variance);
        const float sensorRmsV = adcRmsV * ACS712_DIVIDER_CORRECTION;
        currentA = sensorRmsV / ACS712_SENSITIVITY_V_PER_A;
        sumV = 0.0;
        sumV2 = 0.0;
        sampleCount = 0;
        windowStartMs = millis();
    }
}

float getPumpCurrentA() { return currentA; }

bool calibrateCurrentZero(OperatingMode mode) {
    if (mode != PROTOTYPE_MODE) return false;
    // Call only with the pump physically OFF and mux selecting the shunt input.
    const uint16_t n = 300;
    double total = 0.0;
    for (uint16_t i = 0; i < n; ++i) {
        total += analogReadMilliVolts(PIN_PUMP_CURRENT) / 1000.0;
        delay(3);
    }
    prototypeZeroV = (float)(total / n);
    prefs.putFloat("protoZero", prototypeZeroV);
    sumV = 0.0;
    sumV2 = 0.0;
    sampleCount = 0;
    windowStartMs = millis();
    currentA = 0.0f;
    return true;
}
