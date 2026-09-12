#include "pch.h"
#include "XeFG_Dx12.h"
#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <nvapi/fakenvapi.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>
#include <diagnostics/XeFGTrace.h>

using namespace DirectX;

inline static bool IsXeFGWarning(xefg_swapchain_result_t result) { return static_cast<int32_t>(result) > 0; }

inline static void LogXeFGResult(const char* apiName, xefg_swapchain_result_t result)
{
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return;

    if (IsXeFGWarning(result))
    {
        LOG_WARN("{} warning: {} ({})", apiName, magic_enum::enum_name(result), static_cast<int32_t>(result));
    }
    else
    {
        LOG_ERROR("{} error: {} ({})", apiName, magic_enum::enum_name(result), static_cast<int32_t>(result));
    }
}

void XeFG_Dx12::xefgLogCallback(const char* message, xefg_swapchain_logging_level_t level, void* userData)
{
    switch (level)
    {
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG:
        spdlog::debug("XeFG Log: {}", message);
        return;

    case XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO:
        spdlog::info("XeFG Log: {}", message);
        return;

    case XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING:
        spdlog::warn("XeFG Log: {}", message);
        return;

    default:
        spdlog::error("XeFG Log: {}", message);
        return;
    }
}

bool XeFG_Dx12::CreateSwapchainContext(ID3D12Device* device)
{
    if (XeFGProxy::Module() == nullptr && !XeFGProxy::InitXeFG())
    {
        LOG_ERROR("XeFG proxy can't find libxess_fg.dll!");
        return false;
    }

    auto createResult = false;

#ifndef DONT_USE_XMX
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

    do
    {
        auto result = XeFGProxy::D3D12CreateContext()(device, &_swapChainContext);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGCreateContextResult, 0,
                          reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(device), 0, 0, 0,
                          static_cast<int32_t>(result));
        if (static_cast<int32_t>(result) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LogXeFGResult("D3D12CreateContext", result);
            return false;
        }

        LOG_INFO("XeFG context created");
        result = XeFGProxy::SetLoggingCallback()(_swapChainContext, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, xefgLogCallback,
                                                 nullptr);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGSetLoggingCallbackResult, 0,
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(result));

        LogXeFGResult("SetLoggingCallback", result);

#ifndef LOW_LATENCY_INPUTS
        // Force fakenvapi to create XeLL for us
        if (fakenvapi::forceMode(device, LowLatencyMode::XeLL))
        {
            xell_sleep_params_t sleepParams = {};
            sleepParams.bLowLatencyMode = true;
            sleepParams.bLowLatencyBoost = false;
            sleepParams.minimumIntervalUs = 0;

            auto xellResult =
                XeLLProxy::SetSleepMode()((xell_context_handle_t) fakenvapi::getCurrentContext(), &sleepParams);
            if (xellResult != XELL_RESULT_SUCCESS)
            {
                LOG_ERROR("SetSleepMode error: {} ({})", magic_enum::enum_name(xellResult), (UINT) xellResult);
                return false;
            }

            result = XeFGProxy::SetLatencyReduction()(_swapChainContext, fakenvapi::getCurrentContext());
            XeFGTrace::Record(XeFGTrace::EventType::XeFGSetLatencyReductionResult, 0,
                              reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0,
                              static_cast<int32_t>(result), 0, 0);

            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LogXeFGResult("SetLatencyReduction", result);
                return false;
            }
        }
#else
        InputXeLL::xell_input_handle_t localXellContext;
        if (InputXeLL::D3D12CreateContext(device, &localXellContext) == XELL_RESULT_SUCCESS)
        {
            localXellContext->inputContext.localContext = true; // We created this context

            xell_sleep_params_t sleepParams = {};
            sleepParams.bLowLatencyMode = true;
            sleepParams.bLowLatencyBoost = false;
            sleepParams.minimumIntervalUs = 0;

            auto xellResult = InputXeLL::SetSleepMode(localXellContext, &sleepParams);
            if (xellResult != XELL_RESULT_SUCCESS)
            {
                LOG_ERROR("SetSleepMode error: {} ({})", magic_enum::enum_name(xellResult), (UINT) xellResult);
                return false;
            }

            result = XeFGProxy::SetLatencyReduction()(_swapChainContext, (xell_context_handle_t) localXellContext);
            XeFGTrace::Record(XeFGTrace::EventType::XeFGSetLatencyReductionResult, 0,
                              reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0,
                              static_cast<int32_t>(result), 0, 1);

            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LogXeFGResult("SetLatencyReduction", result);
                return false;
            }
        }
#endif
        else
        {
            LOG_ERROR("Couldn't create XeLL");
            return false;
        }

        createResult = true;

    } while (false);

    return createResult;
}

bool XeFG_Dx12::AbortSwapchainInitialization(const char* stage)
{
    if (_swapChainContext == nullptr)
        return false;

    LOG_ERROR("[XeFG][Lifecycle] action = init_aborted, stage = {}, context = {:X}", stage, (size_t) _swapChainContext);

    if (!DestroySwapchainContext())
    {
        LOG_ERROR("[XeFG][Lifecycle] action = init_cleanup_failed, stage = {}, context = {:X}", stage,
                  (size_t) _swapChainContext);
    }

    return false;
}

const char* XeFG_Dx12::Name()
{
    static std::string nameBuffer;

    if (_maxInterpolationCount == 1 || _framesToInterpolate < 0)
    {
        nameBuffer = "XeFG";
    }
    else
    {
        auto count = _framesToInterpolate + 1;
        nameBuffer = "XeFG " + std::to_string(count) + "x";
    }

    return nameBuffer.c_str();
}

feature_version XeFG_Dx12::Version()
{
    if (XeFGProxy::InitXeFG())
    {
        auto ver = XeFGProxy::Version();
        return feature_version(ver.major, ver.minor, ver.patch);
    }

    return { 0, 0, 0 };
}

HWND XeFG_Dx12::Hwnd() { return _hwnd; }

bool XeFG_Dx12::DestroySwapchainContext()
{
    LOG_DEBUG("");

    if (_swapChainContext == nullptr || State::Instance().isShuttingDown)
        return true;

    auto context = _swapChainContext;
    _swapChainContext = nullptr;

    LOG_INFO("[XeFG][Lifecycle] action = destroy_begin, context = {:X}", (size_t) context);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGDestroyBegin, 0, reinterpret_cast<uint64_t>(context));

    auto result = XeFGProxy::Destroy()(context);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGDestroyResult, 0, reinterpret_cast<uint64_t>(context), 0, 0, 0, 0,
                      static_cast<int32_t>(result));
    if (static_cast<int32_t>(result) < 0)
        XeFGTrace::FlushAfterFailureBestEffort();

    LOG_INFO("[XeFG][Lifecycle] action = destroy_return, context = {:X}, result = {} ({})", (size_t) context,
             magic_enum::enum_name(result), static_cast<int32_t>(result));

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        _swapchainRecreationBlocked = true;

        if (IsXeFGWarning(result))
        {
            _swapChainContext = nullptr;
            LOG_WARN("[XeFG][Lifecycle] action = destroy_warning_quarantined, context = {:X}, result = {} ({}), "
                     "retained = false",
                     (size_t) context, magic_enum::enum_name(result), static_cast<int32_t>(result));
        }
        else
        {
            _swapChainContext = context;
            LOG_ERROR("[XeFG][Lifecycle] action = destroy_failed, context = {:X}, result = {} ({}), retained = true",
                      (size_t) context, magic_enum::enum_name(result), static_cast<int32_t>(result));
        }

        return false;
    }

    _swapchainRecreationBlocked = false;
    State::Instance().currentFGSwapchain = nullptr;
    return true;
}

