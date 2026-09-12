#include "pch.h"
#include "FG_Hooks.h"
#include <Config.h>

#include <framegen/ffx/FSRFG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

#include <inputs/FG/FSR3_Dx12_FG.h>
#include <inputs/FG/FfxApi_Dx12_FG.h>

#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <misc/FrameLimit.h>

#include <misc/IdentifyGpu.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <d3d12.h>
#include <detours/detours.h>
#include <diagnostics/XeFGTrace.h>

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;
static IUnknown* oldSwapChain = nullptr;
static ID3D12CommandQueue* currentCommandQueue = nullptr;
static bool _forcedHdrForXeFG = false;
static HANDLE _semaphore = nullptr;

uint32_t FGHooks::TraceFlags()
{
    uint32_t flags = 0;
    if (_skipResize)
        flags |= XeFGTrace::SkipResize;
    if (_skipResize1)
        flags |= XeFGTrace::SkipResize1;
    if (_skipPresent)
        flags |= XeFGTrace::SkipPresent;
    if (_skipPresent1)
        flags |= XeFGTrace::SkipPresent1;

    auto* fg = State::Instance().currentFG;
    if (fg != nullptr)
    {
        flags |= XeFGTrace::XeFGActive * static_cast<uint32_t>(fg->IsActive());
        flags |= XeFGTrace::XeFGPaused * static_cast<uint32_t>(fg->IsPaused());
    }
    return flags;
}

static bool CheckForFGStatus()
{
    // Need to check overlay menu parameter, goes to places it shouldn't go
    // if (!Config::Instance()->OverlayMenu.value_or_default())
    //    return false;

    if (State::Instance().activeFgInput == FGInput::NoFG || State::Instance().activeFgInput == FGInput::NvngxFG)
        return false;

    // Disable FG if amd dll is not found
    if (State::Instance().activeFgOutput == FGOutput::FSRFG)
    {
        FfxApiProxy::InitFfxDx12();
        if (!FfxApiProxy::IsFGReady())
        {
            ImGui::InsertNotification(
                { ImGuiToastType::Error, 20000, "Can't init FSR FG\nAre you missing the required DLLs?" });

            LOG_DEBUG("Can't init FfxApiProxy, disabling FGOutput");
            Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
            State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        }
    }
    else if (State::Instance().activeFgOutput == FGOutput::XeFG && !XeFGProxy::InitXeFG())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init XeFG\nAre you missing the required DLLs?" });

        LOG_DEBUG("Can't init XeFGProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
    }
    else if (State::Instance().activeFgOutput == FGOutput::DLSSG && !StreamlineProxy::LoadStreamline())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init DLSSG Output\nAre you missing the streamline folder?" });

        LOG_DEBUG("Can't init StreamlineProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();

        Config::Instance()->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::None);
        State::Instance().activeFgNvngx = Config::Instance()->FGNvngxReplacement.value_or_default();
    }

    if (State::Instance().activeFgOutput == FGOutput::NoFG)
    {
        LOG_WARN("FGOutput doesn't have any FG active");
        return false;
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
        XeFGTrace::Initialize();

    return true;
}

HRESULT FGHooks::CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                 IDXGISwapChain** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    IUnknown* previousFGSwapchain = State::Instance().currentFGSwapchain;
    bool scResult = false;

    XeFGTrace::Record(XeFGTrace::EventType::SwapchainCreateBegin, reinterpret_cast<uint64_t>(previousFGSwapchain),
                      reinterpret_cast<uint64_t>(pDevice), reinterpret_cast<uint64_t>(pFactory), 0, 0, 0, 0,
                      TraceFlags());

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && !pDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (previousFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

                XeFGTrace::Record(XeFGTrace::EventType::FenceSignalBegin,
                                  reinterpret_cast<uint64_t>(previousFGSwapchain),
                                  reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                                  reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, 0, TraceFlags());
                resizeFenceValue++;
                HRESULT signalResult = State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);
                XeFGTrace::Record(XeFGTrace::EventType::FenceSignalEnd,
                                  reinterpret_cast<uint64_t>(previousFGSwapchain),
                                  reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                                  reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, signalResult,
                                  TraceFlags());
                if (FAILED(signalResult))
                    XeFGTrace::FlushAfterFailureBestEffort();

                if (resizeFence->GetCompletedValue() < resizeFenceValue)
                {
                    XeFGTrace::Record(XeFGTrace::EventType::FenceWaitBegin,
                                      reinterpret_cast<uint64_t>(previousFGSwapchain),
                                      reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0, 0,
                                      TraceFlags());
                    HRESULT eventResult = resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
                    // Max 5 sec
                    auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
                    XeFGTrace::Record(XeFGTrace::EventType::FenceWaitEnd,
                                      reinterpret_cast<uint64_t>(previousFGSwapchain),
                                      reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0,
                                      static_cast<int32_t>(eventResult), TraceFlags(),
                                      static_cast<uint32_t>(waitResult));
                    if (FAILED(eventResult))
                        XeFGTrace::FlushAfterFailureBestEffort();
                    LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
                }
            }
        }

        if (oldSwapChain == previousFGSwapchain)
            oldSwapChain = nullptr;

        scResult = fg->CreateSwapchain(pFactory, cq, pDesc, ppSwapChain, true);

        XeFGTrace::Record(XeFGTrace::EventType::SwapchainCreateEnd, reinterpret_cast<uint64_t>(previousFGSwapchain),
                          reinterpret_cast<uint64_t>(ppSwapChain != nullptr ? *ppSwapChain : nullptr),
                          reinterpret_cast<uint64_t>(pFactory), 0, 0, 0, scResult ? S_OK : E_FAIL, TraceFlags());

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        IUnknown* newFGSwapchain = ppSwapChain != nullptr ? static_cast<IUnknown*>(*ppSwapChain) : nullptr;
        if (previousFGSwapchain != nullptr && previousFGSwapchain != newFGSwapchain)
            oldSwapChain = previousFGSwapchain;
        else if (oldSwapChain == newFGSwapchain)
            oldSwapChain = nullptr;

        if (State::Instance().currentD3D12Device != nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::FenceRecreateBegin, reinterpret_cast<uint64_t>(*ppSwapChain),
                              reinterpret_cast<uint64_t>(State::Instance().currentD3D12Device),
                              reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, 0, TraceFlags());
            SAFE_RELEASE(resizeFence);
            SAFE_CLOSE_HANDLE(resizeFenceEvent);

            HRESULT fenceResult =
                State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            XeFGTrace::Record(XeFGTrace::EventType::FenceRecreateEnd, reinterpret_cast<uint64_t>(*ppSwapChain),
                              reinterpret_cast<uint64_t>(resizeFence), reinterpret_cast<uint64_t>(resizeFenceEvent),
                              resizeFenceValue, 0, 0, fenceResult, TraceFlags());
            if (FAILED(fenceResult))
                XeFGTrace::FlushAfterFailureBestEffort();
        }

        SetFGSwapchain(*ppSwapChain, pDesc->OutputWindow);

        return S_OK;
    }

    return E_INVALIDARG;
}

