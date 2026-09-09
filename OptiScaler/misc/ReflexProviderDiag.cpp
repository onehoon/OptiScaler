#include <pch.h>

#include "ReflexProviderDiag.h"

#include <magic_enum.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ReflexProviderDiag
{
namespace
{
std::atomic_uint64_t sequence = 0;

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

struct LowLatencyDecisionHash
{
    size_t operator()(const LowLatencyDecisionSnapshot& snapshot) const noexcept
    {
        size_t hash = 0;
        const auto combine = [&hash](uint32_t value)
        { hash ^= std::hash<uint32_t> {}(value) + static_cast<size_t>(0x9E3779B9) + (hash << 6) + (hash >> 2); };

        combine(static_cast<uint32_t>(snapshot.vendorId));
        combine(static_cast<uint32_t>(snapshot.configuredMode));
        combine(static_cast<uint32_t>(snapshot.requestedMode));
        combine(static_cast<uint32_t>(snapshot.vendorMode));
        combine(static_cast<uint32_t>(snapshot.fgOutput));
        combine(snapshot.xefgForce ? 1u : 0u);
        combine(static_cast<uint32_t>(snapshot.finalMode));
        combine(static_cast<uint32_t>(snapshot.activeInput));
        combine(static_cast<uint32_t>(snapshot.activeOutput));
        combine(snapshot.techPresent ? 1u : 0u);
        combine(static_cast<uint32_t>(snapshot.techMode));
        combine(snapshot.devicePresent ? 1u : 0u);
        combine(snapshot.explicitMode ? 1u : 0u);
        combine(static_cast<uint32_t>(snapshot.explicitModeValue));
        return hash;
    }
};

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

bool IsEnabled() { return IsSupportedExecutable(State::Instance().gameExe); }

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
    if (!IsEnabled())
        return;

    static std::mutex mutex;
    static std::unordered_set<LowLatencyDecisionSnapshot, LowLatencyDecisionHash> seen;
    {
        std::scoped_lock lock(mutex);
        if (seen.find(snapshot) != seen.end() || seen.size() >= 16)
            return;
        seen.insert(snapshot);
    }

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
} // namespace ReflexProviderDiag