xefg_swapchain_d3d12_resource_data_t XeFG_Dx12::GetResourceData(FG_ResourceType type, int index)
{
    if (index < 0)
    {
        index = GetIndex();
        LOG_WARN("GetResourceData called with -1 index, using current index: {}", index);
    }

    xefg_swapchain_d3d12_resource_data_t resourceParam = {};

    if (!_frameResources[index].contains(type))
    {
        LOG_WARN("Resource type not found: {} for index: {}", magic_enum::enum_name(type), index);
        return resourceParam;
    }

    auto fResource = &_frameResources[index].at(type);

    resourceParam.validity = (fResource->validity == FG_ResourceValidity::ValidNow)
                                 ? XEFG_SWAPCHAIN_RV_ONLY_NOW
                                 : XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;

    resourceParam.resourceBase = { fResource->left, fResource->top };
    resourceParam.resourceSize = { static_cast<uint32_t>(fResource->width), fResource->height };
    resourceParam.pResource = fResource->GetResource();
    resourceParam.incomingState = fResource->state;

    switch (type)
    {
    case FG_ResourceType::Depth:
        resourceParam.type = XEFG_SWAPCHAIN_RES_DEPTH;
        break;

    case FG_ResourceType::HudlessColor:
        resourceParam.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        break;

    case FG_ResourceType::UIColor:
        resourceParam.type = XEFG_SWAPCHAIN_RES_UI;
        break;

    case FG_ResourceType::Velocity:
        resourceParam.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        break;
    default:
        LOG_WARN("Unsupported resource type: {}", magic_enum::enum_name(type));
        return xefg_swapchain_d3d12_resource_data_t {};
    }

    return resourceParam;
}

bool XeFG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                IDXGISwapChain** swapChain, bool readyToRelease)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock())
    {
        LOG_WARN("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain, "
                 "reason = lifecycle_transaction_in_progress");
        return false;
    }

    if (_swapchainRecreationBlocked)
    {
        LOG_WARN("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain, "
                 "reason = previous_destroy_failed");
        return false;
    }

    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");

            if (!ReleaseSwapchainLocked(_hwnd))
            {
                LOG_ERROR("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain, "
                          "reason = release_not_completed");
                return false;
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (_swapChainContext == nullptr)
    {
        LOG_DEBUG("Creating swapchain context for the first time");

        if (State::Instance().currentD3D12Device == nullptr)
            return false;

        if (!CreateSwapchainContext(State::Instance().currentD3D12Device))
            return AbortSwapchainInitialization("CreateSwapchainContext");

        _width = desc->BufferDesc.Width;
        _height = desc->BufferDesc.Height;

        xefg_swapchain_properties_t props {};
        auto result = XeFGProxy::GetProperties()(_swapChainContext, &props);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGGetPropertiesResult, reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(result));
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            _maxInterpolationCount = props.maxSupportedInterpolations;
            LOG_INFO("Max supported interpolations: {}", props.maxSupportedInterpolations);
        }
        else
        {
            LogXeFGResult("GetProperties", result);
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    IDXGIFactory2* factory12 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
        return false;

    factory12->Release();

    HWND hwnd = desc->OutputWindow;
    DXGI_SWAP_CHAIN_DESC1 scDesc {};

    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE; // No info
    scDesc.BufferCount = desc->BufferCount;
    scDesc.BufferUsage = desc->BufferUsage;
    scDesc.Flags = desc->Flags;
    scDesc.Format = desc->BufferDesc.Format;
    scDesc.Height = desc->BufferDesc.Height;
    scDesc.SampleDesc = desc->SampleDesc;

    switch (desc->BufferDesc.Scaling)
    {
    case DXGI_MODE_SCALING_CENTERED:
        scDesc.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
        break;

    case DXGI_MODE_SCALING_STRETCHED:
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        break;

    case DXGI_MODE_SCALING_UNSPECIFIED:
        scDesc.Scaling = DXGI_SCALING_NONE;
        break;
    }

    scDesc.Stereo = false; // No info
    scDesc.SwapEffect = desc->SwapEffect;
    scDesc.Width = desc->BufferDesc.Width;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc {};
    fsDesc.RefreshRate = desc->BufferDesc.RefreshRate;
    fsDesc.Scaling = desc->BufferDesc.Scaling;
    fsDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fsDesc.Windowed = desc->Windowed;

    xefg_swapchain_d3d12_init_params_t params {};

    int intTarget = _maxInterpolationCount;

    // For old libxess_fg versions we use max to control interpolation count
    if (XeFGProxy::SetNumInterpolatedFrames() == nullptr)
        intTarget = Config::Instance()->FGXeFGInterpolationCount.value_or_default();

    if (intTarget < 1 || intTarget > _maxInterpolationCount)
    {
        LOG_WARN("Invalid XeFG interpolation count: {}, max count: {}", intTarget, _maxInterpolationCount);

        intTarget = 1;
    }

    if (_framesToInterpolate > intTarget)
        Config::Instance()->FGXeFGInterpolationCount.set_volatile_value(intTarget);

    if (Config::Instance()->ForceXeLL.value_or_default())
        params.maxInterpolatedFrames = 1;

    params.maxInterpolatedFrames = intTarget;

    params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;

    if (!Config::Instance()->FGUIPremultipliedAlpha.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;

    LOG_DEBUG("Inverted Depth: {}", Config::Instance()->FGXeFGDepthInverted.value_or_default());
    LOG_DEBUG("Jittered Velocity: {}", Config::Instance()->FGXeFGJitteredMV.value_or_default());
    LOG_DEBUG("High Res MV: {}", Config::Instance()->FGXeFGHighResMV.value_or_default());

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        _constants.flags |= FG_Flags::InvertedDepth;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        _constants.flags |= FG_Flags::JitteredMVs;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        _constants.flags |= FG_Flags::DisplayResolutionMVs;

#ifndef DONT_USE_XMX
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

    xefg_swapchain_result_t result;
    result = XeFGProxy::D3D12InitFromSwapChainDesc()(_swapChainContext, hwnd, &scDesc, &fsDesc, realQueue, factory12,
                                                     &params);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGInitSwapchainResult, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(realQueue), 0, 0, 0,
                      static_cast<int32_t>(result));
    if (static_cast<int32_t>(result) < 0)
        XeFGTrace::FlushAfterFailureBestEffort();

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LogXeFGResult("D3D12InitFromSwapChainDesc", result);
        return AbortSwapchainInitialization("D3D12InitFromSwapChainDesc");
    }

    IDXGISwapChain* queriedSwapChain = nullptr;
    result = XeFGProxy::D3D12GetSwapChainPtr()(_swapChainContext, IID_PPV_ARGS(&queriedSwapChain));
    XeFGTrace::Record(XeFGTrace::EventType::XeFGGetSwapchainPtrResult,
                      reinterpret_cast<uint64_t>(queriedSwapChain), reinterpret_cast<uint64_t>(_swapChainContext), 0,
                      0, 0, 0, static_cast<int32_t>(result));
    if (static_cast<int32_t>(result) < 0)
        XeFGTrace::FlushAfterFailureBestEffort();
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LogXeFGResult("D3D12GetSwapChainPtr", result);
        SAFE_RELEASE(queriedSwapChain);
        return AbortSwapchainInitialization("D3D12GetSwapChainPtr");
    }

    *swapChain = queriedSwapChain;
    LOG_INFO("XeFG swapchain created");

    // When forcing XeLL, always tell XeFG that FG is active, even tho we don't send anything
    if (State::Instance().activeFgInput == FGInput::ForceXeLL)
    {
        auto enabledResult = XeFGProxy::SetEnabled()(_swapChainContext, true);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGSetEnabledResult, reinterpret_cast<uint64_t>(*swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0,
                          static_cast<int32_t>(enabledResult), 0, 1);
        if (static_cast<int32_t>(enabledResult) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();
    }

    _gameCommandQueue = realQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    return true;
}

