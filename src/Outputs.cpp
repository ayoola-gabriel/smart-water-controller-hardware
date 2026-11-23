#include <Arduino.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "Outputs.h"

static bool buzzerState = false;
static uint32_t lastBuzzerToggle = 0;
static uint32_t buzzerStartedAt = 0;
static bool previousFaultActive = false;
static uint32_t lastFaultLedToggle = 0;
static bool faultLedState = false;
static bool previousIndicatorFault = false;

void beginOutputs() {
    pinMode(PIN_PUMP_RELAY, OUTPUT);
    pinMode(PIN_LED_1, OUTPUT);
    pinMode(PIN_LED_2, OUTPUT);
    pinMode(PIN_LED_3, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_PUMP_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(PIN_LED_1, HIGH);
    delay(1000);
    digitalWrite(PIN_LED_2, HIGH);
    delay(1000);
    digitalWrite(PIN_LED_3, HIGH);
    delay(1000);
    digitalWrite(PIN_LED_1, LOW);
    digitalWrite(PIN_LED_2, LOW);
    digitalWrite(PIN_LED_3, LOW);
    digitalWrite(PIN_BUZZER, LOW);
}

void setPumpOutput(bool on) {
    digitalWrite(PIN_PUMP_RELAY, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

void updateIndicators(const SystemStatus &status, const WaterData &waterData) {
    // LED1/LED2 encode water level. LED3 indicates pump state normally and
    // blinks as the fault indicator whenever a fault is latched.
    // LOW: LED1; BETWEEN: LED1+LED2; FULL: LED2.
    bool led1 = false;
    bool led2 = false;

    if (!waterData.levelValid) {
        led1 = false;
        led2 = false;
    } else if (waterData.fullLevel || waterData.levelPercent >= 100.0f) {
        led2 = true;
    } else if (waterData.midLevel || waterData.levelPercent > 0.0f) {
        led1 = true;
        led2 = true;
    } else {
        led1 = true;
    }

    digitalWrite(PIN_LED_1, led1 ? HIGH : LOW);
    digitalWrite(PIN_LED_2, led2 ? HIGH : LOW);

    const uint32_t now = millis();
    const bool faultActive = status.fault != NO_FAULT;
    if (faultActive) {
        if (!previousIndicatorFault) {
            previousIndicatorFault = true;
            faultLedState = true;
            lastFaultLedToggle = now;
        } else if (static_cast<uint32_t>(now - lastFaultLedToggle) >= FAULT_LED_TOGGLE_MS) {
            lastFaultLedToggle = now;
            faultLedState = !faultLedState;
        }
        digitalWrite(PIN_LED_3, faultLedState ? HIGH : LOW);
    } else {
        previousIndicatorFault = false;
        faultLedState = false;
        digitalWrite(PIN_LED_3, status.pumpCommand ? HIGH : LOW);
    }
}

void updateBuzzer(bool faultActive) {
    const uint32_t now = millis();
    if (!faultActive) {
        buzzerState = false;
        previousFaultActive = false;
        buzzerStartedAt = 0;
        digitalWrite(PIN_BUZZER, LOW);
        return;
    }
    if (!previousFaultActive) {
        previousFaultActive = true;
        buzzerStartedAt = now;
        lastBuzzerToggle = now;
        buzzerState = false;
        digitalWrite(PIN_BUZZER, LOW);
    }
    if (static_cast<uint32_t>(now - buzzerStartedAt) >= BUZZER_MAX_DURATION_MS) {
        buzzerState = false;
        digitalWrite(PIN_BUZZER, LOW);
        return;
    }
    if (static_cast<uint32_t>(now - lastBuzzerToggle) >= BUZZER_TOGGLE_MS) {
        lastBuzzerToggle = now;
        buzzerState = !buzzerState;
        digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
    }
}
