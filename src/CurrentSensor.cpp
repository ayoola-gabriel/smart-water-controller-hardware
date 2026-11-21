#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "CurrentSensor.h"

static Preferences prefs;
static bool prefsReady = false;
static float prototypeZeroV = PROTOTYPE_ZERO_OFFSET_V_DEFAULT;
static float currentA = 0.0f;
static uint32_t lastSampleUs = 0;
static uint32_t windowStartMs = 0;
static uint32_t sampleCount = 0;
static double sumV = 0.0;
static double sumV2 = 0.0;
static bool calibrating = false;
static double calibrationSumV = 0.0;
static uint16_t calibrationSamples = 0;
static uint32_t calibrationNextMs = 0;

static float readAveragedAdcVoltage(uint16_t &rawAverage) {
    uint32_t millivoltsSum = 0;
    uint32_t rawSum = 0;
    for (uint8_t i = 0; i < CURRENT_ADC_AVERAGE_SAMPLES; ++i) {
        millivoltsSum += analogReadMilliVolts(PIN_PUMP_CURRENT);
        rawSum += analogRead(PIN_PUMP_CURRENT);
    }
    rawAverage = static_cast<uint16_t>(rawSum / CURRENT_ADC_AVERAGE_SAMPLES);
    return (millivoltsSum / static_cast<float>(CURRENT_ADC_AVERAGE_SAMPLES)) / 1000.0f;
}

void beginCurrentSensor() {
    pinMode(PIN_PUMP_CURRENT, INPUT);
    analogReadResolution(12);
    (void)analogRead(PIN_PUMP_CURRENT); // Initialize the ADC channel before configuring attenuation.
    analogSetPinAttenuation(PIN_PUMP_CURRENT, ADC_11db);
    (void)analogReadMilliVolts(PIN_PUMP_CURRENT); // discard the first conversion after ADC setup
    prefsReady = prefs.begin("waterctl", false);
    if (!prefsReady) {
        Serial.println("Preferences unavailable; using default current-sensor calibration");
    } else if (prefs.isKey("protoZero")) {
        const float savedZero = prefs.getFloat("protoZero", PROTOTYPE_ZERO_OFFSET_V_DEFAULT);
        if (isfinite(savedZero) && savedZero >= 0.0f && savedZero <= 3.3f) {
            prototypeZeroV = savedZero;
        } else {
            Serial.println("Invalid saved current zero; using default calibration");
        }
    } else {
        Serial.println("Current zero key absent; using default calibration");
    }
    lastSampleUs = micros();
    windowStartMs = millis();
}

void updateCurrentSensor(OperatingMode mode, SystemStatus &status, bool pumpOff) {
    if (calibrating && !pumpOff) {
        calibrating = false;
        calibrationSumV = 0.0;
        calibrationSamples = 0;
        Serial.println("Current zero calibration cancelled because the pump is ON");
        return;
    }
    const uint32_t nowMs = millis();
    const uint32_t nowUs = micros();
    const uint32_t intervalUs = (mode == REAL_MODE) ? 500 : 1000;
    const bool calibrationDue = calibrating &&
        (int32_t)(nowMs - calibrationNextMs) >= 0;
    if (!calibrating && (uint32_t)(nowUs - lastSampleUs) < intervalUs) return;
    if (calibrating && !calibrationDue) return;
    lastSampleUs = nowUs;

    status.currentAdcVoltage = readAveragedAdcVoltage(status.currentRawAdc);
    status.currentZeroVoltage = mode == PROTOTYPE_MODE ? prototypeZeroV : 0.0f;

    if (calibrating) {
        calibrationSumV += status.currentAdcVoltage;
        ++calibrationSamples;
        calibrationNextMs = nowMs + CURRENT_CALIBRATION_SAMPLE_INTERVAL_MS;
        if (calibrationSamples >= CURRENT_CALIBRATION_SAMPLE_COUNT) {
            prototypeZeroV = static_cast<float>(calibrationSumV / calibrationSamples);
            status.currentZeroVoltage = prototypeZeroV;
            calibrationSumV = 0.0;
            calibrationSamples = 0;
            calibrating = false;
            sumV = 0.0;
            sumV2 = 0.0;
            sampleCount = 0;
            windowStartMs = nowMs;
            currentA = 0.0f;
            if (prefsReady && prefs.putFloat("protoZero", prototypeZeroV) == 0) {
                Serial.println("Current zero calibration completed but could not be persisted");
            } else {
                Serial.printf("Prototype current zero calibrated: %.3f V\n", prototypeZeroV);
            }
        }
        status.pumpCurrentA = currentA;
        return;
    }

    const double adcV = status.currentAdcVoltage;
    if (mode == PROTOTYPE_MODE) {
        sumV += adcV;
        ++sampleCount;
        if ((uint32_t)(nowMs - windowStartMs) >= 200 && sampleCount > 0) {
            const float averageV = static_cast<float>(sumV / sampleCount);
            const float shuntV = fmaxf(0.0f, averageV - prototypeZeroV);
            currentA = shuntV / SHUNT_OHMS; // Revision 6 has no LM358 amplifier.
            sumV = 0.0;
            sampleCount = 0;
            windowStartMs = nowMs;
        }
    } else {
        // ACS712 AC RMS: standard deviation over each 200ms window, then
        // undo the measured 10k/10k voltage divider.
        sumV += adcV;
        sumV2 += adcV * adcV;
        ++sampleCount;
        if ((uint32_t)(nowMs - windowStartMs) >= 200 && sampleCount > 50) {
            const double mean = sumV / sampleCount;
            double variance = (sumV2 / sampleCount) - (mean * mean);
            if (variance < 0.0) variance = 0.0;
            const float adcRmsV = sqrtf(static_cast<float>(variance));
            const float sensorRmsV = adcRmsV * ACS712_DIVIDER_CORRECTION;
            currentA = sensorRmsV / ACS712_SENSITIVITY_V_PER_A;
            sumV = 0.0;
            sumV2 = 0.0;
            sampleCount = 0;
            windowStartMs = nowMs;
        }
    }
    status.pumpCurrentA = currentA;
}

float getPumpCurrentA() { return currentA; }

bool calibrateCurrentZero(OperatingMode mode) {
    if (mode != PROTOTYPE_MODE || calibrating) return false;
    calibrating = true;
    calibrationSumV = 0.0;
    calibrationSamples = 0;
    calibrationNextMs = millis();
    Serial.println("Prototype current zero calibration started; keep pump OFF");
    return true;
}
