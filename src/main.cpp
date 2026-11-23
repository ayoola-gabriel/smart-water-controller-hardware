#include <Arduino.h>
#include <WiFi.h>

#include "AppConfig.h"
#include "Hardware.h"
#include "Types.h"
#include "Sensors.h"
#include "CurrentSensor.h"
#include "DisplayManager.h"
#include "Outputs.h"
#include "PumpController.h"
#include "GsmManager.h"
#include "GpsManager.h"
#include "WebManager.h"
#include "RemoteManager.h"

WaterData gWater;
SystemStatus gStatus;
static OperatingMode gMode = PROTOTYPE_MODE;
static FaultType previousFault = NO_FAULT;
static uint32_t lastSensorMs = 0;
static uint32_t lastTemperatureMs = 0;
static uint32_t lastControlMs = 0;
static uint32_t lastDisplayMs = 0;
static uint32_t lastGpsDiagnosticMs = 0;
static bool manualPumpRequest = false;

bool requestFaultClear() {
    if (!gWater.levelValid || gStatus.pumpCommand) {
        Serial.println("Fault clear rejected locally: water-level reading must be valid and pump must be stopped");
        return false;
    }
    clearLatchedFault(gStatus);
    return true;
}

static const char *faultText(FaultType f) {
    switch (f) {
        case LEVEL_SENSOR_FAULT: return "LEVEL SENSOR FAULT";
        case CURRENT_SENSOR_FAULT: return "CURRENT SENSOR FAULT";
        case LOW_CURRENT_FAULT: return "LOW CURRENT FAULT";
        case OVERCURRENT_FAULT: return "OVERCURRENT FAULT";
        case MAX_RUNTIME_FAULT: return "MAX RUNTIME FAULT";
        default: return "SYSTEM NORMAL";
    }
}