bool XeFG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                 DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                 IDXGISwapChain1** swapChain, bool readyToRelease)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock())
    {
        LOG_WARN("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain1, "
                 "reason = lifecycle_transaction_in_progress");
        return false;
    }

    if (_swapchainRecreationBlocked)
    {
        LOG_WARN("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain1, "
                 "reason = previous_destroy_failed");
        return false;
    }

    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");

            if (!ReleaseSwapchainLocked(_hwnd))
            {
                LOG_ERROR("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain1, "
                          "reason = release_not_completed");
                return false;
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (_swapChainContext == nullptr)
    {
        if (State::Instance().currentD3D12Device == nullptr)
            return false;

        if (!CreateSwapchainContext(State::Instance().currentD3D12Device))
            return AbortSwapchainInitialization("CreateSwapchainContext");

        _width = desc->Width;
        _height = desc->Height;

        xefg_swapchain_properties_t props {};
        auto result = XeFGProxy::GetProperties()(_swapChainContext, &props);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGGetPropertiesResult, reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(result));
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            _maxInterpolationCount = props.maxSupportedInterpolations;
            LOG_INFO("Max supported interpolations: {}", props.maxSupportedInterpolations);
        }
        else
        {
            LogXeFGResult("GetProperties", result);
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    IDXGIFactory2* factory12 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
        return false;

    factory12->Release();

    xefg_swapchain_d3d12_init_params_t params {};

    int intTarget = _maxInterpolationCount;

    // For old libxess_fg versions we use max to control interpolation count
    if (XeFGProxy::SetNumInterpolatedFrames() == nullptr)
        intTarget = Config::Instance()->FGXeFGInterpolationCount.value_or_default();

    if (intTarget < 1 || intTarget > _maxInterpolationCount)
    {
        LOG_WARN("Invalid XeFG interpolation count: {}, max count: {}", intTarget, _maxInterpolationCount);

        intTarget = 1;
    }

    if (_framesToInterpolate > intTarget)
        Config::Instance()->FGXeFGInterpolationCount.set_volatile_value(intTarget);

    if (Config::Instance()->ForceXeLL.value_or_default())
        params.maxInterpolatedFrames = 1;

    params.maxInterpolatedFrames = intTarget;

    params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;

    if (!Config::Instance()->FGUIPremultipliedAlpha.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;

    LOG_DEBUG("Inverted Depth: {}", Config::Instance()->FGXeFGDepthInverted.value_or_default());
    LOG_DEBUG("Jittered Velocity: {}", Config::Instance()->FGXeFGJitteredMV.value_or_default());
    LOG_DEBUG("High Res MV: {}", Config::Instance()->FGXeFGHighResMV.value_or_default());

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        _constants.flags |= FG_Flags::InvertedDepth;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        _constants.flags |= FG_Flags::JitteredMVs;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        _constants.flags |= FG_Flags::DisplayResolutionMVs;

    xefg_swapchain_result_t result;

    {
#ifndef DONT_USE_XMX
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX
        result = XeFGProxy::D3D12InitFromSwapChainDesc()(_swapChainContext, hwnd, desc, pFullscreenDesc, realQueue,
                                                         factory12, &params);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGInitSwapchainResult, reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(realQueue), 0, 0,
                          0, static_cast<int32_t>(result));
        if (static_cast<int32_t>(result) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();
    }

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LogXeFGResult("D3D12InitFromSwapChainDesc", result);
        return AbortSwapchainInitialization("D3D12InitFromSwapChainDesc");
    }

    IDXGISwapChain1* queriedSwapChain = nullptr;
    result = XeFGProxy::D3D12GetSwapChainPtr()(_swapChainContext, IID_PPV_ARGS(&queriedSwapChain));
    XeFGTrace::Record(XeFGTrace::EventType::XeFGGetSwapchainPtrResult,
                      reinterpret_cast<uint64_t>(queriedSwapChain), reinterpret_cast<uint64_t>(_swapChainContext), 0,
                      0, 0, 0, static_cast<int32_t>(result));
    if (static_cast<int32_t>(result) < 0)
        XeFGTrace::FlushAfterFailureBestEffort();
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LogXeFGResult("D3D12GetSwapChainPtr", result);
        SAFE_RELEASE(queriedSwapChain);
        return AbortSwapchainInitialization("D3D12GetSwapChainPtr");
    }

    *swapChain = queriedSwapChain;
    LOG_INFO("XeFG swapchain created");

    // When forcing XeLL, always tell XeFG that FG is active, even tho we don't send anything
    if (State::Instance().activeFgInput == FGInput::ForceXeLL)
    {
        auto enabledResult = XeFGProxy::SetEnabled()(_swapChainContext, true);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGSetEnabledResult, reinterpret_cast<uint64_t>(*swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0,
                          static_cast<int32_t>(enabledResult), 0, 1);
        if (static_cast<int32_t>(enabledResult) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();
    }

    _gameCommandQueue = realQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    return true;
}

void XeFG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    _device = device;
    CreateObjects(device);

    if (_fgContext == nullptr && _swapChainContext != nullptr)
    {
        _fgContext = _swapChainContext;
        _lastDispatchedFrame = 0;
    }

    if (_isActive)
    {
        LOG_INFO("FG context recreated while active, pausing");
        State::Instance().fgChanged = true;
        UpdateTarget();
        Deactivate();
    }
}

