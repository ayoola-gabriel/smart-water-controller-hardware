#include <Arduino.h>
#include <cstring>
#include "Hardware.h"
#include "AppConfig.h"
#include "GsmManager.h"

static HardwareSerial GSM(1);
static portMUX_TYPE gsmStateMux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t gsmAlertQueue = nullptr;
static bool ready = false;

struct GsmAlert {
    char text[180];
};

static bool isReady() {
    portENTER_CRITICAL(&gsmStateMux);
    const bool value = ready;
    portEXIT_CRITICAL(&gsmStateMux);
    return value;
}

static void setReady(bool value) {
    portENTER_CRITICAL(&gsmStateMux);
    ready = value;
    portEXIT_CRITICAL(&gsmStateMux);
}

static bool waitFor(const char *expected, uint32_t timeoutMs) {
    String response;
    const uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeoutMs) {
        while (GSM.available()) {
            response += static_cast<char>(GSM.read());
            if (response.indexOf(expected) >= 0) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static bool sendCommand(const char *command, const char *expected, uint32_t timeoutMs) {
    while (GSM.available()) (void)GSM.read();
    GSM.println(command);
    return waitFor(expected, timeoutMs);
}

static bool sendSms(const char *message) {
    if (String(ALERT_PHONE_NUMBER).indexOf('X') >= 0) return false;
    while (GSM.available()) (void)GSM.read();
    GSM.println("AT+CMGS=\"" + String(ALERT_PHONE_NUMBER) + "\"");
    if (!waitFor(">", 5000)) return false;
    GSM.print(message);
    GSM.write(26); // Ctrl+Z
    return waitFor("OK", 15000);
}

static bool initializeModem() {
    pinMode(PIN_GSM_RESET, OUTPUT);
    digitalWrite(PIN_GSM_RESET, HIGH); // SIM800L reset is active LOW.
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(PIN_GSM_RESET, LOW);
    vTaskDelay(pdMS_TO_TICKS(150));
    digitalWrite(PIN_GSM_RESET, HIGH);
    vTaskDelay(pdMS_TO_TICKS(1000));

    GSM.begin(GSM_BAUD, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!sendCommand("AT", "OK", 2000)) return false;
    sendCommand("ATE0", "OK", 1000);
    sendCommand("AT+CMGF=1", "OK", 1000);
    sendCommand("AT+CSCS=\"GSM\"", "OK", 1000);
    return true;
}

static void gsmWorker(void *) {
    bool initialized = false;
    uint32_t lastInitAttempt = 0;
    for (;;) {
        if (!initialized) {
            if (lastInitAttempt == 0 || (uint32_t)(millis() - lastInitAttempt) >= 15000) {
                lastInitAttempt = millis();
                initialized = initializeModem();
                setReady(initialized);
                Serial.println(initialized ? "GSM SMS modem ready" : "GSM init failed; retry scheduled");
            }
            if (!initialized) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
        }

        GsmAlert alert{};
        if (gsmAlertQueue && xQueueReceive(gsmAlertQueue, &alert, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (!sendSms(alert.text)) {
                setReady(false);
                initialized = false;
                Serial.println("GSM SMS send failed; modem reinitialization scheduled");
            } else {
                Serial.println("GSM SMS alert sent");
            }
        }
    }
}

void beginGsm() {
    if (!GSM_ENABLED) return;
    gsmAlertQueue = xQueueCreate(4, sizeof(GsmAlert));
    if (!gsmAlertQueue) {
        Serial.println("GSM alert queue unavailable; SMS alerts disabled");
        return;
    }
    if (xTaskCreatePinnedToCore(gsmWorker, "swc-gsm", 6144,
                                nullptr, 1, nullptr, 0) != pdPASS) {
        gsmAlertQueue = nullptr;
        Serial.println("GSM worker could not start; SMS alerts disabled");
    }
}

void updateGsm() {
    // Modem initialization, retry and SMS work run in the dedicated worker.
}

bool gsmIsReady() {
    return isReady();
}

void sendGsmAlert(const char *message) {
    if (!GSM_ENABLED || !message || !gsmAlertQueue) return;
    if (String(ALERT_PHONE_NUMBER).indexOf('X') >= 0) return;
    GsmAlert alert{};
    strncpy(alert.text, message, sizeof(alert.text) - 1);
    if (xQueueSend(gsmAlertQueue, &alert, 0) != pdTRUE) {
        Serial.println("GSM alert queue full; alert not queued");
    }
}
