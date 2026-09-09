#pragma once

#include <sl.h>

#include "Config.h"
#include "State.h"

namespace ReflexProviderDiag
{
struct LowLatencyDecisionSnapshot
{
    VendorId::Value vendorId;
    LowLatencyMode configuredMode;
    LowLatencyMode requestedMode;
    LowLatencyMode vendorMode;
    FGOutput fgOutput;
    bool xefgForce;
    LowLatencyMode finalMode;
    LowLatencyInput activeInput;
    LowLatencyMode activeOutput;
    bool techPresent;
    LowLatencyMode techMode;
    bool devicePresent;
    bool explicitMode;
    LowLatencyMode explicitModeValue;

    bool operator==(const LowLatencyDecisionSnapshot&) const = default;
};

bool IsEnabled();
void LogStreamlineOnce(const char* api, void* returnAddress, sl::Result result, const char* detail = nullptr);
void LogLowLatencyDecision(const LowLatencyDecisionSnapshot& snapshot);
} // namespace ReflexProviderDiag
