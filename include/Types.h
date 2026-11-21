#pragma once
#include <Arduino.h>

enum OperatingMode : uint8_t { PROTOTYPE_MODE, REAL_MODE };
enum PumpMode : uint8_t { AUTO_MODE, MANUAL_MODE, PUMP_OFF_MODE };
enum PumpState : uint8_t { PUMP_STOPPED, PUMP_RUNNING, PUMP_FAULT };
enum FaultType : uint8_t {
    NO_FAULT,
    LEVEL_SENSOR_FAULT,
    CURRENT_SENSOR_FAULT,
    LOW_CURRENT_FAULT,
    OVERCURRENT_FAULT,
    MAX_RUNTIME_FAULT
};

struct WaterData {
    float levelPercent = 0.0f; // Estimated only; real mode has two-point level info.
    bool levelValid = false;
    bool lowLevel = false;
    bool midLevel = false;
    bool highLevel = false;
    bool fullLevel = false;
};

struct SystemStatus {
    OperatingMode operatingMode = PROTOTYPE_MODE;
    PumpMode pumpMode = AUTO_MODE;
    PumpState pumpState = PUMP_STOPPED;
    FaultType fault = NO_FAULT;
    bool pumpCommand = false;
    bool wifiConnected = false;
    bool gsmReady = false;
    float pumpCurrentA = 0.0f;
    bool temperatureValid = false;
    float temperatureC = 0.0f;
    float temperatureNodeV = 0.0f;
    float thermistorResistanceOhms = 0.0f;
    uint16_t temperatureRawAdc = 0;
    bool gpsValid = false;
    double gpsLatitude = 0.0;
    double gpsLongitude = 0.0;
    uint32_t gpsSatellites = 0;
    uint32_t gpsCharactersReceived = 0;
    uint32_t gpsValidSentences = 0;
    uint32_t gpsFailedSentences = 0;
    uint32_t gpsLastLocationUpdateMs = 0;
};
