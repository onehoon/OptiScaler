#include "pch.h"
#include "Dxgi_Spoofing.h"

#include <Config.h>
#include <State.h>
#include <misc/Quirks.h>

#include <detours/detours.h>

#include <string>
#include <algorithm>
#include <cctype>

typedef HRESULT (*PFN_GetDesc)(IDXGIAdapter* This, DXGI_ADAPTER_DESC* pDesc);
typedef HRESULT (*PFN_GetDesc1)(IDXGIAdapter1* This, DXGI_ADAPTER_DESC1* pDesc);
typedef HRESULT (*PFN_GetDesc2)(IDXGIAdapter2* This, DXGI_ADAPTER_DESC2* pDesc);
typedef HRESULT (*PFN_GetDesc3)(IDXGIAdapter4* This, DXGI_ADAPTER_DESC3* pDesc);

inline static PFN_GetDesc o_GetDesc = nullptr;
inline static PFN_GetDesc1 o_GetDesc1 = nullptr;
inline static PFN_GetDesc2 o_GetDesc2 = nullptr;
inline static PFN_GetDesc3 o_GetDesc3 = nullptr;

inline static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline static bool iequals(const std::string& a, const std::string& b) { return toLower(a) == toLower(b); }

#pragma region DXGI Adapter methods

inline static bool SkipSpoofing()
{
    auto skip = !Config::Instance()->DxgiSpoofing.value_or_default() || State::Instance().skipSpoofing;

    if (skip)
    {
        LOG_TRACE("DxgiSpoofing: {}, skipSpoofing: {}, skipping spoofing",
                  Config::Instance()->DxgiSpoofing.value_or_default(), State::Instance().skipSpoofing);
    }

    return skip;
}

inline static bool IsIntelReflexDxgiQuirkConfigured()
{
    const auto* config = Config::Instance();
    const auto& state = State::Instance();

    if (!(state.gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel))
        return false;

    // The release/0.9 local SkipSpoofing() also treats DxgiSpoofing=false as
    // a skip. This quirk intentionally runs with broad DXGI spoofing disabled.
    if (!config->StreamlineSpoofing.value_or_default() || config->DxgiSpoofing.value_or_default() || state.skipSpoofing)
    {
        return false;
    }

    return true;
}

inline static bool IsIntelReflexDxgiQuirkEligible(uint32_t vendorId)
{
    return IsIntelReflexDxgiQuirkConfigured() && vendorId == VendorId::Intel;
}

template <typename T> inline static bool ShouldApplyIntelReflexGameIdentity(const std::string& caller, const T* desc)
{
    if (!IsIntelReflexDxgiQuirkEligible(desc->VendorId) || !iequals(caller, State::Instance().GameExe))
        return false;

    const auto* config = Config::Instance();
    const bool targetVendorIdMatches =
        !config->TargetVendorId.has_value() || config->TargetVendorId.value() == desc->VendorId;
    const bool targetDeviceIdMatches =
        !config->TargetDeviceId.has_value() || config->TargetDeviceId.value() == desc->DeviceId;

    return desc->VendorId != VendorId::Microsoft && targetVendorIdMatches && targetDeviceIdMatches;
}

template <typename T> inline static void ApplyIntelReflexGameIdentity(T* desc)
{
    const auto* config = Config::Instance();

    desc->VendorId = config->SpoofedVendorId.value_or_default();
    desc->DeviceId = config->SpoofedDeviceId.value_or_default();

    const auto spoofedName = config->SpoofedGPUName.value_or_default();
    std::memset(desc->Description, 0, sizeof(desc->Description));
    std::wcscpy(desc->Description, spoofedName.c_str());
}

