#include <pch.h>

#include "ReflexProviderDiag.h"

#include <magic_enum.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ReflexProviderDiag
{
namespace
{
constexpr size_t PrimaryCaptureLimit = 16;
constexpr size_t AuxiliaryCaptureLimit = 8;

std::atomic_uint64_t sequence = 0;
std::atomic_uint32_t observedVendor { static_cast<uint32_t>(VendorId::Invalid) };

struct StreamlineKey
{
    uintptr_t returnAddress = 0;
    std::string api;
    std::string detail;

    bool operator==(const StreamlineKey&) const = default;
};

struct StreamlineKeyHash
{
    size_t operator()(const StreamlineKey& key) const noexcept
    {
        const auto addressHash = std::hash<uintptr_t> {}(key.returnAddress);
        const auto apiHash = std::hash<std::string> {}(key.api);
        const auto detailHash = std::hash<std::string> {}(key.detail);
        return addressHash ^ (apiHash << 1) ^ (detailHash << 2);
    }
};

struct LowLatencyCaptureHash
{
    size_t operator()(const LowLatencyCaptureKey& key) const noexcept
    {
        size_t hash = 0;
        const auto combine = [&hash](uint32_t value)
        { hash ^= std::hash<uint32_t> {}(value) + static_cast<size_t>(0x9E3779B9) + (hash << 6) + (hash >> 2); };

        combine(static_cast<uint32_t>(key.kind));
        combine(static_cast<uint32_t>(key.vendorId));
        combine(static_cast<uint32_t>(key.configuredMode));
        combine(static_cast<uint32_t>(key.requestedMode));
        combine(static_cast<uint32_t>(key.vendorMode));
        combine(static_cast<uint32_t>(key.fgOutput));
        combine(key.xefgForce ? 1u : 0u);
        combine(static_cast<uint32_t>(key.finalMode));
        combine(static_cast<uint32_t>(key.activeInput));
        combine(static_cast<uint32_t>(key.activeOutput));
        combine(key.devicePresent ? 1u : 0u);
        combine(key.explicitMode ? 1u : 0u);
        combine(static_cast<uint32_t>(key.explicitModeValue));
        return hash;
    }
};

struct CaptureBucket
{
    std::atomic_bool saturated = false;
    std::mutex mutex;
    std::unordered_set<LowLatencyCaptureKey, LowLatencyCaptureHash> seen;
};

CaptureBucket primaryBucket;
CaptureBucket auxiliaryBucket;

struct CallsiteInfo
{
    std::string module = "unknown";
    uintptr_t rva = 0;
};

bool IsSupportedExecutable(const std::string& executable)
{
    return _stricmp(executable.c_str(), "re9.exe") == 0 || _stricmp(executable.c_str(), "pragmata.exe") == 0 ||
           _stricmp(executable.c_str(), "dd2.exe") == 0 || _stricmp(executable.c_str(), "dd2ccs.exe") == 0 ||
           _stricmp(executable.c_str(), "monsterhunterwilds.exe") == 0;
}

bool IsPrimaryKind(LowLatencyRecordKind kind)
{
    return kind == LowLatencyRecordKind::Decision || kind == LowLatencyRecordKind::Initialized;
}

bool TryCapture(CaptureBucket& bucket, LowLatencyCaptureKey& key, std::optional<LowLatencyCaptureKey>& lastKey,
                size_t limit)
{
    if (lastKey.has_value() && *lastKey == key)
        return false;
    lastKey = key;

    std::scoped_lock lock(bucket.mutex);
    if (bucket.seen.size() >= limit)
    {
        bucket.saturated.store(true, std::memory_order_relaxed);
        return false;
    }

    if (!bucket.seen.insert(key).second)
        return false;

    if (bucket.seen.size() >= limit)
        bucket.saturated.store(true, std::memory_order_relaxed);

    return true;
}

CallsiteInfo ResolveCallsite(void* returnAddress)
{
    CallsiteInfo info;
    if (returnAddress == nullptr)
        return info;

    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(returnAddress), &module) == 0 ||
        module == nullptr)
        return info;

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(module, path, sizeof(path)) != 0)
    {
        const auto* filename = std::strrchr(path, '\\');
        info.module = filename != nullptr ? filename + 1 : path;
    }

    info.rva = reinterpret_cast<uintptr_t>(returnAddress) - reinterpret_cast<uintptr_t>(module);
    return info;
}

uint64_t NextSequence() { return sequence.fetch_add(1, std::memory_order_relaxed) + 1; }
} // namespace

bool IsEnabled()
{
    static std::atomic<int8_t> cachedScope { -1 };

    const auto cached = cachedScope.load(std::memory_order_relaxed);
    if (cached >= 0)
        return cached == 1;

    const auto& executable = State::Instance().gameExe;
    if (executable.empty())
        return false;

    const bool enabled = IsSupportedExecutable(executable);
    cachedScope.store(enabled ? 1 : 0, std::memory_order_relaxed);
    return enabled;
}

bool ShouldCaptureLowLatency(LowLatencyCaptureKey& key)
{
    const bool primary = IsPrimaryKind(key.kind);
    auto& bucket = primary ? primaryBucket : auxiliaryBucket;
    const size_t limit = primary ? PrimaryCaptureLimit : AuxiliaryCaptureLimit;

    // Saturation is checked before scope/vendor work so a finished bucket becomes essentially inert.
    if (bucket.saturated.load(std::memory_order_relaxed))
        return false;

    if (!IsEnabled())
        return false;

    if (key.vendorId == VendorId::Invalid)
        key.vendorId = GetObservedVendor();

    thread_local std::optional<LowLatencyCaptureKey> lastPrimaryKey;
    thread_local std::optional<LowLatencyCaptureKey> lastAuxiliaryKey;
    auto& lastKey = primary ? lastPrimaryKey : lastAuxiliaryKey;

    return TryCapture(bucket, key, lastKey, limit);
}

