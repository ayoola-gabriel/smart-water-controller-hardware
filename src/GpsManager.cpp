#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "Hardware.h"
#include "AppConfig.h"
#include "GpsManager.h"

static HardwareSerial GPS_UART(2);
static TinyGPSPlus gps;

void beginGps() {
    if (!GPS_ENABLED) return;
    GPS_UART.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
}

void updateGps(SystemStatus &status) {
    if (!GPS_ENABLED) return;

    while (GPS_UART.available()) {
        gps.encode((char)GPS_UART.read());
    }

    if (gps.location.isValid() && gps.location.age() < 10000) {
        status.gpsValid = true;
        status.gpsLatitude = gps.location.lat();
        status.gpsLongitude = gps.location.lng();
    } else {
        status.gpsValid = false;
    }

    if (gps.satellites.isValid()) {
        status.gpsSatellites = gps.satellites.value();
    }
}
