#include <Arduino.h>
#include <cstring>
#include "Hardware.h"
#include "AppConfig.h"
#include "GsmManager.h"

static HardwareSerial GSM(2); // Use UART2 for SIM800L; ESP32 RX GPIO16, TX GPIO17.
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

static bool waitFor(const char *expected, uint32_t timeoutMs, String *capturedResponse = nullptr) {
    String response;
    const uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeoutMs) {
        while (GSM.available()) {
            if (response.length() < 256) response += static_cast<char>(GSM.read());
            else (void)GSM.read();
            if (response.indexOf(expected) >= 0) {
                if (capturedResponse) *capturedResponse = response;
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (capturedResponse) *capturedResponse = response;
    return false;
}

static bool sendCommand(const char *command, const char *expected, uint32_t timeoutMs) {
    while (GSM.available()) (void)GSM.read();
    GSM.println(command);
    String response;
    const bool matched = waitFor(expected, timeoutMs, &response);
    response.replace('\r', ' ');
    response.replace('\n', ' ');
    response.trim();
    if (!response.length()) response = "<no response>";
    Serial.printf("GSM init: TX [%s], RX [%s], expected [%s]: %s\n",
                  command, response.c_str(), expected,
                  matched ? "matched" : "timeout");
    return matched;
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

    Serial.printf("GSM UART: baud=%lu ESP32_RX=GPIO%d ESP32_TX=GPIO%d reset=GPIO%u\n",
                  static_cast<unsigned long>(GSM_BAUD), PIN_GSM_RX, PIN_GSM_TX,
                  static_cast<unsigned>(PIN_GSM_RESET));
    GSM.begin(GSM_BAUD, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);
    // The successful standalone SIM800L test allowed three seconds after UART
    // startup. Give the modem the same boot/baud-detection interval here.
    vTaskDelay(pdMS_TO_TICKS(3000));
    bool atReady = false;
    for (uint8_t attempt = 1; attempt <= 5 && !atReady; ++attempt) {
        Serial.printf("GSM: AT handshake attempt %u/5\n", attempt);
        atReady = sendCommand("AT", "OK", 2000);
        if (!atReady) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!atReady) return false;
    sendCommand("ATE0", "OK", 1000);
    sendCommand("AT+CMGF=1", "OK", 1000);
    sendCommand("AT+CSCS=\"GSM\"", "OK", 1000);
    // Read-only diagnostics: deliberately do not query/log the SIM CCID/IMSI.
    sendCommand("AT+CSQ", "OK", 1500);
    sendCommand("AT+CREG?", "OK", 1500);
    sendCommand("AT+CBC", "OK", 1500);
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
