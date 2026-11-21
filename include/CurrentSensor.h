#pragma once
#include "Types.h"

void beginCurrentSensor();
void updateCurrentSensor(OperatingMode mode, SystemStatus &status);
float getPumpCurrentA();
bool calibrateCurrentZero(OperatingMode mode); // Starts nonblocking calibration; call with pump OFF.