HRESULT FGHooks::CreateSwapChainForHwnd(IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
                                        DXGI_SWAP_CHAIN_DESC1* pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    IUnknown* previousFGSwapchain = State::Instance().currentFGSwapchain;
    bool scResult = false;

    XeFGTrace::Record(XeFGTrace::EventType::SwapchainCreate1Begin, reinterpret_cast<uint64_t>(previousFGSwapchain),
                      reinterpret_cast<uint64_t>(pDevice), reinterpret_cast<uint64_t>(pFactory), 0, 0, 0, 0,
                      TraceFlags());

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && pFullscreenDesc != nullptr &&
            !pFullscreenDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (previousFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

                XeFGTrace::Record(XeFGTrace::EventType::FenceSignalBegin,
                                  reinterpret_cast<uint64_t>(previousFGSwapchain),
                                  reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                                  reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, 0, TraceFlags());
                resizeFenceValue++;
                HRESULT signalResult = State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);
                XeFGTrace::Record(XeFGTrace::EventType::FenceSignalEnd,
                                  reinterpret_cast<uint64_t>(previousFGSwapchain),
                                  reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                                  reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, signalResult,
                                  TraceFlags());
                if (FAILED(signalResult))
                    XeFGTrace::FlushAfterFailureBestEffort();

                if (resizeFence->GetCompletedValue() < resizeFenceValue)
                {
                    XeFGTrace::Record(XeFGTrace::EventType::FenceWaitBegin,
                                      reinterpret_cast<uint64_t>(previousFGSwapchain),
                                      reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0, 0,
                                      TraceFlags());
                    HRESULT eventResult = resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
                    // Max 5 sec
                    auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
                    XeFGTrace::Record(XeFGTrace::EventType::FenceWaitEnd,
                                      reinterpret_cast<uint64_t>(previousFGSwapchain),
                                      reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0,
                                      static_cast<int32_t>(eventResult), TraceFlags(),
                                      static_cast<uint32_t>(waitResult));
                    if (FAILED(eventResult))
                        XeFGTrace::FlushAfterFailureBestEffort();
                    LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
                }
            }
        }

        if (oldSwapChain == previousFGSwapchain)
            oldSwapChain = nullptr;

        scResult = fg->CreateSwapchain1(pFactory, cq, hWnd, pDesc, pFullscreenDesc, ppSwapChain, true);

        XeFGTrace::Record(XeFGTrace::EventType::SwapchainCreate1End,
                          reinterpret_cast<uint64_t>(previousFGSwapchain),
                          reinterpret_cast<uint64_t>(ppSwapChain != nullptr ? *ppSwapChain : nullptr),
                          reinterpret_cast<uint64_t>(pFactory), 0, 0, 0, scResult ? S_OK : E_FAIL, TraceFlags());

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        IUnknown* newFGSwapchain = ppSwapChain != nullptr ? static_cast<IUnknown*>(*ppSwapChain) : nullptr;
        if (previousFGSwapchain != nullptr && previousFGSwapchain != newFGSwapchain)
            oldSwapChain = previousFGSwapchain;
        else if (oldSwapChain == newFGSwapchain)
            oldSwapChain = nullptr;

        if (State::Instance().currentD3D12Device != nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::FenceRecreateBegin, reinterpret_cast<uint64_t>(*ppSwapChain),
                              reinterpret_cast<uint64_t>(State::Instance().currentD3D12Device),
                              reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, 0, TraceFlags());
            SAFE_RELEASE(resizeFence);
            SAFE_CLOSE_HANDLE(resizeFenceEvent);

            HRESULT fenceResult =
                State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            XeFGTrace::Record(XeFGTrace::EventType::FenceRecreateEnd, reinterpret_cast<uint64_t>(*ppSwapChain),
                              reinterpret_cast<uint64_t>(resizeFence), reinterpret_cast<uint64_t>(resizeFenceEvent),
                              resizeFenceValue, 0, 0, fenceResult, TraceFlags());
            if (FAILED(fenceResult))
                XeFGTrace::FlushAfterFailureBestEffort();
        }

        SetFGSwapchain((IDXGISwapChain*) *ppSwapChain, hWnd);

        return S_OK;
    }

    return E_INVALIDARG;
}

