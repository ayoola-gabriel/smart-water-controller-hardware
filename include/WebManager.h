#pragma once
#include "Types.h"

void beginWebManager();
void handleWebManager();
void requestWifiCredentials();
void publishWebStatus(const WaterData &water, const SystemStatus &status);
PumpMode getWebPumpMode();
bool getWebManualPumpRequest();
bool consumeWebClearFaultRequest();
