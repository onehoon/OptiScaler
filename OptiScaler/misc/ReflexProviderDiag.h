#pragma once

#include <cstdint>

#include <sl.h>

#include "Config.h"
#include "State.h"

namespace ReflexProviderDiag
{
enum class LowLatencyRecordKind : uint8_t
{
    Decision,
    NullDevice,
    ModeAlreadyActive,
    InputChangeExistingTech,
    Initialized,
};

struct LowLatencyCaptureKey
{
    LowLatencyRecordKind kind;
    VendorId::Value vendorId;
    LowLatencyMode configuredMode;
    LowLatencyMode requestedMode;
    LowLatencyMode vendorMode;
    FGOutput fgOutput;
    bool xefgForce;
    LowLatencyMode finalMode;
    LowLatencyInput activeInput;
    LowLatencyMode activeOutput;
    bool devicePresent;
    bool explicitMode;
    LowLatencyMode explicitModeValue;

    bool operator==(const LowLatencyCaptureKey&) const = default;
};

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
bool ShouldCaptureLowLatency(const LowLatencyCaptureKey& key);
void LogStreamlineOnce(const char* api, void* returnAddress, sl::Result result, const char* detail = nullptr);
void LogLowLatencyDecision(const LowLatencyDecisionSnapshot& snapshot);
void LogLowLatencyEarlyExit(const char* reason, const LowLatencyCaptureKey& key);
void LogLowLatencyInputTransition(const LowLatencyCaptureKey& key, LowLatencyMode techMode);
void LogLowLatencyInitialized(const LowLatencyCaptureKey& key, LowLatencyMode techMode);
} // namespace ReflexProviderDiag