void FGHooks::SetFGSwapchain(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _hwnd = hWnd;

    if (_dx12InteropPresentSC == pSwapChain)
    {
        _dx12InteropPresentSC = nullptr;
        _dx12InteropPresentHwnd = nullptr;
    }

    State::Instance().currentFGSwapchain = pSwapChain;
    State::Instance().currentSwapchain = pSwapChain;

    HookFGSwapchain(pSwapChain);
}

void FGHooks::SetDx12InteropPresentSC(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _dx12InteropPresentSC = pSwapChain;
    _dx12InteropPresentHwnd = hWnd;

    LOG_INFO("Set DX12 presenting swapchain: {:X}, hWnd: {:X}", (size_t) pSwapChain, (size_t) hWnd);
}

void FGHooks::ClearDx12InteropPresentSC(IUnknown* pSwapChain)
{
    if (pSwapChain == nullptr || pSwapChain != _dx12InteropPresentSC)
        return;

    LOG_INFO("Clearing DX12 presenting swapchain: {:X}", (size_t) pSwapChain);

    _dx12InteropPresentSC = nullptr;
    _dx12InteropPresentHwnd = nullptr;
}

bool FGHooks::IsDx12InteropPresentSC(IUnknown* pSwapChain)
{
    return pSwapChain != nullptr && pSwapChain == _dx12InteropPresentSC;
}

void FGHooks::HookFGSwapchain(IDXGISwapChain* pSwapChain)
{
    if (o_FGSCPresent != nullptr || pSwapChain == nullptr)
        return;

    void** pFactoryVTable = *reinterpret_cast<void***>(pSwapChain);

    o_FGRelease = (PFN_Release) pFactoryVTable[2];
    o_FGSCPresent = (PFN_Present) pFactoryVTable[8];
    o_FGSCSetFullscreenState = (PFN_SetFullscreenState) pFactoryVTable[10];
    o_FGSCGetFullscreenState = (PFN_GetFullscreenState) pFactoryVTable[11];
    o_FGSCResizeBuffers = (PFN_ResizeBuffers) pFactoryVTable[13];
    o_FGSCResizeTarget = (PFN_ResizeTarget) pFactoryVTable[14];
    o_FGSCGetFullscreenDesc = (PFN_GetFullscreenDesc) pFactoryVTable[19];
    o_FGSCPresent1 = (PFN_Present1) pFactoryVTable[22];
    o_FGSCGetFrameLatencyWaitableObject = (PFN_GetFrameLatencyWaitableObject) pFactoryVTable[33];
    o_FGSCResizeBuffers1 = (PFN_ResizeBuffers1) pFactoryVTable[39];

    if (o_FGSCPresent != nullptr)
    {
        LOG_INFO("Hooking FG SwapChain present");
        LOG_TRACE("FGRelease: {:X}", (size_t) o_FGRelease);
        LOG_TRACE("FGSCPresent: {:X}", (size_t) o_FGSCPresent);
        LOG_TRACE("FGSCSetFullscreenState: {:X}", (size_t) o_FGSCSetFullscreenState);
        LOG_TRACE("FGSCGetFullscreenState: {:X}", (size_t) o_FGSCGetFullscreenState);
        LOG_TRACE("FGSCResizeBuffers: {:X}", (size_t) o_FGSCResizeBuffers);
        LOG_TRACE("FGSCResizeTarget: {:X}", (size_t) o_FGSCResizeTarget);
        LOG_TRACE("FGSCGetFullscreenDesc: {:X}", (size_t) o_FGSCGetFullscreenDesc);
        LOG_TRACE("FGSCPresent1: {:X}", (size_t) o_FGSCPresent1);
        LOG_TRACE("FGSCResizeBuffers1: {:X}", (size_t) o_FGSCResizeBuffers1);
        LOG_TRACE("FGSCGetFrameLatencyWaitableObject: {:X}", (size_t) o_FGSCGetFrameLatencyWaitableObject);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_FGRelease, hkFGRelease);
        DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent);
        DetourAttach(&(PVOID&) o_FGSCResizeTarget, hkResizeTarget);
        DetourAttach(&(PVOID&) o_FGSCResizeBuffers, hkResizeBuffers);
        DetourAttach(&(PVOID&) o_FGSCSetFullscreenState, hkSetFullscreenState);

        if (o_FGSCPresent1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCPresent1, hkFGPresent1);

        if (o_FGSCResizeBuffers1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCResizeBuffers1, hkResizeBuffers1);

        if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            if (o_FGSCGetFullscreenState != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenState, hkGetFullscreenState);

            if (o_FGSCGetFullscreenDesc != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenDesc, hkGetFullscreenDesc);

            if ((Config::Instance()->SimulateWaitableObject.value_or_default() ||
                 (State::Instance().gameEngine == GameEngineType::Unity &&
                  State::Instance().activeFgOutput == FGOutput::XeFG)) &&
                o_FGSCGetFrameLatencyWaitableObject != nullptr)
            {
                DetourAttach(&(PVOID&) o_FGSCGetFrameLatencyWaitableObject, hkGetFrameLatencyWaitableObject);
            }
        }

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to attach detour: {:X}", detourResult);
            o_FGRelease = nullptr;
            o_FGSCPresent = nullptr;
            o_FGSCSetFullscreenState = nullptr;
            o_FGSCGetFullscreenState = nullptr;
            o_FGSCResizeBuffers = nullptr;
            o_FGSCResizeTarget = nullptr;
            o_FGSCGetFullscreenDesc = nullptr;
            o_FGSCPresent1 = nullptr;
            o_FGSCResizeBuffers1 = nullptr;
            o_FGSCGetFrameLatencyWaitableObject = nullptr;
        }
    }
}

