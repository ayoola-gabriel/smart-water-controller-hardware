#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <cstring>
#include "Hardware.h"
#include "AppConfig.h"
#include "GpsManager.h"

static TinyGPSPlus gps;
static portMUX_TYPE gpsMux = portMUX_INITIALIZER_UNLOCKED;
static bool gpsTaskStarted = false;
static bool latestLocationValid = false;
static double latestLatitude = 0.0;
static double latestLongitude = 0.0;
static uint32_t latestSatellites = 0;
static uint32_t charactersReceived = 0;
static uint32_t validSentences = 0;
static uint32_t failedSentences = 0;
static uint32_t lastLocationUpdateMs = 0;
static char latestNmeaLine[128] = {};
static bool nmeaLinePending = false;

static void gpsReaderTask(void *) {
    char line[sizeof(latestNmeaLine)] = {};
    size_t lineLength = 0;
    uint32_t receivedCharacterCount = 0;

    for (;;) {
        while (Serial1.available() > 0) {
            const char c = static_cast<char>(Serial1.read());
            ++receivedCharacterCount;
            gps.encode(c);

            if (c == '\n') {
                line[lineLength] = '\0';
                portENTER_CRITICAL(&gpsMux);
                memcpy(latestNmeaLine, line, sizeof(latestNmeaLine));
                nmeaLinePending = true;
                portEXIT_CRITICAL(&gpsMux);
                lineLength = 0;
            } else if (c != '\r') {
                if (lineLength + 1 < sizeof(line)) line[lineLength++] = c;
                else lineLength = 0;
            }
        }

        const bool locationUpdated = gps.location.isUpdated() && gps.location.isValid();
        const bool locationValid = gps.location.isValid() && gps.location.age() < 10000;
        const double latitude = locationValid ? gps.location.lat() : latestLatitude;
        const double longitude = locationValid ? gps.location.lng() : latestLongitude;
        const uint32_t satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
        const uint32_t passed = gps.passedChecksum();
        const uint32_t failed = gps.failedChecksum();
        const uint32_t updateTime = locationUpdated ? millis() : lastLocationUpdateMs;

        portENTER_CRITICAL(&gpsMux);
        charactersReceived = receivedCharacterCount;
        latestLocationValid = locationValid;
        if (locationValid) {
            latestLatitude = latitude;
            latestLongitude = longitude;
        }
        latestSatellites = satellites;
        validSentences = passed;
        failedSentences = failed;
        if (locationUpdated) lastLocationUpdateMs = updateTime;
        portEXIT_CRITICAL(&gpsMux);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void beginGps() {
    if (!GPS_ENABLED) return;
    Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    gpsTaskStarted = xTaskCreatePinnedToCore(
        gpsReaderTask, "swc-gps", 4096, nullptr, 1, nullptr, 1) == pdPASS;
    if (!gpsTaskStarted) Serial.println("GPS parser task could not start");
}

void updateGps(SystemStatus &status) {
    if (!GPS_ENABLED || !gpsTaskStarted) {
        status.gpsValid = false;
        return;
    }

    portENTER_CRITICAL(&gpsMux);
    status.gpsValid = latestLocationValid;
    if (latestLocationValid) {
        status.gpsLatitude = latestLatitude;
        status.gpsLongitude = latestLongitude;
    }
    status.gpsSatellites = latestSatellites;
    status.gpsCharactersReceived = charactersReceived;
    status.gpsValidSentences = validSentences;
    status.gpsFailedSentences = failedSentences;
    status.gpsLastLocationUpdateMs = lastLocationUpdateMs;
    portEXIT_CRITICAL(&gpsMux);
}

bool consumeGpsNmeaLine(char *buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return false;
    portENTER_CRITICAL(&gpsMux);
    const bool pending = nmeaLinePending;
    if (pending) {
        strncpy(buffer, latestNmeaLine, bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
        nmeaLinePending = false;
    }
    portEXIT_CRITICAL(&gpsMux);
    return pending;
}