void XeFG_Dx12::Activate()
{
    LOG_DEBUG("");

    auto currentFeature = State::Instance().currentFeature;
    bool nativeAA = false;
    if (State::Instance().activeFgInput == FGInput::Upscaler && currentFeature != nullptr)
        nativeAA = currentFeature->RenderWidth() == currentFeature->DisplayWidth();

    if (_swapChainContext != nullptr && _fgContext != nullptr && !_isActive &&
        (IsLowResMV() || nativeAA || (State::Instance().gameQuirks & GameQuirk::ForceFGRenderSizeMVs) ||
         Config::Instance()->FGXeFGIgnoreInitChecks.value_or_default()))
    {
        auto result = XeFGProxy::SetEnabled()(_swapChainContext, true);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGSetEnabledResult, reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(result), 0,
                          1);
        if (static_cast<int32_t>(result) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();

        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            _isActive = true;
            _lastDispatchedFrame = 0;
        }

        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            LOG_INFO("SetEnabled: true, result: {} ({})", magic_enum::enum_name(result), static_cast<int32_t>(result));
        else
            LogXeFGResult("SetEnabled(true)", result);
    }
}

void XeFG_Dx12::Deactivate()
{
    LOG_DEBUG("");

    if (_isActive)
    {
        auto fIndex = GetIndex();
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[fIndex][{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();
            XeFGTrace::Record(XeFGTrace::EventType::UiCommandListCloseResult,
                              reinterpret_cast<uint64_t>(_swapChain),
                              reinterpret_cast<uint64_t>(_uiCommandList[fIndex]),
                              reinterpret_cast<uint64_t>(_gameCommandQueue), _uiAllocatorFenceValues[fIndex], 0, 0,
                              static_cast<int32_t>(closeResult), 0, static_cast<uint32_t>(fIndex));

            if (closeResult == S_OK)
            {
                XeFGTrace::Record(XeFGTrace::EventType::UiExecuteCommandListsBegin,
                                  reinterpret_cast<uint64_t>(_swapChain),
                                  reinterpret_cast<uint64_t>(_gameCommandQueue),
                                  reinterpret_cast<uint64_t>(_uiCommandList[fIndex]), _uiAllocatorFenceValues[fIndex],
                                  0, 0, 0, 0, static_cast<uint32_t>(fIndex));
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
                XeFGTrace::Record(XeFGTrace::EventType::UiExecuteCommandListsEnd,
                                  reinterpret_cast<uint64_t>(_swapChain),
                                  reinterpret_cast<uint64_t>(_gameCommandQueue),
                                  reinterpret_cast<uint64_t>(_uiCommandList[fIndex]), _uiAllocatorFenceValues[fIndex],
                                  0, 0, 0, 0, static_cast<uint32_t>(fIndex));
            }
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            auto signalResult = _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);
            XeFGTrace::Record(XeFGTrace::EventType::UiQueueSignalResult,
                              reinterpret_cast<uint64_t>(_swapChain), reinterpret_cast<uint64_t>(_uiFence),
                              reinterpret_cast<uint64_t>(_gameCommandQueue), _uiAllocatorFenceValues[fIndex], 0, 0,
                              static_cast<int32_t>(signalResult), 0, static_cast<uint32_t>(fIndex));

            _uiCommandListResetted[fIndex] = false;
        }

        xefg_swapchain_result_t result = XEFG_SWAPCHAIN_RESULT_SUCCESS;

        if (_swapChainContext != nullptr)
        {
            result = XeFGProxy::SetEnabled()(_swapChainContext, false);
            XeFGTrace::Record(XeFGTrace::EventType::XeFGSetEnabledResult, reinterpret_cast<uint64_t>(_swapChain),
                              reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0,
                              static_cast<int32_t>(result), 0, 0);
            if (static_cast<int32_t>(result) < 0)
                XeFGTrace::FlushAfterFailureBestEffort();
            if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                _isActive = false;
        }
        else
        {
            _isActive = false;
        }

        if (_swapChainContext == nullptr || result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            _waitingNewFrameData = false;

        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            LOG_INFO("SetEnabled: false, result: {} ({})", magic_enum::enum_name(result), static_cast<int32_t>(result));
        else
            LogXeFGResult("SetEnabled(false)", result);
    }
}

void XeFG_Dx12::DestroyFGContext()
{
    Deactivate();

    if (_fgContext != nullptr)
        _fgContext = nullptr;

    ReleaseObjects();
}

bool XeFG_Dx12::Shutdown()
{
    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    ReleaseObjects();

    if (_swapChainContext != nullptr)
        DestroySwapchainContext();

    return true;
}

bool XeFG_Dx12::Dispatch()
{
    XeFGTrace::Record(XeFGTrace::EventType::DispatchEnter, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), _frameCount, 0, 0, 0, 0, 0,
                      static_cast<uint32_t>(_frameCount), static_cast<uint32_t>(_frameCount >> 32));
    LOG_FUNC();

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame);
    XeFGTrace::Record(XeFGTrace::EventType::DispatchAfterIndexResolve, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), 0, willDispatchFrame, 0, 0, 0, 0,
                      static_cast<uint32_t>(fIndex), static_cast<uint32_t>(willDispatchFrame));
    if (fIndex < 0)
        return false;

    if (!IsActive() || IsPaused())
        return false;

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    auto& state = State::Instance();

    if (XeFGProxy::SetUiCompositionState() != nullptr &&
        Config::Instance()->FGXeFGUIComposition.value_or_default() != _uiComposition && IsUsingHudless(fIndex))
    {
        // To prevent XeLL issues
        LOG_DEBUG("UI Composition state changed {}, skipping rendering for 10 frames", _uiComposition);
        state.WAR_xefgRequestFGToggle = true;

        _uiComposition = Config::Instance()->FGXeFGUIComposition.value_or_default();

        auto uiState =
            _uiComposition ? XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED : XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED;

#ifndef DONT_USE_XMX
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

        auto uiResult = XeFGProxy::SetUiCompositionState()(_swapChainContext, uiState);

        XeFGTrace::Record(XeFGTrace::EventType::XeFGSetUiCompositionResult, reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(uiResult), 0,
                          static_cast<uint32_t>(uiState));
        if (static_cast<int32_t>(uiResult) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();

        LogXeFGResult("SetUiCompositionState", uiResult);
    }

    if (XeFGProxy::SetNumInterpolatedFrames() != nullptr)
    {
        if (Config::Instance()->FGXeFGInterpolationCount.value_or_default() > _maxInterpolationCount)
        {
            Config::Instance()->FGXeFGInterpolationCount = _maxInterpolationCount;
            LOG_WARN("Requested interpolation count is higher than max supported, setting to max: {}",
                     _maxInterpolationCount);
        }

        if (_framesToInterpolate != Config::Instance()->FGXeFGInterpolationCount.value_or_default())
        {
            LOG_INFO("Interpolation count changed {} -> {}", _framesToInterpolate,
                     Config::Instance()->FGXeFGInterpolationCount.value_or_default());

            state.WAR_xefgRequestFGToggle = true;

#ifndef DONT_USE_XMX
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

            auto intResult = XeFGProxy::SetNumInterpolatedFrames()(
                _swapChainContext, Config::Instance()->FGXeFGInterpolationCount.value_or_default());

            XeFGTrace::Record(XeFGTrace::EventType::XeFGSetNumInterpolatedFramesResult,
                              reinterpret_cast<uint64_t>(_swapChain), reinterpret_cast<uint64_t>(_swapChainContext), 0,
                              0, 0, 0, static_cast<int32_t>(intResult), 0,
                              static_cast<uint32_t>(Config::Instance()->FGXeFGInterpolationCount.value_or_default()));
            if (static_cast<int32_t>(intResult) < 0)
                XeFGTrace::FlushAfterFailureBestEffort();

            _framesToInterpolate = Config::Instance()->FGXeFGInterpolationCount.value_or_default();

            LogXeFGResult("SetNumInterpolatedFrames", intResult);
        }
    }

    // Workaround for wrong frame limit
    if (state.WAR_xefgRequestFGToggle)
    {
        state.WAR_xefgRequestFGToggle = false;

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();
    }

    if (!_haveHudless.has_value())
    {
        _haveHudless = IsUsingHudless(fIndex);
    }
    else
    {
        auto usingHudless = IsUsingHudless(fIndex);
        static auto version = Version();

        // SDK version 2.1.1 fixed this
        // https://github.com/intel/xess/issues/48
        if (version < feature_version { 1, 2, 2 } && _haveHudless.value() != usingHudless)
        {
            LOG_INFO("Hudless state changed {} -> {}, skipping rendering for 10 frames", _haveHudless.value(),
                     usingHudless);

            _haveHudless = usingHudless;
            state.fgChanged = true;
            UpdateTarget();
            Deactivate();

            return false;
        }
    }

    if (!_noHudless[fIndex])
    {
        XeFGTrace::Record(XeFGTrace::EventType::DispatchBeforeHudlessLookup,
                          reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, 0, 0,
                          static_cast<uint32_t>(fIndex));
        auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
        XeFGTrace::Record(XeFGTrace::EventType::DispatchAfterHudlessLookup,
                          reinterpret_cast<uint64_t>(_swapChain), reinterpret_cast<uint64_t>(res),
                          reinterpret_cast<uint64_t>(res->resource), 0, 0, 0, 0, 0,
                          static_cast<uint32_t>(fIndex), static_cast<uint32_t>(res->validity));
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            XeFGTrace::Record(XeFGTrace::EventType::DispatchBeforeHudlessSetResource,
                              reinterpret_cast<uint64_t>(_swapChain), reinterpret_cast<uint64_t>(res),
                              reinterpret_cast<uint64_t>(res->resource), reinterpret_cast<uint64_t>(res->cmdList), 0, 0,
                              0, 0, static_cast<uint32_t>(fIndex), static_cast<uint32_t>(res->validity));
            SetResource(res);
        }
    }

    if (!_noDistortionField[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::Distortion];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    auto debugResult = XeFGProxy::EnableDebugFeature()(
        _swapChainContext, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,
        Config::Instance()->FGXeFGDebugView.value_or_default(), nullptr);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGEnableDebugFeatureResult, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(debugResult), 0,
                      0);

    debugResult = XeFGProxy::EnableDebugFeature()(_swapChainContext,
                                                   XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION,
                                                   state.fgOnlyGenerated, nullptr);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGEnableDebugFeatureResult, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(debugResult), 0,
                      1);

    xefg_swapchain_frame_constant_data_t constData = {};

    if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
    {
        XMVECTOR right = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraRight[fIndex]));
        XMVECTOR up = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraUp[fIndex]));
        XMVECTOR forward = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraForward[fIndex]));
        XMVECTOR pos = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraPosition[fIndex]));

        float x = -XMVectorGetX(XMVector3Dot(pos, right));
        float y = -XMVectorGetX(XMVector3Dot(pos, up));
        float z = -XMVectorGetX(XMVector3Dot(pos, forward));

        XMMATRIX view = { XMVectorSet(XMVectorGetX(right), XMVectorGetX(up), XMVectorGetX(forward), 0.0f),
                          XMVectorSet(XMVectorGetY(right), XMVectorGetY(up), XMVectorGetY(forward), 0.0f),
                          XMVectorSet(XMVectorGetZ(right), XMVectorGetZ(up), XMVectorGetZ(forward), 0.0f),
                          XMVectorSet(x, y, z, 1.0f) };

        memcpy(constData.viewMatrix, view.r, sizeof(view));
    }

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        std::swap(_cameraNear[fIndex], _cameraFar[fIndex]);

    if (_infiniteDepth && _cameraFar[fIndex] > _cameraNear[fIndex])
        _cameraFar[fIndex] = std::numeric_limits<float>::infinity();
    else if (_infiniteDepth && _cameraNear[fIndex] > _cameraFar[fIndex])
        _cameraNear[fIndex] = std::numeric_limits<float>::infinity();

    // Cyberpunk seems to be sending LH so do the same
    // it also sends some extra data in usually empty spots but no idea what that is
    if (_cameraNear[fIndex] > 0.f && _cameraFar[fIndex] > 0.f &&
        !XMScalarNearEqual(_cameraVFov[fIndex], 0.0f, 0.00001f) &&
        !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
    {
        if (XMScalarNearEqual(_cameraNear[fIndex], _cameraFar[fIndex], 0.00001f))
            _cameraFar[fIndex]++;

        auto projectionMatrix = XMMatrixPerspectiveFovLH(_cameraVFov[fIndex], _cameraAspectRatio[fIndex],
                                                         _cameraNear[fIndex], _cameraFar[fIndex]);
        memcpy(constData.projectionMatrix, projectionMatrix.r, sizeof(projectionMatrix));
    }
    else
    {
        LOG_WARN("Can't calculate projectionMatrix");
    }

    constData.jitterOffsetX = _jitterX[fIndex];
    constData.jitterOffsetY = _jitterY[fIndex];
    constData.motionVectorScaleX = _mvScaleX[fIndex];
    constData.motionVectorScaleY = _mvScaleY[fIndex];

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.resetHistory = _reset[fIndex];
    else
        constData.resetHistory = false;

    switch (Config::Instance()->FTInput.value_or_default())
    {
    case FrameTimeSource::Input:
        constData.frameRenderTime = (float) _ftDelta[fIndex];
        break;

    case FrameTimeSource::Opti:
        constData.frameRenderTime = static_cast<float>(state.lastFGFrameTime);
        break;

    case FrameTimeSource::Zero:
        constData.frameRenderTime = 0.0f;
        break;
    }

    LOG_DEBUG("Reset: {}, Opti FT: {}, Source FT: {}, Set FT: {}, Opti Id: {}, Reflex Id: {}", _reset[fIndex],
              constData.frameRenderTime, _ftDelta[fIndex], constData.frameRenderTime, _frameCount,
              State::Instance().reflexFrameId);

    auto frameId = static_cast<uint32_t>(willDispatchFrame);

    auto result = XeFGProxy::TagFrameConstants()(_swapChainContext, frameId, &constData);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGTagFrameConstantsResult, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(result), 0,
                      frameId);
    if (static_cast<int32_t>(result) < 0)
        XeFGTrace::FlushAfterFailureBestEffort();
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LogXeFGResult("TagFrameConstants", result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    result = XeFGProxy::SetPresentId()(_swapChainContext, frameId);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGSetPresentIdResult, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), 0, 0, 0, 0, static_cast<int32_t>(result), 0,
                      frameId);
    if (static_cast<int32_t>(result) < 0)
        XeFGTrace::FlushAfterFailureBestEffort();
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LogXeFGResult("SetPresentId", result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    // When using Hudfix, we always copy hudless as swapchain size
    if (state.activeFgInput != FGInput::Upscaler)
    {
        uint32_t left = 0;
        uint32_t top = 0;

        if (_interpolationWidth[fIndex] == 0 && _interpolationHeight[fIndex] == 0)
        {
            LOG_WARN("Interpolation size is 0, using swapchain size");
            _interpolationWidth[fIndex] = state.currentSwapchainDesc.BufferDesc.Width;
            _interpolationHeight[fIndex] = state.currentSwapchainDesc.BufferDesc.Height;
        }
        else
        {
            auto calculatedLeft =
                ((int) state.currentSwapchainDesc.BufferDesc.Width - (int) _interpolationWidth[fIndex]) / 2;
            if (calculatedLeft > 0)
                left = Config::Instance()->FGRectLeft.value_or(_interpolationLeft[fIndex].value_or(calculatedLeft));

            auto calculatedTop =
                ((int) state.currentSwapchainDesc.BufferDesc.Height - (int) _interpolationHeight[fIndex]) / 2;
            if (calculatedTop > 0)
                top = Config::Instance()->FGRectTop.value_or(_interpolationTop[fIndex].value_or(calculatedTop));
        }

        LOG_DEBUG("SwapChain Res: {}x{}, Interpolation Res: {}x{}", state.currentSwapchainDesc.BufferDesc.Width,
                  state.currentSwapchainDesc.BufferDesc.Height, _interpolationWidth[fIndex],
                  _interpolationHeight[fIndex]);

        xefg_swapchain_d3d12_resource_data_t backbuffer = {};
        backbuffer.type = XEFG_SWAPCHAIN_RES_BACKBUFFER;
        backbuffer.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        backbuffer.resourceBase = { (UINT) Config::Instance()->FGRectLeft.value_or(left),
                                    (UINT) Config::Instance()->FGRectTop.value_or(top) };
        backbuffer.resourceSize = { static_cast<uint32_t>(
                                        Config::Instance()->FGRectWidth.value_or(_interpolationWidth[fIndex])),
                                    (UINT) Config::Instance()->FGRectHeight.value_or(_interpolationHeight[fIndex]) };

        result = XeFGProxy::D3D12TagFrameResource()(_swapChainContext, (ID3D12CommandList*) 1, frameId, &backbuffer);
        XeFGTrace::Record(XeFGTrace::EventType::XeFGTagFrameResourceResult, reinterpret_cast<uint64_t>(_swapChain),
                          reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(backbuffer.pResource),
                          0, 0, 0, static_cast<int32_t>(result), 0, frameId);
        if (static_cast<int32_t>(result) < 0)
            XeFGTrace::FlushAfterFailureBestEffort();

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LogXeFGResult("D3D12TagFrameResource Backbuffer", result);

            state.fgChanged = true;
            UpdateTarget();
            Deactivate();
        }
    }

    LOG_DEBUG("Result: Ok");

    return true;
}