static void handleFaultAlert() {
    if (gStatus.fault != previousFault && gStatus.fault != NO_FAULT) {
        char message[180];
        if (gStatus.gpsValid) {
            snprintf(message, sizeof(message),
                "Water controller alert: %s. Pump stopped. Location: %.6f,%.6f",
                faultText(gStatus.fault),
                gStatus.gpsLatitude,
                gStatus.gpsLongitude);
        } else {
            snprintf(message, sizeof(message),
                "Water controller alert: %s. Pump stopped. GPS fix unavailable.",
                faultText(gStatus.fault));
        }
        sendGsmAlert(message);
        previousFault = gStatus.fault;
    } else if (gStatus.fault == NO_FAULT) {
        previousFault = NO_FAULT;
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    beginOutputs(); // Pump output is forced OFF first.

    gMode = detectOperatingMode();
    gStatus.operatingMode = gMode;

    beginLevelSensors(gMode);
    beginTemperatureSensor();
    beginCurrentSensor();

    beginDisplay();

    beginGsm();
    beginGps();
    beginWebManager();
    beginRemoteManager();

    updateLevelSensors(gMode, gWater);

    // Do not start pump during boot. Automatic logic starts only after
    // the first control cycle and valid sensor state.
    setPumpOutput(false);
    gStatus.pumpCommand = false;
    gStatus.pumpState = PUMP_STOPPED;
    renderDisplay(gWater, gStatus, gMode, isBlePairingPending(), getBlePairingPasskey());

    Serial.println();
    Serial.println("Smart Water Controller booted.");
    Serial.println(gMode == PROTOTYPE_MODE ? "Mode: PROTOTYPE" : "Mode: REAL");
}

void loop() {
    const uint32_t now = millis();

    updateRemoteManager(gWater, gStatus);
    updateGsm();
    gStatus.gsmReady = gsmIsReady();
    updateGps(gStatus);

    if (now - lastSensorMs >= SENSOR_UPDATE_MS) {
        lastSensorMs = now;
        updateLevelSensors(gMode, gWater);
    }

    if (now - lastTemperatureMs >= TEMPERATURE_UPDATE_MS) {
        lastTemperatureMs = now;
        updateTemperatureSensor(gStatus);
    }

    // Current sampler runs on every loop pass so the ACS712 gets a useful
    // waveform sampling rate for 50Hz RMS estimation.
    updateCurrentSensor(gMode, gStatus, !gStatus.pumpCommand);

    if (consumeWebCurrentCalibrationRequest()) {
        if (gStatus.pumpCommand) {
            Serial.println("Current zero calibration rejected: pump must be OFF");
        } else if (!calibrateCurrentZero(gMode)) {
            Serial.println("Current zero calibration unavailable outside idle prototype mode");
        }
    }

    const bool webClearFaultRequested = consumeWebClearFaultRequest();
    const bool bleClearFaultRequested = consumeBleClearFaultRequest();
    const bool cloudClearFaultRequested = consumeCloudClearFaultRequest();
    const bool clearFaultRequested = webClearFaultRequested ||
        bleClearFaultRequested || cloudClearFaultRequested;
    if (clearFaultRequested) {
        requestFaultClear();
    }

    if (now - lastControlMs >= CONTROL_UPDATE_MS) {
        lastControlMs = now;

        PumpMode requestedMode = getWebPumpMode();
        manualPumpRequest = getWebManualPumpRequest();
        // BLE control is local and takes precedence over the legacy LAN API.
        if (isBleControlActive()) {
            requestedMode = getBlePumpMode();
            manualPumpRequest = getBleManualPumpRequest();
        } else if (isCloudControlActive()) {
            requestedMode = getCloudPumpMode();
            manualPumpRequest = getCloudManualPumpRequest();
        }

        updatePumpController(
            gMode,
            gWater,
            gStatus,
            requestedMode,
            manualPumpRequest,
            getPumpCurrentA()
        );

        updateIndicators(gStatus, gWater);
        updateBuzzer(gStatus.fault != NO_FAULT);
        handleFaultAlert();
    }

    if (now - lastDisplayMs >= DISPLAY_UPDATE_MS) {
        lastDisplayMs = now;
        renderDisplay(gWater, gStatus, gMode, isBlePairingPending(), getBlePairingPasskey());

        Serial.printf(
            "Mode=%s Level=%.0f%% Valid=%d Pump=%s Current=%.3fA Temp=%s%.1fC Fault=%s WiFi=%s GSM=%s GPS=%s\n\n",
            gMode == PROTOTYPE_MODE ? "PROTOTYPE" : "REAL",
            gWater.levelPercent,
            gWater.levelValid,
            gStatus.pumpCommand ? "ON" : "OFF",
            gStatus.pumpCurrentA,
            gStatus.temperatureValid ? "" : "INVALID/",
            gStatus.temperatureC,
            faultText(gStatus.fault),
            WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE",
            gsmIsReady() ? "READY" : "NOT_READY",
            gStatus.gpsValid ? "FIX" : "NO_FIX"
        );
        // Serial.printf("NTC: ADC=%u Node=%.3fV Rntc=%.0f ohm TempValid=%d\n",
        //     gStatus.temperatureRawAdc, gStatus.temperatureNodeV,
        //     gStatus.thermistorResistanceOhms, gStatus.temperatureValid);
        // Serial.printf("Current ADC: raw=%u averaged=%.3fV zero=%.3fV current=%.3fA\n",
        //     gStatus.currentRawAdc, gStatus.currentAdcVoltage,
        //     gStatus.currentZeroVoltage, gStatus.pumpCurrentA);
    }

    if (now - lastGpsDiagnosticMs >= 5000) {
        lastGpsDiagnosticMs = now;
        Serial.printf("GPS: chars=%lu validSentences=%lu failedSentences=%lu sats=%lu locationValid=%d lastFixAgeMs=%lu\n",
            static_cast<unsigned long>(gStatus.gpsCharactersReceived),
            static_cast<unsigned long>(gStatus.gpsValidSentences),
            static_cast<unsigned long>(gStatus.gpsFailedSentences),
            static_cast<unsigned long>(gStatus.gpsSatellites),
            gStatus.gpsValid,
            gStatus.gpsLastLocationUpdateMs
                ? static_cast<unsigned long>(now - gStatus.gpsLastLocationUpdateMs)
                : 0UL);
        char nmea[128];
        if (consumeGpsNmeaLine(nmea, sizeof(nmea))) Serial.printf("GPS NMEA: %s\n", nmea);
    }

    delay(1);
}




// #include <Arduino.h>

// void updateSerial();
// void test_sim800_module();
// void send_SMS();

// void setup() {
//   Serial.begin(115200);
//   Serial2.begin(9600, SERIAL_8N1, 16, 17); // RX, TX
//   Serial.println("SIM800L Test");
//   delay(3000);
//   test_sim800_module();
//   send_SMS();
// }

// void loop() {
//   updateSerial();
// }


// void test_sim800_module()
// {
//   Serial2.println("AT");
//   updateSerial();
//   Serial.println();
//   Serial2.println("AT+CSQ");
//   updateSerial();
//   Serial2.println("AT+CCID");
//   updateSerial();
//   Serial2.println("AT+CREG?");
//   updateSerial();
//   Serial2.println("ATI");
//   updateSerial();
//   Serial2.println("AT+CBC");
//   updateSerial();
// }


// void updateSerial()
// {
//   delay(500);
//   while (Serial.available())
//   {
//     Serial2.write(Serial.read());//Forward what Serial received to Software Serial Port
//   }
//   while (Serial2.available())
//   {
//     Serial.write(Serial2.read());//Forward what Software Serial received to Serial Port
//   }
// }


// void send_SMS()
// {
//   Serial2.println("AT+CMGF=1"); // Configuring TEXT mode
//   updateSerial();
//   Serial2.println("AT+CMGS=\"+919804049270\"");//change ZZ with country code and xxxxxxxxxxx with phone number to sms
//   updateSerial();
//   Serial2.print("Circuit Digest"); //text content
//   updateSerial();
// Serial.println();
//   Serial.println("Message Sent");
//   Serial2.write(26);
// }