HRESULT FGHooks::hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    bool modeChanged = false;
    bool orgFS = Fullscreen;
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (Fullscreen)
        {
            Fullscreen = false;

            if (!State::Instance().SCExclusiveFullscreen)
            {
                State::Instance().SCExclusiveFullscreen = true;
                modeChanged = true;
            }

            LOG_DEBUG("Prevented exclusive fullscreen");
        }
        else
        {
            if (State::Instance().SCExclusiveFullscreen)
            {
                modeChanged = true;
                State::Instance().SCExclusiveFullscreen = false;
            }
        }
    }

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (State::Instance().activeFgOutput == FGOutput::XeFG && Fullscreen)
        LOG_WARN("Using exclusive fullscreen with XeFG!!!");

    auto result = S_OK;

    if (!Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        result = o_FGSCSetFullscreenState(This, Fullscreen, pTarget);
        LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Result: {:X}", Fullscreen, (size_t) pTarget, (UINT) result);
    }

    if (result == S_OK && modeChanged)
    {
        LOG_DEBUG("Mode changed");

        DXGI_SWAP_CHAIN_DESC scDesc {};
        This->GetDesc(&scDesc);

        if (State::Instance().SCExclusiveFullscreen)
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

            Util::MonitorInfo info;

            if (pTarget != nullptr)
                info = Util::GetMonitorInfoForOutput(pTarget);
            else
                info = Util::GetMonitorInfoForWindow(_hwnd);

            LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                      info.y, wstring_to_string(info.name));

            SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_OVERLAPPEDWINDOW);
            SetWindowPos(_hwnd, nullptr, 0, 0, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    return result;
}

HANDLE FGHooks::hkGetFrameLatencyWaitableObject(IDXGISwapChain2* This)
{
    if (State::Instance().activeFgOutput != FGOutput::XeFG)
        return o_FGSCGetFrameLatencyWaitableObject(This);

    if (_semaphore == nullptr)
    {
        _semaphore = CreateSemaphore(nullptr, 16, 16, nullptr);

        if (_semaphore == nullptr)
            return nullptr;
    }

    HANDLE duplicatedHandle = nullptr;

    if (!DuplicateHandle(GetCurrentProcess(), _semaphore, GetCurrentProcess(), &duplicatedHandle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        return nullptr;
    }

    return duplicatedHandle;
}

HRESULT FGHooks::hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    auto result = o_FGSCGetFullscreenDesc(This, pDesc);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        pDesc->Windowed = false;
    }

    return result;
}

HRESULT FGHooks::hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    auto result = o_FGSCGetFullscreenState(This, pFullscreen, ppTarget);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        *pFullscreen = true;
    }

    return result;
}

HRESULT FGHooks::hkResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                 UINT SwapChainFlags)
{
    XeFGTrace::Record(XeFGTrace::EventType::ResizeEnter, reinterpret_cast<uint64_t>(This), 0, 0, resizeFenceValue,
                      0, 0, 0, TraceFlags(), BufferCount, Width);

    // Skip XeFG's internal call
    if (_skipResize)
    {
        LOG_DEBUG("XeFG call skipping");
        XeFGTrace::Record(XeFGTrace::EventType::ResizeInternalBypass, reinterpret_cast<uint64_t>(This), 0, 0,
                          resizeFenceValue, 0, 0, 0, TraceFlags());
        _skipResize = false;

        IDXGISwapChain* sc = nullptr;

        if (sc != nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::ResizeDxgiBegin, reinterpret_cast<uint64_t>(This));
            auto result = sc->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
            XeFGTrace::Record(XeFGTrace::EventType::ResizeDxgiEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                              result, TraceFlags());
            LOG_DEBUG("XeFG internal ResizeBuffers result: {:X}", (UINT) result);
            if (FAILED(result))
                XeFGTrace::FlushAfterFailureBestEffort();
            return result;
        }

        XeFGTrace::Record(XeFGTrace::EventType::ResizeDxgiBegin, reinterpret_cast<uint64_t>(This));
        auto result = o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        XeFGTrace::Record(XeFGTrace::EventType::ResizeDxgiEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                          result, TraceFlags());
        if (FAILED(result))
            XeFGTrace::FlushAfterFailureBestEffort();
        return result;
    }

    if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        XeFGTrace::Record(XeFGTrace::EventType::FenceSignalBegin, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                          reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, 0, TraceFlags());
        resizeFenceValue++;
        HRESULT signalResult = State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);
        XeFGTrace::Record(XeFGTrace::EventType::FenceSignalEnd, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                          reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, signalResult, TraceFlags());
        if (FAILED(signalResult))
            XeFGTrace::FlushAfterFailureBestEffort();

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            XeFGTrace::Record(XeFGTrace::EventType::FenceWaitBegin, reinterpret_cast<uint64_t>(This),
                              reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0, 0,
                              TraceFlags());
            HRESULT eventResult = resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            XeFGTrace::Record(XeFGTrace::EventType::FenceWaitEnd, reinterpret_cast<uint64_t>(This),
                              reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0,
                              static_cast<int32_t>(eventResult), TraceFlags(), static_cast<uint32_t>(waitResult));
            if (FAILED(eventResult))
                XeFGTrace::FlushAfterFailureBestEffort();
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    auto fgDx12 = State::Instance().currentFG;
    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (NewFormat == desc.BufferDesc.Format || NewFormat == 0) &&
                State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();

                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // Docs say you cannot do that
        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);

    _skipResize1 = true;

    // Backbuffer release probing is disabled for the MHW resize investigation.

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        XeFGTrace::Record(XeFGTrace::EventType::ResizeDxgiBegin, reinterpret_cast<uint64_t>(This), 0, 0,
                          resizeFenceValue, 0, 0, 0, TraceFlags());
        result = o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        XeFGTrace::Record(XeFGTrace::EventType::ResizeDxgiEnd, reinterpret_cast<uint64_t>(This), 0, 0,
                          resizeFenceValue, 0, 0, result, TraceFlags());
    }

    _skipResize1 = false;

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    return result;
}

