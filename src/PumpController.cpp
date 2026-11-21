#include <Arduino.h>
#include "AppConfig.h"
#include "Outputs.h"
#include "PumpController.h"

static uint32_t pumpStartMs = 0;
static uint32_t lowCurrentSinceMs = 0;
static uint32_t overCurrentSinceMs = 0;
static uint32_t faultLatchedAtMs = 0;
static uint32_t lastRecoveryCheckMs = 0;
static bool lastCommand = false;

static void latchFault(FaultType fault, SystemStatus &status) {
    if (status.fault == NO_FAULT) {
        status.fault = fault;
        faultLatchedAtMs = millis();
        lastRecoveryCheckMs = faultLatchedAtMs;
    }
}

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
    faultLatchedAtMs = 0;
    lastRecoveryCheckMs = 0;
    status.pumpState = PUMP_STOPPED;
    commandPump(false, status);
}

static bool runtimeDemandCleared(const WaterData &water, PumpMode mode,
                                bool manualPumpRequest) {
    if (mode == PUMP_OFF_MODE) return true;
    if (mode == MANUAL_MODE) return !manualPumpRequest;
    return water.levelValid && (water.fullLevel || water.highLevel);
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
        latchFault(LEVEL_SENSOR_FAULT, status);
        commandPump(false, status);
        status.pumpState = PUMP_FAULT;
        return;
    }

    if (status.fault != NO_FAULT) {
        commandPump(false, status);
        status.pumpState = PUMP_FAULT;
        const uint32_t now = millis();
        const bool canAutoRecover = status.fault == MAX_RUNTIME_FAULT &&
                                    AUTO_RECOVER_MAX_RUNTIME_FAULT;
        if (canAutoRecover && faultLatchedAtMs != 0 &&
            (uint32_t)(now - faultLatchedAtMs) >= FAULT_AUTO_RECOVERY_DELAY_MS) {
            if (runtimeDemandCleared(water, requestedMode, manualPumpRequest)) {
                Serial.println("MAX RUNTIME fault auto-cleared after demand ended");
                clearLatchedFault(status);
            } else if ((uint32_t)(now - lastRecoveryCheckMs) >= FAULT_AUTO_RECOVERY_DELAY_MS) {
                lastRecoveryCheckMs = now;
                Serial.println("MAX RUNTIME fault persists; pump remains OFF");
                return;
            } else {
                return;
            }
        } else {
            return;
        }
    }

    if (status.pumpCommand &&
        pumpStartMs != 0 &&
        millis() - pumpStartMs >= MAX_PUMP_RUNTIME_MS) {
                latchFault(MAX_RUNTIME_FAULT, status);
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
                latchFault(OVERCURRENT_FAULT, status);
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
                latchFault(LOW_CURRENT_FAULT, status);
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
