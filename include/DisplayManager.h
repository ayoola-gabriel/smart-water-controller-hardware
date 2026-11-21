#pragma once

#include <stdint.h>
#include "Types.h"

void beginDisplay();
void renderDisplay(const WaterData &water, const SystemStatus &status,
                   OperatingMode mode, bool pairingPending, uint32_t passkey);