HRESULT FGHooks::hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        LOG_DEBUG("Skipping resize target.");
        return S_OK;
    }

    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    return o_FGSCResizeTarget(This, pNewTargetParameters);
}

HRESULT FGHooks::hkResizeBuffers1(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                  UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    XeFGTrace::Record(XeFGTrace::EventType::Resize1Enter, reinterpret_cast<uint64_t>(This), 0, 0, resizeFenceValue,
                      0, 0, 0, TraceFlags(), BufferCount, Width);

    // Skip XeFG's internal call
    if (_skipResize1)
    {
        LOG_DEBUG("XeFG call skipping");
        XeFGTrace::Record(XeFGTrace::EventType::Resize1InternalBypass, reinterpret_cast<uint64_t>(This), 0, 0,
                          resizeFenceValue, 0, 0, 0, TraceFlags());
        _skipResize1 = false;

        IDXGISwapChain3* sc = nullptr;

        if (sc != nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::Resize1DxgiBegin, reinterpret_cast<uint64_t>(This));
            auto result = sc->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                             ppPresentQueue);
            XeFGTrace::Record(XeFGTrace::EventType::Resize1DxgiEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                              result, TraceFlags());

            LOG_DEBUG("XeFG internal ResizeBuffers1 result: {:X}", (UINT) result);
            return result;
        }

        XeFGTrace::Record(XeFGTrace::EventType::Resize1DxgiBegin, reinterpret_cast<uint64_t>(This));
        auto result = o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags,
                                           pCreationNodeMask, ppPresentQueue);
        XeFGTrace::Record(XeFGTrace::EventType::Resize1DxgiEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                          result, TraceFlags());
        if (FAILED(result))
            XeFGTrace::FlushAfterFailureBestEffort();
        return result;
    }

    if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        XeFGTrace::Record(XeFGTrace::EventType::FenceSignalBegin, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                          reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, 0, TraceFlags());
        resizeFenceValue++;
        HRESULT signalResult = State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);
        XeFGTrace::Record(XeFGTrace::EventType::FenceSignalEnd, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(State::Instance().currentCommandQueue),
                          reinterpret_cast<uint64_t>(resizeFence), resizeFenceValue, 0, 0, signalResult, TraceFlags());

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            XeFGTrace::Record(XeFGTrace::EventType::FenceWaitBegin, reinterpret_cast<uint64_t>(This),
                              reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0, 0,
                              TraceFlags());
            HRESULT eventResult = resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            XeFGTrace::Record(XeFGTrace::EventType::FenceWaitEnd, reinterpret_cast<uint64_t>(This),
                              reinterpret_cast<uint64_t>(resizeFenceEvent), 0, resizeFenceValue, 0, 0,
                              static_cast<int32_t>(eventResult), TraceFlags(), static_cast<uint32_t>(waitResult));
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}, Caller: {}", BufferCount,
              Width, Height, (UINT) Format, SwapChainFlags, Util::WhoIsTheCaller(_ReturnAddress()));

    auto fgDx12 = State::Instance().currentFG;
    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (Format == desc.BufferDesc.Format || Format == 0) && State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();
                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);

    // Backbuffer release probing is disabled for the MHW resize investigation.

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        _skipResize = true;

        XeFGTrace::Record(XeFGTrace::EventType::Resize1DxgiBegin, reinterpret_cast<uint64_t>(This), 0, 0,
                          resizeFenceValue, 0, 0, 0, TraceFlags());
        result = o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);
        XeFGTrace::Record(XeFGTrace::EventType::Resize1DxgiEnd, reinterpret_cast<uint64_t>(This), 0, 0,
                          resizeFenceValue, 0, 0, result, TraceFlags());

        _skipResize = false;
    }

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    return result;
}

