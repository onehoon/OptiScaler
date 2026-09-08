#include "pch.h"

#include "proxies/FfxApi_Proxy.h"
#include "State.h"
#include "Util.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#include "fakenvapi.h"
#include "NvApiTypes.h"
#include "fakenvapi/nvapi_calls.h"

#define nvapi_interface_table nvapi_interface_table_extern
#include <nvapi_interface.h>
#undef nvapi_interface_table

#pragma intrinsic(_ReturnAddress)

namespace ReflexNvapiGateDiag
{
namespace
{
bool enabled()
{
    const auto& state = State::Instance();
    if (static_cast<bool>(state.gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel))
        return true;

    return _stricmp(state.gameExe.c_str(), "dd2.exe") == 0 || _stricmp(state.gameExe.c_str(), "dd2ccs.exe") == 0;
}

std::string callerName(void* callerAddress)
{
    auto name = Util::WhoIsTheCaller(callerAddress);
    return name.empty() ? "unknown" : name;
}
} // namespace

void logQuery(NvU32 id, const char* name, const char* resolution, void* function, bool cacheHit, void* callerAddress)
{
    if (!enabled())
        return;

    static std::mutex logMutex;
    static std::unordered_set<std::string> loggedQueries;
    static std::unordered_map<NvU32, std::string> knownNames;

    const auto caller = callerName(callerAddress);

    std::scoped_lock lock(logMutex);
    if (name != nullptr && name[0] != '\0')
        knownNames[id] = name;

    const auto nameEntry = knownNames.find(id);
    const auto loggedName = nameEntry != knownNames.end() ? nameEntry->second : "unknown";
    const auto key = std::format("{}|{:08X}|{}", caller, id, resolution);

    if (loggedQueries.emplace(key).second)
    {
        LOG_INFO("[ReflexNvapiGate] QueryInterface caller={} id=0x{:08X} name={} resolution={} cache={} ptr=0x{:X}",
                 caller, id, loggedName, resolution, cacheHit ? "hit" : "miss", reinterpret_cast<uintptr_t>(function));
    }
}

void logCall(const char* functionName, NvAPI_Status status, void* callerAddress)
{
    if (!enabled() || functionName == nullptr)
        return;

    static std::mutex logMutex;
    static std::unordered_set<std::string> loggedCalls;

    const auto caller = callerName(callerAddress);
    const auto key = std::format("{}|{}", caller, functionName);

    std::scoped_lock lock(logMutex);
    if (loggedCalls.emplace(key).second)
    {
        LOG_INFO("[ReflexNvapiGate] Call caller={} function={} status=0x{:08X}", caller, functionName,
                 static_cast<uint32_t>(status));
    }
}
} // namespace ReflexNvapiGateDiag

