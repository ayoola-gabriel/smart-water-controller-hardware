#pragma once
#include "Types.h"

void beginCurrentSensor();
void updateCurrentSensor(OperatingMode mode, SystemStatus &status, bool pumpOff);
float getPumpCurrentA();
bool calibrateCurrentZero(OperatingMode mode); // Starts nonblocking calibration; call with pump OFF.
