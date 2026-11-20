#include <Arduino.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "Outputs.h"

static bool buzzerState = false;
static uint32_t lastBuzzerToggle = 0;

void beginOutputs() {
    pinMode(PIN_PUMP_RELAY, OUTPUT);
    pinMode(PIN_LED_1, OUTPUT);
    pinMode(PIN_LED_2, OUTPUT);
    pinMode(PIN_LED_3, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_PUMP_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
      
    digitalWrite(PIN_LED_1, LOW);
    digitalWrite(PIN_LED_2, LOW);
    digitalWrite(PIN_LED_3, LOW);
    digitalWrite(PIN_BUZZER, LOW);
}

void setPumpOutput(bool on) {
    digitalWrite(PIN_PUMP_RELAY, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

void updateIndicators(const SystemStatus &status, const WaterData &waterData) {
    // LED1/LED2 encode water level; LED3 exclusively indicates pump running.
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
    digitalWrite(PIN_LED_3, status.pumpCommand ? HIGH : LOW);
}

void updateBuzzer(bool faultActive) {
    if (!faultActive) { buzzerState = false; digitalWrite(PIN_BUZZER, LOW); return; }
    if (millis() - lastBuzzerToggle >= BUZZER_TOGGLE_MS) {
        lastBuzzerToggle = millis();
        buzzerState = !buzzerState;
        digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
    }
}
