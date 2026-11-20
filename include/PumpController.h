#pragma once
#include "Types.h"

void updatePumpController(
    OperatingMode mode,
    const WaterData &water,
    SystemStatus &status,
    PumpMode requestedMode,
    bool manualPumpRequest,
    float currentA
);
void clearLatchedFault(SystemStatus &status);
