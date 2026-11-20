#include <Arduino.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "GsmManager.h"

static HardwareSerial GSM(1);
static bool ready = false;
static uint32_t lastInitAttempt = 0;

static bool waitFor(const char *expected, uint32_t timeoutMs) {
    String response;
    const uint32_t start = millis();

    while (millis() - start < timeoutMs) {
        while (GSM.available()) {
            char c = (char)GSM.read();
            response += c;
            if (response.indexOf(expected) >= 0) return true;
        }
        delay(2);
    }
    return false;
}

static bool sendCommand(const char *command, const char *expected, uint32_t timeoutMs) {
    while (GSM.available()) GSM.read();
    GSM.println(command);
    return waitFor(expected, timeoutMs);
}

void beginGsm() {
    if (!GSM_ENABLED) return;

    pinMode(PIN_GSM_RESET, OUTPUT);
    digitalWrite(PIN_GSM_RESET, HIGH); // SIM800L RESET is typically active LOW.
    delay(100);
    digitalWrite(PIN_GSM_RESET, LOW);
    delay(150);
    digitalWrite(PIN_GSM_RESET, HIGH);
    delay(1000);
    GSM.begin(GSM_BAUD, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);
    delay(500);
    lastInitAttempt = millis();

    ready = sendCommand("AT", "OK", 2000);
    if (ready) {
        sendCommand("ATE0", "OK", 1000);
        sendCommand("AT+CMGF=1", "OK", 1000);
        sendCommand("AT+CSCS=\"GSM\"", "OK", 1000);
    }
}

void updateGsm() {
    if (!GSM_ENABLED || ready) return;

    if (millis() - lastInitAttempt > 15000) {
        lastInitAttempt = millis();
        ready = sendCommand("AT", "OK", 2000);
        if (ready) {
            sendCommand("ATE0", "OK", 1000);
            sendCommand("AT+CMGF=1", "OK", 1000);
        }
    }
}

bool gsmIsReady() {
    return ready;
}

void sendGsmAlert(const char *message) {
    if (!GSM_ENABLED || !ready) return;
    if (String(ALERT_PHONE_NUMBER).indexOf('X') >= 0) return;

    while (GSM.available()) GSM.read();

    GSM.println("AT+CMGS=\"" + String(ALERT_PHONE_NUMBER) + "\"");
    if (!waitFor(">", 5000)) return;

    GSM.print(message);
    GSM.write(26); // Ctrl+Z
    waitFor("OK", 15000);
}