void* XeFG_Dx12::FrameGenerationContext() { return _fgContext; }

void* XeFG_Dx12::SwapchainContext() { return _swapChainContext; }

XeFG_Dx12::~XeFG_Dx12() { Shutdown(); }

bool XeFG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

void XeFG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    auto& state = State::Instance();

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!XeFGProxy::InitXeFG() || state.currentFGSwapchain == nullptr)
        return;

    if (state.isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    _infiniteDepth = static_cast<bool>(fgConstants.flags & FG_Flags::InfiniteDepth);

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        // If FG context is nullptr
        if (_fgContext == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);

            // Pause for 10 frames
            UpdateTarget();
        }
        // If there is a change deactivate it
        else if (state.fgChanged)
        {
            LOG_DEBUG("FGChanged");
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();

            // Destroy if Swapchain has a change destroy FG Context too
            if (state.scChanged)
                DestroyFGContext();
        }

        if (_fgContext != nullptr && State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        LOG_DEBUG("!FGEnabled");
        Deactivate();

        state.clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (state.fgChanged)
    {
        LOG_DEBUG("FGchanged");

        state.fgChanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.scChanged = false;
}

void XeFG_Dx12::ReleaseObjects()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _mvFlip.reset();
    _depthFlip.reset();
    _depthInvert.reset();
}

