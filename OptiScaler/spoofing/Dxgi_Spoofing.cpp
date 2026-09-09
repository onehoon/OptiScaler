#include "pch.h"
#include "Dxgi_Spoofing.h"

#include <Config.h>

#include <detours/detours.h>

#include <string>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <misc/IdentifyGpu.h>
#include <misc/ReflexDxgiIdentityScope.h>

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

namespace
{
constexpr uint32_t SelectiveLogLimit = 16;
std::atomic_uint32_t selectiveLogCount = 0;

bool IsExcludedCaller(const std::string& caller)
{
    return iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") || iequals(caller, "dxgi.dll") ||
           iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll");
}

bool IsSelectiveReflexDxgiPocEligible()
{
    const auto* config = Config::Instance();
    if (ReflexDxgiIdentity::ParseScope(config->ReflexDxgiIdentityScope.value_or_default()) ==
            ReflexDxgiIdentity::Scope::Off ||
        !(State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel) ||
        !config->StreamlineSpoofing.value_or_default() || config->DxgiSpoofing.value_or_default() || SkipSpoofing())
    {
        return false;
    }

    return IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;
}

bool TryReserveSelectiveLog()
{
    auto current = selectiveLogCount.load(std::memory_order_relaxed);
    while (current < SelectiveLogLimit &&
           !selectiveLogCount.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
    {
    }
    return current < SelectiveLogLimit;
}

template <typename T> bool ApplyConfiguredIdentity(T* desc, const char* api, const std::string& caller)
{
    const auto* config = Config::Instance();
    const bool targetVendorIdMatches = !config->TargetVendorId.has_value() ||
                                       config->TargetVendorId.value() == desc->VendorId;
    const bool targetDeviceIdMatches = !config->TargetDeviceId.has_value() ||
                                       config->TargetDeviceId.value() == desc->DeviceId;

    if (desc->VendorId == VendorId::Microsoft || !targetVendorIdMatches || !targetDeviceIdMatches)
        return false;

    const bool broadSpoof = config->DxgiSpoofing.value_or_default() && !SkipSpoofing();
    const bool selectiveEligible = !broadSpoof && IsSelectiveReflexDxgiPocEligible();
    const bool selectiveSpoof =
        selectiveEligible && ReflexDxgiIdentity::ShouldApply(caller, config->ReflexDxgiIdentityScope.value_or_default(),
                                                              true, true, true, false, false);

    if (!broadSpoof && !selectiveSpoof)
        return false;

    const auto originalVendorId = desc->VendorId;
    const auto originalDeviceId = desc->DeviceId;
    const auto spoofedVendorId = config->SpoofedVendorId.value_or_default();
    const auto spoofedDeviceId = config->SpoofedDeviceId.value_or_default();
    desc->VendorId = spoofedVendorId;
    desc->DeviceId = spoofedDeviceId;

    const auto spoofedName = config->SpoofedGPUName.value_or_default();
    std::memset(desc->Description, 0, sizeof(desc->Description));
    std::wcscpy(desc->Description, spoofedName.c_str());

    if (selectiveSpoof && !broadSpoof && TryReserveSelectiveLog())
    {
        LOG_INFO("[ReflexDxgiPOC] api={} caller={} scope={} originalVendorId=0x{:X} originalDeviceId=0x{:X} "
                 "spoofVendorId=0x{:X} spoofDeviceId=0x{:X} targetExe={}",
                 api, caller, config->ReflexDxgiIdentityScope.value_or_default(), originalVendorId,
                 originalDeviceId, spoofedVendorId, spoofedDeviceId, State::Instance().gameExe);
    }

#ifdef _DEBUG
    if (broadSpoof)
        LOG_DEBUG("spoofing");
#endif

    return true;
}
} // namespace

#pragma region DXGI Adapter methods

HRESULT DxgiSpoofing::hkGetDesc3(IDXGIAdapter4* This, DXGI_ADAPTER_DESC3* pDesc)
{
    auto result = o_GetDesc3(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (IsExcludedCaller(caller))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        ApplyConfiguredIdentity(pDesc, "GetDesc3", caller);
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc2(IDXGIAdapter2* This, DXGI_ADAPTER_DESC2* pDesc)
{
    auto result = o_GetDesc2(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (IsExcludedCaller(caller))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        ApplyConfiguredIdentity(pDesc, "GetDesc2", caller);
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc1(IDXGIAdapter1* This, DXGI_ADAPTER_DESC1* pDesc)
{
    auto result = o_GetDesc1(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (IsExcludedCaller(caller))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        ApplyConfiguredIdentity(pDesc, "GetDesc1", caller);

        if (caller.starts_with("amdxcffx64") || caller.starts_with("amd_fidelityfx_upscaler_dx12"))
        {
            const auto primaryGpu = IdentifyGpu::getPrimaryGpu();

            if (primaryGpu.fsr4ForcedSupport && primaryGpu.fsr4Support == FSR4Support::INT8)
            {
                LOG_TRACE("Spoofing vendor AMD for {}", caller);
                pDesc->VendorId = VendorId::AMD;
            }
        }
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc(IDXGIAdapter* This, DXGI_ADAPTER_DESC* pDesc)
{
    auto result = o_GetDesc(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (IsExcludedCaller(caller))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        ApplyConfiguredIdentity(pDesc, "GetDesc", caller);
    }

    AttachToAdapter(This);

    return result;
}

#pragma endregion

#pragma region DXGI Attach methods

void DxgiSpoofing::AttachToAdapter(IUnknown* unkAdapter)
{
    static bool logAdded = false;
    const bool broadDxgiNeeded = Config::Instance()->DxgiSpoofing.value_or_default() ||
                                 Config::Instance()->DxgiVRAM.has_value();
    const bool selectiveReflexDxgiNeeded = IsSelectiveReflexDxgiPocEligible();
    if (!broadDxgiNeeded && !selectiveReflexDxgiNeeded)
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