#undef INSERT_AND_RETURN_WHEN_EQUALS
#define INSERT_AND_RETURN_WHEN_EQUALS(method)                                                                          \
    if (std::string(it->func) == #method)                                                                              \
    {                                                                                                                  \
        const auto function = idToFuncMapping.insert({ id, (void*) nvapi_calls::method }).first->second;               \
        ReflexNvapiGateDiag::logQuery(id, it->func, "implemented", function, false, callerAddress);                    \
        return function;                                                                                               \
    }

std::unordered_map<NvU32, void*> fakenvapi::idToFuncMapping;

void fakenvapi::init(bool onlyContext)
{
    LowLatencyCtx::init();

    if (onlyContext)
        return;
}

void fakenvapi::deinit()
{
    LowLatencyCtx::get()->deinit_current_tech();
    LowLatencyCtx::shutdown();
}

// names from: https://github.com/SveSop/nvapi_standalone/blob/master/dlls/nvapi/nvapi.c
static NVAPI_INTERFACE_TABLE additional_interface_table[] = { { "NvAPI_Diag_ReportCallStart", 0x33c7358c },
                                                              { "NvAPI_Diag_ReportCallReturn", 0x593e8644 },
                                                              { "NvAPI_Unknown_1", 0xe9b009b9 },
                                                              { "NvAPI_SK_1", 0x57f7caac },
                                                              { "NvAPI_SK_2", 0x11104158 },
                                                              { "NvAPI_SK_3", 0xe3795199 },
                                                              { "NvAPI_SK_4", 0xdf0dfcdd },
                                                              { "NvAPI_SK_5", 0x932ac8fb } };

extern "C" __declspec(dllexport) void* nvapi_QueryInterface(NvU32 id)
{
    return fakenvapi::queryInterfaceWithCaller(id, _ReturnAddress());
}

void* __cdecl fakenvapi::queryInterface(NvU32 id) { return queryInterfaceWithCaller(id, _ReturnAddress()); }

void* __cdecl fakenvapi::queryInterfaceWithCaller(NvU32 id, void* callerAddress)
{
    auto entry = idToFuncMapping.find(id);
    if (entry != idToFuncMapping.end())
    {
        const auto resolution = entry->second == nullptr               ? "unknown-null"
                                : entry->second == (void*) placeholder ? "placeholder"
                                                                       : "implemented";
        ReflexNvapiGateDiag::logQuery(id, nullptr, resolution, entry->second, true, callerAddress);
        return entry->second;
    }

    constexpr auto original_size = sizeof(nvapi_interface_table_extern) / sizeof(nvapi_interface_table_extern[0]);
    constexpr auto additional_size = sizeof(additional_interface_table) / sizeof(additional_interface_table[0]);

    constexpr auto total_size = original_size + additional_size;

    struct NVAPI_INTERFACE_TABLE extended_interface_table[total_size] {};
    memcpy(extended_interface_table, nvapi_interface_table_extern,
           sizeof(nvapi_interface_table_extern)); // copy original table

    for (unsigned int i = 0; i < additional_size; i++)
    {
        extended_interface_table[original_size + i] = additional_interface_table[i];
    }

    auto it = std::find_if(std::begin(extended_interface_table), std::end(extended_interface_table),
                           [id](const auto& item) { return item.id == id; });

    if (it == std::end(extended_interface_table))
    {
        LOG_DEBUG("NvAPI_QueryInterface (0x{:x}): Unknown interface ID", id);
        const auto function = idToFuncMapping.insert({ id, nullptr }).first->second;
        ReflexNvapiGateDiag::logQuery(id, "unknown", "unknown-null", function, false, callerAddress);
        return function;
    }

    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Initialize)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetInterfaceVersionString)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_EnumNvidiaDisplayHandle)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetLogicalGPUFromPhysicalGPU)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_EnumPhysicalGPUs)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_EnumLogicalGPUs)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetGPUIDfromPhysicalGPU)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetPhysicalGPUFromGPUID)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetPhysicalGPUsFromDisplay)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetPhysicalGPUsFromLogicalGPU)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetErrorMessage)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GetDisplayDriverVersion)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetLogicalGpuInfo)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetConnectedDisplayIds)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_CudaEnumComputeCapableGpus)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetArchInfo)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetPCIIdentifiers)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetFullName)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetGpuCoreCount)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetAllClockFrequencies)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetAdapterIdFromPhysicalGpu)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_GPU_GetPstates20)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DISP_GetDisplayIdByDisplayName)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DISP_GetGDIPrimaryDisplayId)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_SetOutputMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_GetOutputMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_GetHdrCapabilities)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Disp_HdrColorControl)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Mosaic_GetDisplayViewportsByResolution)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetDisplayDriverInfo)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetDriverAndBranchVersion)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetDisplayIdFromGpuAndOutputId)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SYS_GetGpuAndOutputIdFromDisplayId)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetResourceHint)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetObjectHandleForResource)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetSleepStatus)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_GetLatency)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetSleepMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetLatencyMarker)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_Sleep)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D_SetReflexSync)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_IsNvShaderExtnOpCodeSupported)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_BeginUAVOverlap)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_EndUAVOverlap)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D11_SetDepthBoundsTest)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_GetRaytracingCaps)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_IsNvShaderExtnOpCodeSupported)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_BuildRaytracingAccelerationStructureEx)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_NotifyOutOfBandCommandQueue)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_D3D12_SetAsyncFrameMarker)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_InitLowLatencyDevice)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_DestroyLowLatencyDevice)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_GetSleepStatus)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_SetSleepMode)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_Sleep)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_GetLatency)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_SetLatencyMarker)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Vulkan_NotifyOutOfBandVkQueue)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_CreateSession)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_LoadSettings)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_SaveSettings)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_GetBaseProfile)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_GetSetting)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_SetSetting)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_DRS_DestroySession)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_NGX_GetDriverFeatureSupport)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Unknown_1)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_1)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_2)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_3)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_4)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_SK_5)
    INSERT_AND_RETURN_WHEN_EQUALS(NvAPI_Unload)

    LOG_DEBUG("{}: not implemented, placeholder given", it->func);
    const auto function = idToFuncMapping.insert({ id, (void*) placeholder }).first->second;
    ReflexNvapiGateDiag::logQuery(id, it->func, "placeholder", function, false, callerAddress);
    return function;
    // return registry.insert({ id, nullptr }).first->second;
}