void XeFG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

bool XeFG_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();
    LOG_DEBUG("fIndex: {}", fIndex);
    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentEnter, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex));
    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentBeforeUiWork, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex));

    if (Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui && (ui->validity == FG_ResourceValidity::UntilPresent ||
                   ui->validity == FG_ResourceValidity::JustTrackCmdlist ||
                   ui->validity == FG_ResourceValidity::UntilPresentFromDispatch))
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                    _renderUI = std::make_unique<RUI_Dx12>(
                        "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _renderUI->Dispatch((IDXGISwapChain3*) _swapChain, commandList, ui->GetResource(), ui->state);
                }
            }
        }
        else if (!ui)
        {
            LOG_WARN("UI resource is nullptr");
        }
    }

    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentAfterUiWork, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex));
    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentBeforeScWork, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex));

    if (IsActive() && !IsPaused())
    {
        if (State::Instance().fgHudlessCompare)
        {
            auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
            if (hudless && (hudless->validity == FG_ResourceValidity::UntilPresent ||
                            hudless->validity == FG_ResourceValidity::JustTrackCmdlist ||
                            hudless->validity == FG_ResourceValidity::UntilPresentFromDispatch))
            {
                LOG_DEBUG("Hudless[{}] resource: {:X}, copy: {}", fIndex, (size_t) hudless->resource,
                          (size_t) hudless->copy);
                if (_hudlessCompare.get() == nullptr)
                {
                    _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
                }
                else
                {
                    if (_hudlessCompare->IsInit())
                    {
                        auto commandList = GetSCCommandList(fIndex);
                        _hudlessCompare->Dispatch((IDXGISwapChain3*) _swapChain, commandList, hudless->GetResource(),
                                                  hudless->state);
                    }
                }
            }
            else if (!hudless)
            {
                LOG_WARN("Hudless resource is nullptr");
            }
        }
    }

    bool result = false;

    // if (IsActive() && !IsPaused())
    {
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();
            XeFGTrace::Record(XeFGTrace::EventType::UiCommandListCloseResult,
                              reinterpret_cast<uint64_t>(_swapChain),
                              reinterpret_cast<uint64_t>(_uiCommandList[fIndex]),
                              reinterpret_cast<uint64_t>(_gameCommandQueue), _uiAllocatorFenceValues[fIndex], 0, 0,
                              static_cast<int32_t>(closeResult), 0, static_cast<uint32_t>(fIndex));

            if (closeResult == S_OK)
            {
                XeFGTrace::Record(XeFGTrace::EventType::UiExecuteCommandListsBegin,
                                  reinterpret_cast<uint64_t>(_swapChain),
                                  reinterpret_cast<uint64_t>(_gameCommandQueue),
                                  reinterpret_cast<uint64_t>(_uiCommandList[fIndex]), _uiAllocatorFenceValues[fIndex],
                                  0, 0, 0, 0, static_cast<uint32_t>(fIndex));
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
                XeFGTrace::Record(XeFGTrace::EventType::UiExecuteCommandListsEnd,
                                  reinterpret_cast<uint64_t>(_swapChain),
                                  reinterpret_cast<uint64_t>(_gameCommandQueue),
                                  reinterpret_cast<uint64_t>(_uiCommandList[fIndex]), _uiAllocatorFenceValues[fIndex],
                                  0, 0, 0, 0, static_cast<uint32_t>(fIndex));
            }
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            auto signalResult = _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);
            XeFGTrace::Record(XeFGTrace::EventType::UiQueueSignalResult,
                              reinterpret_cast<uint64_t>(_swapChain), reinterpret_cast<uint64_t>(_uiFence),
                              reinterpret_cast<uint64_t>(_gameCommandQueue), _uiAllocatorFenceValues[fIndex], 0, 0,
                              static_cast<int32_t>(signalResult), 0, static_cast<uint32_t>(fIndex));

            _uiCommandListResetted[fIndex] = false;
        }

        if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();
            XeFGTrace::Record(XeFGTrace::EventType::ScCommandListCloseResult,
                              reinterpret_cast<uint64_t>(_swapChain),
                              reinterpret_cast<uint64_t>(_scCommandList[fIndex]),
                              reinterpret_cast<uint64_t>(_gameCommandQueue), 0, 0, 0,
                              static_cast<int32_t>(closeResult), 0, static_cast<uint32_t>(fIndex));

            if (closeResult == S_OK)
            {
                XeFGTrace::Record(XeFGTrace::EventType::ScExecuteCommandListsBegin,
                                  reinterpret_cast<uint64_t>(_swapChain),
                                  reinterpret_cast<uint64_t>(_gameCommandQueue),
                                  reinterpret_cast<uint64_t>(_scCommandList[fIndex]), 0, 0, 0, 0, 0,
                                  static_cast<uint32_t>(fIndex));
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
                XeFGTrace::Record(XeFGTrace::EventType::ScExecuteCommandListsEnd,
                                  reinterpret_cast<uint64_t>(_swapChain),
                                  reinterpret_cast<uint64_t>(_gameCommandQueue),
                                  reinterpret_cast<uint64_t>(_scCommandList[fIndex]), 0, 0, 0, 0, 0,
                                  static_cast<uint32_t>(fIndex));
            }
            else
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _scCommandListResetted[fIndex] = false;
        }
    }

    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentAfterScWork, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex));

    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentBeforeDispatch, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex));
    const auto dispatchResult = Dispatch();
    XeFGTrace::Record(XeFGTrace::EventType::XeFGPresentAfterDispatch, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(_swapChainContext), reinterpret_cast<uint64_t>(_gameCommandQueue), 0,
                      0, 0, 0, 0, static_cast<uint32_t>(fIndex), dispatchResult ? 1u : 0u);
    return dispatchResult;
}