HRESULT FGHooks::hkFGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    XeFGTrace::Record(XeFGTrace::EventType::PresentEnter, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0, 0,
                      TraceFlags(), SyncInterval, Flags);

    // Skip XeFG's internal call
    if (_skipPresent)
    {
        LOG_DEBUG("XeFG call skipping");
        XeFGTrace::Record(XeFGTrace::EventType::PresentInternalBypass, reinterpret_cast<uint64_t>(This), 0, 0, 0,
                          0, 0, 0, TraceFlags(), SyncInterval, Flags);

        IDXGISwapChain* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentBegin, reinterpret_cast<uint64_t>(This));
            result = sc->Present(SyncInterval, Flags);
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                              result, TraceFlags());
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentBegin, reinterpret_cast<uint64_t>(This));
            result = o_FGSCPresent(This, SyncInterval, Flags);
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                              result, TraceFlags());
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        if (FAILED(result))
            XeFGTrace::FlushAfterFailureBestEffort();
        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);

    XeFGTrace::Record(XeFGTrace::EventType::PresentDispatchBegin, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                      0, TraceFlags());
    _skipPresent1 = true;
    auto result = FGPresent(This, SyncInterval, Flags, nullptr);
    _skipPresent1 = false;
    XeFGTrace::Record(XeFGTrace::EventType::PresentDispatchEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                      result, TraceFlags());

    return result;
}

HRESULT FGHooks::hkFGPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags,
                              const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    XeFGTrace::Record(XeFGTrace::EventType::Present1Enter, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0, 0,
                      TraceFlags(), SyncInterval, Flags);

    // Skip XeFG's internal call
    if (_skipPresent1)
    {
        LOG_DEBUG("XeFG call skipping");
        XeFGTrace::Record(XeFGTrace::EventType::Present1InternalBypass, reinterpret_cast<uint64_t>(This), 0, 0, 0,
                          0, 0, 0, TraceFlags(), SyncInterval, Flags);

        IDXGISwapChain3* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1Begin, reinterpret_cast<uint64_t>(This));
            result = sc->Present1(SyncInterval, Flags, pPresentParameters);
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1End, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0,
                              0, result, TraceFlags());
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1Begin, reinterpret_cast<uint64_t>(This));
            result = o_FGSCPresent1(This, SyncInterval, Flags, pPresentParameters);
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1End, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0,
                              0, result, TraceFlags());
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        if (FAILED(result))
            XeFGTrace::FlushAfterFailureBestEffort();
        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    XeFGTrace::Record(XeFGTrace::EventType::PresentDispatchBegin, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                      0, TraceFlags());
    _skipPresent = true;
    auto result = FGPresent(This, SyncInterval, Flags, pPresentParameters);
    _skipPresent = false;
    XeFGTrace::Record(XeFGTrace::EventType::PresentDispatchEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                      result, TraceFlags());

    return result;
}

