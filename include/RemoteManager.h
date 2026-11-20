#pragma once
#include "Types.h"

void beginRemoteManager();
void updateRemoteManager(const WaterData &water, SystemStatus &status);
PumpMode getBlePumpMode();
bool getBleManualPumpRequest();
bool isBleControlActive();
bool consumeBleClearFaultRequest();
PumpMode getCloudPumpMode();
bool getCloudManualPumpRequest();
bool isCloudControlActive();
