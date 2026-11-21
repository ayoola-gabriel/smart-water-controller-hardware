#include <Arduino.h>
#include "AppConfig.h"
#include "Outputs.h"
#include "PumpController.h"

static uint32_t pumpStartMs = 0;
static uint32_t lowCurrentSinceMs = 0;
static uint32_t overCurrentSinceMs = 0;
static bool lastCommand = false;

static void commandPump(bool on, SystemStatus &status) {
    status.pumpCommand = on;
    setPumpOutput(on);

    if (on && !lastCommand) pumpStartMs = millis();
    if (!on) {
        pumpStartMs = 0;
        lowCurrentSinceMs = 0;
        overCurrentSinceMs = 0;
    }

    status.pumpState = on ? PUMP_RUNNING : PUMP_STOPPED;
    lastCommand = on;
}

void clearLatchedFault(SystemStatus &status) {
    status.fault = NO_FAULT;
    status.pumpState = PUMP_STOPPED;
    commandPump(false, status);
}

void updatePumpController(
    OperatingMode mode,
    const WaterData &water,
    SystemStatus &status,
    PumpMode requestedMode,
    bool manualPumpRequest,
    float currentA
) {
    status.pumpMode = requestedMode;
    status.pumpCurrentA = currentA;

    if (!water.levelValid) {
        status.fault = LEVEL_SENSOR_FAULT;
        commandPump(false, status);
        status.pumpState = PUMP_FAULT;
        return;
    }

    if (status.fault != NO_FAULT) {
        commandPump(false, status);
        status.pumpState = PUMP_FAULT;
        return;
    }

    if (status.pumpCommand &&
        pumpStartMs != 0 &&
        millis() - pumpStartMs >= MAX_PUMP_RUNTIME_MS) {
        status.fault = MAX_RUNTIME_FAULT;
        commandPump(false, status);
        status.pumpState = PUMP_FAULT;
        return;
    }

    if (status.pumpCommand &&
        pumpStartMs != 0 &&
        millis() - pumpStartMs >= PUMP_STARTUP_GRACE_MS) {

        const float maxCurrent = mode == PROTOTYPE_MODE
            ? PROTOTYPE_OVERCURRENT_A : REAL_OVERCURRENT_A;

        if (ENABLE_OVERCURRENT_PROTECTION && currentA > maxCurrent) {
            if (overCurrentSinceMs == 0) overCurrentSinceMs = millis();
            if (millis() - overCurrentSinceMs >= OVERCURRENT_TRIP_DELAY_MS) {
                status.fault = OVERCURRENT_FAULT;
                commandPump(false, status);
                status.pumpState = PUMP_FAULT;
                return;
            }
        } else {
            overCurrentSinceMs = 0;
        }

        const float minCurrent = mode == PROTOTYPE_MODE
            ? PROTOTYPE_MIN_RUNNING_CURRENT_A : REAL_MIN_RUNNING_CURRENT_A;

        if (ENABLE_LOW_CURRENT_FAULT && currentA < minCurrent) {
            if (lowCurrentSinceMs == 0) lowCurrentSinceMs = millis();
            if (millis() - lowCurrentSinceMs >= LOW_CURRENT_TRIP_DELAY_MS) {
                status.fault = LOW_CURRENT_FAULT;
                commandPump(false, status);
                status.pumpState = PUMP_FAULT;
                return;
            }
        } else {
            lowCurrentSinceMs = 0;
        }
    }

    if (requestedMode == PUMP_OFF_MODE) {
        commandPump(false, status);
        return;
    }

    if (requestedMode == MANUAL_MODE) {
        // Manual ON is still subject to level validity, runtime and enabled
        // electrical protection. High-level/overflow stop is never bypassed.
        if (manualPumpRequest && !water.highLevel && !water.fullLevel) {
            commandPump(true, status);
        } else {
            commandPump(false, status);
        }
        return;
    }

    // Automatic mode uses mode-specific threshold semantics.
    if (mode == PROTOTYPE_MODE) {
        // Electrode booleans mean WET. Start when the lowest probe is DRY;
        // stop when the HIGH or FULL probe becomes WET.
        if (water.highLevel || water.fullLevel) {
            commandPump(false, status);
        } else if (!water.lowLevel) {
            commandPump(true, status);
        }
    } else {
        // Real-mode float switches use the same monotonic wet-level states as
        // the prototype electrodes: below LOW starts refill, LOW/MID retain
        // the refill latch, and FULL stops it.
        if (water.highLevel) {
            commandPump(false, status);
        } else if (!water.lowLevel) {
            commandPump(true, status);
        }
    }
    // Between thresholds: retain previous pump state.
}