HRESULT FGHooks::FGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags,
                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    _lastPresentFlags = Flags;

    auto& state = State::Instance();
    auto config = Config::Instance();

    if (state.isShuttingDown)
    {
        HRESULT shutdownResult;
        if (pPresentParameters == nullptr)
        {
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentBegin, reinterpret_cast<uint64_t>(This));
            shutdownResult = o_FGSCPresent(This, SyncInterval, Flags);
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                              shutdownResult, TraceFlags());
        }
        else
        {
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1Begin, reinterpret_cast<uint64_t>(This));
            shutdownResult = o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);
            XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1End, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0,
                              0, shutdownResult, TraceFlags());
        }
        return shutdownResult;
    }

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;

    if (willPresent)
    {
        state.fgLastFrame++;

        double ftDelta = 0.0f;
        auto now = Util::MillisecondsNow();

        if (_lastFGFrameTime > 0.0)
            ftDelta = now - _lastFGFrameTime;

        _lastFGFrameTime = now;
        state.lastFGFrameTime = ftDelta;

        LOG_DEBUG("flags: {:X}, Frametime: {}", Flags, ftDelta);

#ifdef LOW_LATENCY_INPUTS
        IUnknown* device = state.currentD3D12Device ? state.currentD3D12Device : (IUnknown*) state.currentD3D11Device;
        InputCommon::mark_present_start(device);
#endif
    }

    IFGFeature* fg = state.currentFG;

    if (fg != nullptr && willPresent && fg->IsActive() && !fg->IsPaused())
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};

            if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 && state.currentD3D11Device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                state.currentD3D11Device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context); upscalerTimeOpt.has_value())
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }
            else if (state.swapchainInteropApi == SwapchainInteropApi::None && state.currentCommandQueue != nullptr)
            {
                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(state.currentCommandQueue);
                    upscalerTimeOpt.has_value())
                {
                    currentFeature->ReadDetailedGpuTimes(state.currentCommandQueue, state.detailedGpuTimes);
                }
            }

            if (upscalerTimeOpt.has_value())
            {
                auto upscalerTime = upscalerTimeOpt.value();
                // filter out possibly wrong measured high values
                if (upscalerTime < 100.0)
                {
                    State::Instance().frameTimeMutex.lock();
                    State::Instance().upscaleTimes.push_back(upscalerTime);
                    State::Instance().upscaleTimes.pop_front();
                    State::Instance().frameTimeMutex.unlock();
                }
            }
        }
    }

    bool mutexUsed = false;
    const bool mutexCandidate = willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
                                config->FGUseMutexForSwapchain.value_or_default();
    const bool mutexOwnerCurrent = mutexCandidate && fg->Mutex.isOwnedByCurrentThread(2);
    XeFGTrace::Record(XeFGTrace::EventType::PresentMutexDecision, reinterpret_cast<uint64_t>(This),
                      reinterpret_cast<uint64_t>(fg), 0, 0, fg != nullptr ? fg->Mutex.getOwner() : 0,
                      fg != nullptr ? fg->Mutex.getOwnerThread() : 0, mutexCandidate ? 1 : 0, TraceFlags());
    if (mutexCandidate && !mutexOwnerCurrent)
    {
        LOG_TRACE("Waiting FG->Mutex 2, current: {}", fg->Mutex.getOwner());
        XeFGTrace::Record(XeFGTrace::EventType::PresentMutexWaitBegin, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(fg), 0, 0, fg->Mutex.getOwner(), fg->Mutex.getOwnerThread(), 0,
                          TraceFlags());
        fg->Mutex.lock(2);
        mutexUsed = true;
        XeFGTrace::Record(XeFGTrace::EventType::PresentMutexAcquired, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(fg), 0, 0, fg->Mutex.getOwner(), fg->Mutex.getOwnerThread(), 0,
                          TraceFlags());
        LOG_TRACE("Accuired FG->Mutex: {}", fg->Mutex.getOwner());
    }
    else if (mutexOwnerCurrent)
    {
        XeFGTrace::Record(XeFGTrace::EventType::PresentMutexSameThreadBypass, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(fg), 0, 0, fg->Mutex.getOwner(), fg->Mutex.getOwnerThread(), 0,
                          TraceFlags());
    }

    const bool fgFeatureActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

    sl::FrameToken* localToken = nullptr;
    sl::Result tokenResult = sl::Result::eErrorReflexAPI;
    if (willPresent && fg != nullptr && !fgFeatureActive)
        state.dlssgDetectedInterpolationCount = 0;

    if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::DLSSG)
    {
        if ((!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()))
        {
            if (StreamlineProxy::PCLSetMarker() != nullptr)
            {
                ((IDXGISwapChain4*) This)->GetCurrentBackBufferIndex();
                const uint32_t frameId = (uint32_t) fg->FrameCount();
                tokenResult = StreamlineProxy::GetNewFrameToken()(localToken, &frameId);

                if (tokenResult == sl::Result::eOk)
                    StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentStart, *localToken);
            }
        }
    }

    if (willPresent && fgFeatureActive)
    {
        if (state.activeFgInput == FGInput::FSRFG)
        {
            XeFGTrace::Record(XeFGTrace::EventType::FSRFGPresentCallbackEnter,
                              reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(fg),
                              reinterpret_cast<uint64_t>(state.currentFGSwapchain), fg->FrameCount(), 0, 0, 0,
                              TraceFlags(), 1);
            ffxPresentCallback();
            XeFGTrace::Record(XeFGTrace::EventType::FSRFGPresentCallbackExit,
                              reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(fg),
                              reinterpret_cast<uint64_t>(state.currentFGSwapchain), fg->FrameCount(), 0, 0, 0,
                              TraceFlags(), 1);
        }
        else if (state.activeFgInput == FGInput::FSRFG30)
        {
            XeFGTrace::Record(XeFGTrace::EventType::FSRFGPresentCallbackEnter,
                              reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(fg),
                              reinterpret_cast<uint64_t>(state.currentFGSwapchain), fg->FrameCount(), 0, 0, 0,
                              TraceFlags(), 2);
            FSR3FG::ffxPresentCallback();
            XeFGTrace::Record(XeFGTrace::EventType::FSRFGPresentCallbackExit,
                              reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(fg),
                              reinterpret_cast<uint64_t>(state.currentFGSwapchain), fg->FrameCount(), 0, 0, 0,
                              TraceFlags(), 2);
        }

        fg->Present();
    }
    else if (willPresent && fg != nullptr)
    {
        LOG_TRACE("FGHooks::FGPresent: FG feature exists but is inactive/paused; pass-through present only");
    }

    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None)
    {
        ResTrack_Dx12::ClearPossibleHudless();
        Hudfix_Dx12::PresentStart();
    }

    if (willPresent && config->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  config->ForceVsync.value(), config->VsyncInterval.value_or_default(), state.SCAllowTearing,
                  state.realExclusiveFullscreen);

        if (!config->ForceVsync.value())
        {
            SyncInterval = 0;

            if (state.SCAllowTearing && !state.realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            SyncInterval = config->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // Used at wrapped_swapchain LocalPresent to determine is frame is interpolated or not
    if (willPresent)
        state.fgPresentIsCalled = true;

    HRESULT result;
    if (pPresentParameters == nullptr)
    {
        XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentBegin, reinterpret_cast<uint64_t>(This));
        result = o_FGSCPresent(This, SyncInterval, Flags);
        XeFGTrace::Record(XeFGTrace::EventType::DxgiPresentEnd, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                          result, TraceFlags());
    }
    else
    {
        XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1Begin, reinterpret_cast<uint64_t>(This));
        result = o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);
        XeFGTrace::Record(XeFGTrace::EventType::DxgiPresent1End, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                          result, TraceFlags());
    }

    if (FAILED(result))
        XeFGTrace::FlushAfterFailureBestEffort();

    if (result == S_OK)
    {
        LOG_DEBUG("Result: {:X}", result);
    }
    else
    {
        if (result == DXGI_ERROR_DEVICE_REMOVED && state.currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(state.currentD3D12Device);
    }

    if (tokenResult == sl::Result::eOk && localToken != nullptr && fgFeatureActive &&
        (!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()) &&
        willPresent && state.activeFgOutput == FGOutput::DLSSG)
    {
        if (StreamlineProxy::PCLSetMarker() != nullptr)
            StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentEnd, *localToken);

        LOG_DEBUG("Calling ReflexSleep");
        StreamlineProxy::ReflexSleep()(*localToken);
    }

    if (state.swapchainInteropApi == SwapchainInteropApi::None)
        Hudfix_Dx12::PresentEnd();

    if (willPresent && !state.reflexLimitsFps && state.activeFgOutput != FGOutput::NoFG &&
        !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
    {
        FrameLimit::sleep(fg != nullptr ? fg->IsActive() && !fg->IsPaused() : false);
    }

    if ((config->SimulateWaitableObject.value_or_default() ||
         (state.gameEngine == GameEngineType::Unity && state.activeFgOutput == FGOutput::XeFG)) &&
        _semaphore != nullptr)
    {
        ReleaseSemaphore(_semaphore, 1, nullptr);
    }

    if (mutexUsed && fg != nullptr)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", fg->Mutex.getOwner());
        XeFGTrace::Record(XeFGTrace::EventType::PresentMutexUnlock, reinterpret_cast<uint64_t>(This),
                          reinterpret_cast<uint64_t>(fg), 0, 0, fg->Mutex.getOwner(), fg->Mutex.getOwnerThread(), 0,
                          TraceFlags());
        fg->Mutex.unlockThis(2);
    }

    LOG_DEBUG("Present finished");

    return result;
}

