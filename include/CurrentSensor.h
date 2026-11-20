#pragma once
#include "Types.h"

void beginCurrentSensor();
void updateCurrentSensor(OperatingMode mode);
float getPumpCurrentA();
bool calibrateCurrentZero(OperatingMode mode); // Prototype shunt only; call with pump OFF.