bool XeFG_Dx12::SetResource(Dx12Resource* inputResource)
{
    XeFGTrace::Record(XeFGTrace::EventType::SetResourceEnter, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(inputResource));
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    XeFGTrace::Record(XeFGTrace::EventType::SetResourceBeforeMutexWait, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(inputResource), reinterpret_cast<uint64_t>(inputResource->resource),
                      static_cast<uint64_t>(fIndex), 0, 0, 0, 0, static_cast<uint32_t>(fIndex),
                      static_cast<uint32_t>(type));
    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
    XeFGTrace::Record(XeFGTrace::EventType::SetResourceAfterMutexAcquire, reinterpret_cast<uint64_t>(_swapChain),
                      reinterpret_cast<uint64_t>(inputResource), reinterpret_cast<uint64_t>(inputResource->resource),
                      static_cast<uint64_t>(fIndex), 0, 0, 0, 0, static_cast<uint32_t>(fIndex),
                      static_cast<uint32_t>(type));

    // This is mostly useful for cases where the user has manually set validity as ValidNow
    if (!inputResource->cmdList && inputResource->validity != FG_ResourceValidity::UntilPresent &&
        inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
    {
        LOG_WARN("XeFG needs cmdList for ValidNow resources, YOLOing");
        inputResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        // Making a copy if it's just valid now to be able to use it later
        if (State::Instance().fgHudlessCompare && inputResource->validity == FG_ResourceValidity::ValidNow)
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;

        if (!_noHudless[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor)
    {
        if (Config::Instance()->FGDisableUI.value_or_default())
            return false;

        // Making a copy if it's just valid now
        if (Config::Instance()->FGDrawUIOverFG.value_or_default() &&
            inputResource->validity == FG_ResourceValidity::ValidNow)
        {
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;
        }

        if (!_noUi[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if (type == FG_ResourceType::Distortion)
    {
        if (!_noDistortionField[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) && _frameResources[fIndex].contains(type))
    {
        return false;
    }

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    if (type == FG_ResourceType::Distortion)
    {
        LOG_TRACE("Distortion field is not supported by XeFG");
        return false;
    }

    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (type == FG_ResourceType::Velocity || type == FG_ResourceType::Depth);

    // Resource flipping
    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    // Depth Invert
    // https://github.com/intel/xess/issues/50
    static auto version = Version();

    // SDK version 2.1.1 fixed this
    if (version < feature_version { 1, 2, 2 } && _device != nullptr && type == FG_ResourceType::Depth &&
        !Config::Instance()->FGXeFGDepthInverted.value_or_default())
    {
        if (_depthInvert.get() == nullptr)
        {
            _depthInvert = std::make_unique<DI_Dx12>("DepthInvert", _device);
        }
        else if (_depthInvert->IsInit())
        {
            if (_depthInvert->CreateBufferResource(_device, fResource->GetResource(), fResource->width,
                                                   fResource->height, fResource->state) &&
                _depthInvert->Buffer() != nullptr)
            {
                auto cmdList = (fResource->cmdList != nullptr) ? fResource->cmdList : GetUICommandList(fIndex);

                _depthInvert->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (_depthInvert->Dispatch(cmdList, fResource->GetResource(), _depthInvert->Buffer()))
                {
                    fResource->copy = _depthInvert->Buffer();
                }

                _depthInvert->SetBufferState(cmdList, fResource->state);
            }
        }
    }

    // We usually don't copy any resources for XeFG, the ones with this tag are the exception
    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        LOG_DEBUG("Making a resource copy of: {}", magic_enum::enum_name(type));

        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        _resourceCopy[fIndex][type]->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;

        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) ||
        (fResource->validity != FG_ResourceValidity::UntilPresent &&
         fResource->validity != FG_ResourceValidity::JustTrackCmdlist))
    {
        fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                                  ? FG_ResourceValidity::UntilPresent
                                  : FG_ResourceValidity::ValidNow;

        if (type == FG_ResourceType::HudlessColor)
        {
            static DXGI_FORMAT lastFormat[BUFFER_COUNT] = {};
            auto desc = fResource->GetResource()->GetDesc();

            if (lastFormat[fIndex] != DXGI_FORMAT_UNKNOWN && lastFormat[fIndex] != desc.Format)
            {
                State::Instance().fgChanged = true;
                return false;
            }

            lastFormat[fIndex] = desc.Format;
        }

        xefg_swapchain_d3d12_resource_data_t resourceParam = GetResourceData(type, fIndex);

        // SDK version 2.1.1 fixes those issues
        if (version < feature_version { 1, 2, 2 })
        {
            // HACK: XeFG docs lie and cmd list is technically required as it checks for it
            // But it doesn't seem to use it when the validity is UNTIL_NEXT_PRESENT
            // https://github.com/intel/xess/issues/45
            if (fResource->cmdList == nullptr && resourceParam.validity == XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT)
                fResource->cmdList = (ID3D12GraphicsCommandList*) 1;

            // HACK: XeFG seems to crash if the resource is in COPY_SOURCE state
            // even though the docs say it's the preferred state
            // https://github.com/intel/xess/issues/47
            if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
            {
                ResourceBarrier(inputResource->cmdList, inputResource->resource, inputResource->state,
                                D3D12_RESOURCE_STATE_COPY_DEST);

                resourceParam.incomingState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
        }

        int indexDiff = GetIndex() - fIndex;
        if (indexDiff < 0)
            indexDiff += BUFFER_COUNT;

        // We will us UI color later with Render UI
        if (type != FG_ResourceType::UIColor ||
            (XeFGProxy::SetUiCompositionState() != nullptr || Config::Instance()->FGDrawUIOverFG.value_or_default()))
        {
            auto frameId = static_cast<uint32_t>(_frameCount - indexDiff);
            XeFGTrace::Record(XeFGTrace::EventType::SetResourceBeforeTagFrameResource,
                              reinterpret_cast<uint64_t>(_swapChainContext),
                              reinterpret_cast<uint64_t>(fResource->cmdList),
                              reinterpret_cast<uint64_t>(resourceParam.pResource), frameId, 0, 0, 0, 0,
                              static_cast<uint32_t>(fIndex),
                              (static_cast<uint32_t>(type) << 16) | static_cast<uint32_t>(fResource->validity));
            auto result =
                XeFGProxy::D3D12TagFrameResource()(_swapChainContext, fResource->cmdList, frameId, &resourceParam);
            XeFGTrace::Record(XeFGTrace::EventType::XeFGTagFrameResourceResult,
                              reinterpret_cast<uint64_t>(_swapChain), reinterpret_cast<uint64_t>(_swapChainContext),
                              reinterpret_cast<uint64_t>(resourceParam.pResource), 0, 0, 0,
                              static_cast<int32_t>(result), 0, frameId);
            if (static_cast<int32_t>(result) < 0)
                XeFGTrace::FlushAfterFailureBestEffort();
            LOG_DEBUG("D3D12TagFrameResource, frameId: {}, type: {} result: {} ({})", frameId,
                      magic_enum::enum_name(type), magic_enum::enum_name(result), (int32_t) result);

            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LogXeFGResult("D3D12TagFrameResource", result);
                State::Instance().fgChanged = true;
                UpdateTarget();
                Deactivate();

                return false;
            }
        }

        // Potentially we don't need to restore but do it just to be safe
        if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            ResourceBarrier(inputResource->cmdList, inputResource->resource, D3D12_RESOURCE_STATE_COPY_DEST,
                            inputResource->state);
        }

        SetResourceReady(type, fIndex);
    }

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());

    return true;
}

void XeFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

bool XeFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock())
    {
        LOG_WARN("[XeFG][Lifecycle] action = release_swapchain_deferred, "
                 "reason = lifecycle_transaction_in_progress");
        return false;
    }

    return ReleaseSwapchainLocked(hwnd);
}

bool XeFG_Dx12::ReleaseSwapchainFromFinalProxyRelease(HWND hwnd, std::function<void()> releaseFinalProxy)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex);

    if (!releaseFinalProxy)
    {
        LOG_ERROR("[XeFG][Lifecycle] action = release_swapchain_aborted, "
                  "reason = missing_final_proxy_release");
        return false;
    }

    auto& state = State::Instance();
    auto* const finalProxy = state.currentFGSwapchain;
    bool finalProxyReleased = false;
    auto releaseFinalProxyOnce = [&]()
    {
        if (finalProxyReleased)
            return;

        if (state.currentSwapchain == finalProxy)
        {
            state.currentSwapchain = nullptr;
        }
        if (state.currentFGSwapchain == finalProxy)
            state.currentFGSwapchain = nullptr;

        LOG_DEBUG("[XeFG][Ownership] action = final_proxy_aliases_cleared, ptr = {:X}", (size_t) finalProxy);
        releaseFinalProxy();
        finalProxyReleased = true;
    };

    const bool releaseSucceeded = ReleaseSwapchainLocked(hwnd, releaseFinalProxyOnce);

    if (!finalProxyReleased)
    {
        // A confirmed final COM release cannot be deferred outside this
        // lifecycle transaction. Consume it while the mutex is still held and
        // quarantine recreation if teardown did not reach the callback.
        _swapchainRecreationBlocked = true;
        releaseFinalProxyOnce();
        LOG_ERROR("[XeFG][Lifecycle] action = final_proxy_release_quarantined, "
                  "reason = teardown_not_completed, hwnd = {:X}, context = {:X}",
                  (size_t) hwnd, (size_t) _swapChainContext);
    }

    return releaseSucceeded;
}