ULONG FGHooks::hkFGRelease(IUnknown* This)
{
    XeFGTrace::Record(XeFGTrace::EventType::SwapchainReleaseEnter, reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0,
                      0, TraceFlags());

    // We already released this one, prevent crashes
    if (This == oldSwapChain)
    {
        LOG_DEBUG("Release called on old swapchain, skipping release and returning 0");
        return 0;
    }

    static thread_local bool skipReleaseChecks = false;

    if (skipReleaseChecks || State::Instance().currentFGSwapchain != This || State::Instance().isShuttingDown)
        return o_FGRelease(This);

    if (State::Instance().activeFgOutput == FGOutput::XeFG && State::Instance().currentFG != nullptr)
    {
        auto* xefg = dynamic_cast<XeFG_Dx12*>(State::Instance().currentFG);
        if (xefg != nullptr && xefg->SwapchainReleaseOwnedByCurrentThread())
        {
            LOG_TRACE("[XeFG][Lifecycle] action = fg_release_forwarded, "
                      "reason = same_thread_lifecycle_reentry");
            return o_FGRelease(This);
        }
    }

    This->AddRef();

    auto& state = State::Instance();

    if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
    {
        const auto releaseResult = o_FGRelease(This);
        if (releaseResult == 1)
        {
            XeFGTrace::Record(XeFGTrace::EventType::SwapchainReleaseLifecycleBegin,
                              reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(state.currentFG), 0, 0,
                              0, 0, static_cast<int32_t>(releaseResult), TraceFlags());
            LOG_DEBUG("");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

                resizeFenceValue++;
                State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);

                if (resizeFence->GetCompletedValue() < resizeFenceValue)
                {
                    resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
                    // Max 5 sec
                    auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
                    LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
                }
            }

            // To prevent deadlock when FG release the swapchain
            skipReleaseChecks = true;
            bool releaseSucceeded = true;

            if (State::Instance().currentFG != nullptr)
            {
                LOG_DEBUG("FG Swapchain released, release FG & swapchain context");
                if (auto* xefg = dynamic_cast<XeFG_Dx12*>(State::Instance().currentFG); xefg != nullptr)
                {
                    releaseSucceeded =
                        xefg->ReleaseSwapchainFromFinalProxyRelease(_hwnd, [This]() { o_FGRelease(This); });
                }
                else
                {
                    releaseSucceeded = State::Instance().currentFG->ReleaseSwapchain(_hwnd);
                }
                XeFGTrace::Record(XeFGTrace::EventType::SwapchainFinalProxyRelease,
                                  reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(state.currentFG), 0, 0,
                                  0, 0, releaseSucceeded ? S_OK : E_FAIL, TraceFlags());
            }

            if (!releaseSucceeded)
            {
                skipReleaseChecks = false;
                LOG_ERROR("[XeFG][Lifecycle] action = fg_release_aborted, reason = release_swapchain_failed");
                XeFGTrace::Record(XeFGTrace::EventType::SwapchainReleaseLifecycleEnd,
                                  reinterpret_cast<uint64_t>(This), reinterpret_cast<uint64_t>(state.currentFG), 0, 0,
                                  0, 0, E_FAIL, TraceFlags());
                return 0;
            }

            LOG_DEBUG("FG Swapchain released, clearing currentFGSwapchain");
            state.currentFGSwapchain = nullptr;

            skipReleaseChecks = false;

            XeFGTrace::Record(XeFGTrace::EventType::SwapchainReleaseLifecycleEnd,
                              reinterpret_cast<uint64_t>(This), 0, 0, 0, 0, 0, S_OK, TraceFlags());

            return 0;
        }
    }
    else
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_INFO("Preserving FG Swapchain from release");
            return 0;
        }
    }

    return o_FGRelease(This);
}
