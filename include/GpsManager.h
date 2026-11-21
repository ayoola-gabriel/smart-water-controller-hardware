#pragma once
#include <stddef.h>
#include "Types.h"

void beginGps();
void updateGps(SystemStatus &status);
bool consumeGpsNmeaLine(char *buffer, size_t bufferSize);