void ObserveVendor(VendorId::Value vendorId)
{
    if (vendorId == VendorId::Invalid || !IsEnabled())
        return;

    observedVendor.store(static_cast<uint32_t>(vendorId), std::memory_order_relaxed);
}

VendorId::Value GetObservedVendor()
{
    return static_cast<VendorId::Value>(observedVendor.load(std::memory_order_relaxed));
}

void LogStreamlineOnce(const char* api, void* returnAddress, sl::Result result, const char* detail)
{
    if (!IsEnabled() || api == nullptr)
        return;

    const std::string_view detailView = detail != nullptr ? detail : "";
    static std::mutex mutex;
    static std::unordered_set<StreamlineKey, StreamlineKeyHash> seen;

    const StreamlineKey key { reinterpret_cast<uintptr_t>(returnAddress), std::string(api), std::string(detailView) };
    {
        std::scoped_lock lock(mutex);
        if (seen.find(key) != seen.end() || seen.size() >= 32)
            return;
        seen.insert(key);
    }

    const auto callsite = ResolveCallsite(returnAddress);
    const auto resultName = magic_enum::enum_name(result);
    const auto seq = NextSequence();
    if (detailView.empty())
    {
        LOG_INFO("[ReflexProviderGate] seq={} area=SL api={} result={} caller={} rva=0x{:X}", seq, api, resultName,
                 callsite.module, callsite.rva);
    }
    else
    {
        LOG_INFO("[ReflexProviderGate] seq={} area=SL api={} detail={} result={} caller={} rva=0x{:X}", seq, api,
                 detailView, resultName, callsite.module, callsite.rva);
    }
}

void LogLowLatencyDecision(const LowLatencyDecisionSnapshot& snapshot)
{
    const auto seq = NextSequence();
    LOG_INFO("[ReflexProviderGate] seq={} area=LL phase=decision vendor={} configured={} requested={} vendorMode={} "
             "fgOutput={} xefgForce={} finalMode={} activeInput={} activeOutput={} techPresent={} techMode={} "
             "devicePresent={} explicitMode={} explicitValue={}",
             seq, magic_enum::enum_name(snapshot.vendorId), magic_enum::enum_name(snapshot.configuredMode),
             magic_enum::enum_name(snapshot.requestedMode), magic_enum::enum_name(snapshot.vendorMode),
             magic_enum::enum_name(snapshot.fgOutput), snapshot.xefgForce, magic_enum::enum_name(snapshot.finalMode),
             magic_enum::enum_name(snapshot.activeInput), magic_enum::enum_name(snapshot.activeOutput),
             snapshot.techPresent, magic_enum::enum_name(snapshot.techMode), snapshot.devicePresent,
             snapshot.explicitMode, magic_enum::enum_name(snapshot.explicitModeValue));
}

void LogLowLatencyEarlyExit(const char* reason, const LowLatencyCaptureKey& key)
{
    const auto seq = NextSequence();
    LOG_INFO("[ReflexProviderGate] seq={} area=LL phase=early-exit reason={} vendor={} configured={} requested={} "
             "vendorMode={} fgOutput={} xefgForce={} finalMode={} activeInput={} activeOutput={} "
             "devicePresent={} explicitMode={} explicitValue={} techMode=not-captured",
             seq, reason, magic_enum::enum_name(key.vendorId), magic_enum::enum_name(key.configuredMode),
             magic_enum::enum_name(key.requestedMode), magic_enum::enum_name(key.vendorMode),
             magic_enum::enum_name(key.fgOutput), key.xefgForce, magic_enum::enum_name(key.finalMode),
             magic_enum::enum_name(key.activeInput), magic_enum::enum_name(key.activeOutput), key.devicePresent,
             key.explicitMode, magic_enum::enum_name(key.explicitModeValue));
}

void LogLowLatencyInputTransition(const LowLatencyCaptureKey& key, LowLatencyMode techMode)
{
    const auto seq = NextSequence();
    LOG_INFO("[ReflexProviderGate] seq={} area=LL phase=input-change-existing-tech vendor={} configured={} "
             "requested={} activeInput={} activeOutput={} techPresent=true techMode={} devicePresent={} "
             "explicitMode={} explicitValue={}",
             seq, magic_enum::enum_name(key.vendorId), magic_enum::enum_name(key.configuredMode),
             magic_enum::enum_name(key.requestedMode), magic_enum::enum_name(key.activeInput),
             magic_enum::enum_name(key.activeOutput), magic_enum::enum_name(techMode), key.devicePresent,
             key.explicitMode, magic_enum::enum_name(key.explicitModeValue));
}

void LogLowLatencyInitialized(const LowLatencyCaptureKey& key, LowLatencyMode techMode)
{
    const auto seq = NextSequence();
    LOG_INFO("[ReflexProviderGate] seq={} area=LL phase=initialized vendor={} mode={} activeInput={} activeOutput={} "
             "techPresent=true techMode={} devicePresent={}",
             seq, magic_enum::enum_name(key.vendorId), magic_enum::enum_name(techMode),
             magic_enum::enum_name(key.activeInput), magic_enum::enum_name(key.activeOutput),
             magic_enum::enum_name(techMode), key.devicePresent);
}
} // namespace ReflexProviderDiag