HRESULT DxgiSpoofing::hkGetDesc3(IDXGIAdapter4* This, DXGI_ADAPTER_DESC3* pDesc)
{
    auto result = o_GetDesc3(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "fakenvapi.dll") || iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") ||
        iequals(caller, "dxgi.dll") || iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (pDesc->VendorId != VendorId::Microsoft &&
            !State::Instance().adapterDescs.contains(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart))
        {
            std::wstring szName(pDesc->Description);
            std::string descStr =
                std::format("Adapter: {}, VRAM: {} MB, VendorId: {:#x}, DeviceId: {:#x}", wstring_to_string(szName),
                            pDesc->DedicatedVideoMemory / (1024.0 * 1024.0), pDesc->VendorId, pDesc->DeviceId);
            LOG_INFO("{}", descStr);
            State::Instance().adapterDescs.insert_or_assign(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart,
                                                            descStr);
        }

        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        if (pDesc->VendorId != VendorId::Microsoft &&
            (!Config::Instance()->TargetVendorId.has_value() ||
             Config::Instance()->TargetVendorId.value() == pDesc->VendorId) &&
            (!Config::Instance()->TargetDeviceId.has_value() ||
             Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId) &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }

        if (ShouldApplyIntelReflexGameIdentity(caller, pDesc))
            ApplyIntelReflexGameIdentity(pDesc);
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc2(IDXGIAdapter2* This, DXGI_ADAPTER_DESC2* pDesc)
{
    auto result = o_GetDesc2(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "fakenvapi.dll") || iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") ||
        iequals(caller, "dxgi.dll") || iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (pDesc->VendorId != VendorId::Microsoft &&
            !State::Instance().adapterDescs.contains(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart))
        {
            std::wstring szName(pDesc->Description);
            std::string descStr =
                std::format("Adapter: {}, VRAM: {} MB, VendorId: {:#x}, DeviceId: {:#x}", wstring_to_string(szName),
                            pDesc->DedicatedVideoMemory / (1024.0 * 1024.0), pDesc->VendorId, pDesc->DeviceId);
            LOG_INFO("{}", descStr);
            State::Instance().adapterDescs.insert_or_assign(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart,
                                                            descStr);
        }

        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        if (pDesc->VendorId != VendorId::Microsoft &&
            (!Config::Instance()->TargetVendorId.has_value() ||
             Config::Instance()->TargetVendorId.value() == pDesc->VendorId) &&
            (!Config::Instance()->TargetDeviceId.has_value() ||
             Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId) &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }

        if (ShouldApplyIntelReflexGameIdentity(caller, pDesc))
            ApplyIntelReflexGameIdentity(pDesc);
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc1(IDXGIAdapter1* This, DXGI_ADAPTER_DESC1* pDesc)
{
    auto result = o_GetDesc1(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "fakenvapi.dll") || iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") ||
        iequals(caller, "dxgi.dll") || iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (pDesc->VendorId != VendorId::Microsoft &&
            !State::Instance().adapterDescs.contains(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart))
        {
            std::wstring szName(pDesc->Description);
            std::string descStr =
                std::format("Adapter: {}, VRAM: {} MB, VendorId: {:#x}, DeviceId: {:#x}", wstring_to_string(szName),
                            pDesc->DedicatedVideoMemory / (1024.0 * 1024.0), pDesc->VendorId, pDesc->DeviceId);
            LOG_INFO("{}", descStr);
            State::Instance().adapterDescs.insert_or_assign(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart,
                                                            descStr);
        }

        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        if (pDesc->VendorId != VendorId::Microsoft &&
            (!Config::Instance()->TargetVendorId.has_value() ||
             Config::Instance()->TargetVendorId.value() == pDesc->VendorId) &&
            (!Config::Instance()->TargetDeviceId.has_value() ||
             Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId) &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }

        if (ShouldApplyIntelReflexGameIdentity(caller, pDesc))
            ApplyIntelReflexGameIdentity(pDesc);
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc(IDXGIAdapter* This, DXGI_ADAPTER_DESC* pDesc)
{
    auto result = o_GetDesc(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "fakenvapi.dll") || iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") ||
        iequals(caller, "dxgi.dll") || iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (pDesc->VendorId != VendorId::Microsoft &&
            !State::Instance().adapterDescs.contains(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart))
        {
            std::wstring szName(pDesc->Description);
            std::string descStr =
                std::format("Adapter: {}, VRAM: {} MB, VendorId: {:#x}, DeviceId: {:#x}", wstring_to_string(szName),
                            pDesc->DedicatedVideoMemory / (1024.0 * 1024.0), pDesc->VendorId, pDesc->DeviceId);
            LOG_INFO("{}", descStr);
            State::Instance().adapterDescs.insert_or_assign(pDesc->AdapterLuid.HighPart | pDesc->AdapterLuid.LowPart,
                                                            descStr);
        }

        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        if (pDesc->VendorId != VendorId::Microsoft &&
            (!Config::Instance()->TargetVendorId.has_value() ||
             Config::Instance()->TargetVendorId.value() == pDesc->VendorId) &&
            (!Config::Instance()->TargetDeviceId.has_value() ||
             Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId) &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }

        if (ShouldApplyIntelReflexGameIdentity(caller, pDesc))
            ApplyIntelReflexGameIdentity(pDesc);
    }

    AttachToAdapter(This);

    return result;
}

#pragma endregion

#pragma region DXGI Attach methods

void DxgiSpoofing::AttachToAdapter(IUnknown* unkAdapter)
{
    static bool logAdded = false;
    const bool normalDxgiHooksNeeded =
        Config::Instance()->DxgiSpoofing.value_or_default() || Config::Instance()->DxgiVRAM.has_value();
    const bool intelReflexQuirkNeeded = IsIntelReflexDxgiQuirkConfigured();

    if (!normalDxgiHooksNeeded && !intelReflexQuirkNeeded)
    {
        if (!logAdded)
        {
            LOG_WARN("DxgiSpoofing and DxgiVRAM is disabled, skipping hooking");
            logAdded = true;
        }

        return;
    }

    if (o_GetDesc != nullptr && o_GetDesc1 != nullptr && o_GetDesc2 != nullptr && o_GetDesc3 != nullptr)
        return;

    PVOID* pVTable = *(PVOID**) unkAdapter;

    IDXGIAdapter* adapter = nullptr;
    bool adapterOk = unkAdapter->QueryInterface(__uuidof(IDXGIAdapter), (void**) &adapter) == S_OK;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_GetDesc == nullptr && adapterOk)
    {
        LOG_DEBUG("Attach to GetDesc");
        o_GetDesc = (PFN_GetDesc) pVTable[8];
        DetourAttach(&(PVOID&) o_GetDesc, hkGetDesc);
    }

    if (adapter != nullptr)
        adapter->Release();

    IDXGIAdapter1* adapter1 = nullptr;
    if (o_GetDesc1 == nullptr && unkAdapter->QueryInterface(__uuidof(IDXGIAdapter1), (void**) &adapter1) == S_OK)
    {
        LOG_DEBUG("Attach to GetDesc1");
        o_GetDesc1 = (PFN_GetDesc1) pVTable[10];
        DetourAttach(&(PVOID&) o_GetDesc1, hkGetDesc1);
    }

    if (adapter1 != nullptr)
        adapter1->Release();

    IDXGIAdapter2* adapter2 = nullptr;
    if (o_GetDesc2 == nullptr && unkAdapter->QueryInterface(__uuidof(IDXGIAdapter2), (void**) &adapter2) == S_OK)
    {
        LOG_DEBUG("Attach to GetDesc2");
        o_GetDesc2 = (PFN_GetDesc2) pVTable[11];
        DetourAttach(&(PVOID&) o_GetDesc2, hkGetDesc2);
    }

    if (adapter2 != nullptr)
        adapter2->Release();

    IDXGIAdapter4* adapter4 = nullptr;
    if (o_GetDesc3 == nullptr && unkAdapter->QueryInterface(__uuidof(IDXGIAdapter4), (void**) &adapter4) == S_OK)
    {
        LOG_DEBUG("Attach to GetDesc3");
        o_GetDesc3 = (PFN_GetDesc3) pVTable[18];
        DetourAttach(&(PVOID&) o_GetDesc3, hkGetDesc3);
    }

    if (adapter4 != nullptr)
        adapter4->Release();

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
        o_GetDesc = nullptr;
        o_GetDesc1 = nullptr;
        o_GetDesc2 = nullptr;
        o_GetDesc3 = nullptr;
    }
}

#pragma endregion