// Inform AntiLag 2 when present of interpolated frames starts
void fakenvapi::reportFGPresent(IDXGISwapChain* pSwapChain, bool fg_state, bool frame_interpolated)
{
    if (!isUsingAsMainNvapi() || State::Instance().activeFgOutput != FGOutput::FSRFG)
        return;

    auto lowLatencyCtx = LowLatencyCtx::get();

    // Lets fakenvapi log and reset correctly
    if (lowLatencyCtx)
        lowLatencyCtx->set_forced_fg(fg_state);
    else
        LOG_ERROR("Couldn't get low latency context");

    if (fg_state)
    {
        // Starting with FSR 3.1.1 we can provide an AntiLag 2 context to FSR FG
        // and it will call SetFrameGenFrameType for us
        auto static ffxApiVersion = FfxApiProxy::VersionDx12();
        constexpr feature_version requiredVersion = { 3, 1, 1 };
        if (ffxApiVersion >= requiredVersion && updateModeAndContext())
        {
            antilag2_data.enabled = _lowLatencyTechContext != nullptr && _lowLatencyMode == LowLatencyMode::AntiLag2;
            antilag2_data.context = antilag2_data.enabled ? _lowLatencyTechContext : nullptr;

            pSwapChain->SetPrivateData(IID_IFfxAntiLag2Data, sizeof(antilag2_data), &antilag2_data);
        }
        else
        {
            // Tell fakenvapi to call SetFrameGenFrameType by itself
            // Reflex frame id might get used in the future
            LOG_TRACE("Fake_InformPresentFG: {}", frame_interpolated);

            if (lowLatencyCtx)
                lowLatencyCtx->set_fg_type(frame_interpolated, 0);
        }
    }
    else
    {
        // Remove it or other FG mods won't work with AL2
        pSwapChain->SetPrivateData(IID_IFfxAntiLag2Data, 0, nullptr);
    }
}

bool fakenvapi::updateModeAndContext()
{
    LOG_FUNC();

    auto lowLatencyCtx = LowLatencyCtx::get();

    if (!lowLatencyCtx)
    {
        // Real reflex should fall here and not log
        if (_usingFakenvapiAsMainNvapi)
            LOG_ERROR("Couldn't get low latency context");

        return false;
    }

    auto result = lowLatencyCtx->get_low_latency_tech_context(&_lowLatencyTechContext, &_lowLatencyMode);

    if (!result)
        LOG_TRACE_FAKENVAPI("Can't get Low Latency context from fakenvapi");

    return result;
}

bool fakenvapi::forceMode(IUnknown* device, LowLatencyMode mode)
{
    LOG_FUNC();

    // Force init here, in case Reflex is not supported
    LowLatencyCtx::init();

    auto lowLatencyCtx = LowLatencyCtx::get();

    if (!lowLatencyCtx)
    {
        LOG_ERROR("Couldn't get low latency context");
        return false;
    }

    auto result = lowLatencyCtx->set_low_latency_tech_mode(device, mode);

    if (!result)
        LOG_TRACE_FAKENVAPI("Can't set Low Latency context from fakenvapi");

    return result;
}

bool fakenvapi::isLowLatencyActive()
{
    auto lowLatencyCtx = LowLatencyCtx::get();

    if (!lowLatencyCtx)
    {
        if (_usingFakenvapiAsMainNvapi)
            LOG_ERROR("Couldn't get low latency context");

        return false;
    }

    return lowLatencyCtx->is_low_latency_enabled();
}

LowLatencyMode fakenvapi::getCurrentMode()
{
    if (updateModeAndContext())
        return _lowLatencyMode;
    else
        return LowLatencyMode::None;
}

void* fakenvapi::getCurrentContext()
{
    if (updateModeAndContext())
        return _lowLatencyTechContext;
    else
        return nullptr;
}

bool fakenvapi::isUsingAsMainNvapi() { return _usingFakenvapiAsMainNvapi; }

void fakenvapi::setUsingAsMainNvapi(bool usingAsMain) { _usingFakenvapiAsMainNvapi = usingAsMain; }
