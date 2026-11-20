#pragma once
#include "Types.h"

OperatingMode detectOperatingMode();
void beginLevelSensors(OperatingMode mode);
void updateLevelSensors(OperatingMode mode, WaterData &data);
void beginTemperatureSensor();
void updateTemperatureSensor(SystemStatus &status);