bool XeFG_Dx12::ReleaseSwapchainLocked(HWND hwnd, std::function<void()> releaseFinalProxy)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (_swapchainReleaseInProgress.exchange(true, std::memory_order_acq_rel))
    {
        LOG_WARN("[XeFG][Lifecycle] action = release_swapchain_deferred, "
                 "reason = release_already_in_progress");
        return false;
    }

    _swapchainReleaseOwnerThread.store(GetCurrentThreadId(), std::memory_order_release);

    struct ReleaseGuard
    {
        std::atomic_bool& flag;
        std::atomic<DWORD>& ownerThread;
        ~ReleaseGuard()
        {
            ownerThread.store(0, std::memory_order_release);
            flag.store(false, std::memory_order_release);
        }
    } releaseGuard { _swapchainReleaseInProgress, _swapchainReleaseOwnerThread };

    const bool useConfiguredMutex = Config::Instance()->FGUseMutexForSwapchain.value_or_default();

    if (useConfiguredMutex)
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("[XeFG][Lifecycle] action = release_swapchain_deferred, "
                     "reason = release_already_in_progress");
            return false;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    if (releaseFinalProxy)
    {
        releaseFinalProxy();
        // The proxy object may now be destroyed. Do not access it after this point.
    }

    if (!State::Instance().isShuttingDown)
    {
        if (_swapChainContext != nullptr)
        {
            if (!DestroySwapchainContext())
            {
                LOG_ERROR("[XeFG][Lifecycle] action = release_swapchain_aborted, reason = destroy_failed, "
                          "context = {:X}, swapchain = {:X}",
                          (size_t) _swapChainContext, (size_t) State::Instance().currentFGSwapchain);

                if (useConfiguredMutex)
                {
                    LOG_TRACE("Releasing Mutex after failed destroy: {}", Mutex.getOwner());
                    Mutex.unlockThis(1);
                }

                return false;
            }
        }

        _swapChainContext = nullptr;
        State::Instance().currentFGSwapchain = nullptr;
    }

    ReleaseObjects();

    if (useConfiguredMutex)
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}
