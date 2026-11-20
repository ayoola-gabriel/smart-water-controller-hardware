#pragma once
#include "Types.h"

void beginOutputs();
void setPumpOutput(bool on);
void updateIndicators(const SystemStatus &status, const WaterData &data);
void updateBuzzer(bool faultActive);
