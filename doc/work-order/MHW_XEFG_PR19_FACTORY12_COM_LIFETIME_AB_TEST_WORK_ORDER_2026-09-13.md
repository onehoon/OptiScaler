# MHW XeFG PR19 `IDXGIFactory2` COM Lifetime A/B Test Work Order

Date: 2026-09-13  
Target repository: `onehoon/OptiScaler`  
Target branch / PR: `diag/mhw-xefg-auto-ring-trace` / PR #19  
Scope: one controlled functional A/B change only

## Objective

Add one minimal test commit on top of PR #19 to determine whether the MHW FSRFG -> XeFG startup termination is caused by a COM lifetime defect in `XeFG_Dx12::CreateSwapchain()` / `XeFG_Dx12::CreateSwapchain1()`.

The current code obtains `IDXGIFactory2* factory12` with `QueryInterface()`, immediately calls `factory12->Release()`, and later passes the same pointer to `XeFGProxy::D3D12InitFromSwapChainDesc()`.

The log5 crash-dump correlation report identifies this as the strongest concrete source-level suspect. Both independent MHW runs terminate after `XeFGGetPropertiesResult = SUCCESS` and before `XeFGInitSwapchainResult` is recorded.

This work order is intentionally limited to moving the `factory12->Release()` point so the COM reference remains valid through `D3D12InitFromSwapChainDesc()`.

Do not attempt any other fix in this commit.

## Important context

This is not an FSRFG-only code path. The defect is in XeFG swapchain creation and may affect any input path that ultimately creates an XeFG output swapchain, including DLSSG -> XeFG and FSRFG -> XeFG.

However, do not use this change to reinterpret previously observed later-stage failures. MHW DLSSG log3 successfully passed swapchain initialization and later failed around frame/resource generation. release/0.9 also contains the same early-release pattern and can proceed farther in some configurations. Therefore this is a controlled A/B test for the specific log5 startup failure, not a claim that this defect explains every XeFG issue.

PR #19 must remain unmerged after this test commit.

## Evidence boundary

Both log5 runs showed this trace sequence:

```text
SwapchainCreate1Begin
XeFGCreateContextResult              SUCCESS
XeFGSetLoggingCallbackResult         SUCCESS
XeFGSetLatencyReductionResult        SUCCESS
XeFGGetPropertiesResult              SUCCESS
--- process terminates ---
```

No `XeFGInitSwapchainResult` was recorded.

The crash dumps were effectively identical and terminated through:

```text
0xC0000409 / FAST_FAIL_FATAL_APP_EXIT
ucrtbase!abort
```

with retained C++ EH metadata associated with the custom `dxgi.dll` image. DirectSR and XeLL were loaded but were not present on the recoverable faulting stack.

## Required source change

File:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Apply the same lifetime correction to both:

```cpp
XeFG_Dx12::CreateSwapchain(...)
XeFG_Dx12::CreateSwapchain1(...)
```

### Current unsafe pattern

Conceptually, both functions currently do this:

```cpp
IDXGIFactory2* factory12 = nullptr;
if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
    return false;

factory12->Release();

// ... parameter preparation ...

result = XeFGProxy::D3D12InitFromSwapChainDesc(
    _swapChainContext,
    hwnd,
    ...,
    realQueue,
    factory12,
    &params);
```

The `QueryInterface()` reference must remain owned until the XeFG initialization call has completed.

### Required A/B pattern

Use the smallest possible change:

```cpp
IDXGIFactory2* factory12 = nullptr;
if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
    return false;

// Keep factory12 alive through the XeFG SDK call.

result = XeFGProxy::D3D12InitFromSwapChainDesc(
    _swapChainContext,
    hwnd,
    ...,
    realQueue,
    factory12,
    &params);

factory12->Release();
factory12 = nullptr;
```

The release should occur immediately after `D3D12InitFromSwapChainDesc()` returns and before any return path based on `result`.

For example:

```cpp
result = XeFGProxy::D3D12InitFromSwapChainDesc()(
    _swapChainContext,
    hwnd,
    desc,
    pFullscreenDesc,
    realQueue,
    factory12,
    &params);

factory12->Release();
factory12 = nullptr;

XeFGTrace::Record(
    XeFGTrace::EventType::XeFGInitSwapchainResult,
    reinterpret_cast<uint64_t>(_swapChain),
    reinterpret_cast<uint64_t>(_swapChainContext),
    reinterpret_cast<uint64_t>(realQueue),
    0, 0, 0,
    static_cast<int32_t>(result));
```

Apply the equivalent change to the `CreateSwapchain()` overload using its local `scDesc` / `fsDesc` arguments.

## Why both functions must be changed

Both XeFG swapchain creation functions contain the same release-before-use pattern.

Do not patch only `CreateSwapchain1()` even though the current MHW reproduction enters that path. The A/B commit should remove the same lifetime defect consistently from both XeFG creation paths while changing no other behavior.

## Explicit non-goals

Do not change any of the following in this commit:

- XeLL or fakenvapi ownership/lifecycle
- `SetLatencyReduction()` behavior
- DirectSR handling
- `CheckForRealObject()` behavior
- command queue ownership or AddRef/Release behavior
- swapchain descriptors or XeFG init flags
- FSRFG frame IDs, frame slots, resources, dispatch, or activation logic
- DLSSG frame/resource logic
- REFramework integration
- `OwnedMutex`, Present, Resize, fence, or teardown behavior
- PR19 trace event definitions or decoder format
- additional logging or additional diagnostic events
- conversion to `ComPtr`, `wil::com_ptr`, or another RAII abstraction in this A/B commit

RAII may be considered later for the production fix, but it adds an unnecessary variable to this diagnostic test.

## Failure-path requirement

Ensure `factory12` is released exactly once after the XeFG init call returns, regardless of whether `result` indicates success or failure.

Do not leave the reference held across later `D3D12GetSwapChainPtr()` processing.

Do not add an extra `AddRef()` elsewhere to compensate for the old early release.

## Validation

At minimum run:

```text
Release x64 build of OptiScaler.vcxproj
python tools/decode_xefg_trace.py --self-test
python -m py_compile tools/decode_xefg_trace.py
git diff --check
```

Run the existing clang-format / CI checks used by PR #19.

Before committing, inspect the final diff and confirm the functional source change is limited to the `factory12` lifetime in the two XeFG swapchain creation functions. Documentation generated by this work order is already present and should not trigger unrelated source cleanup.

## Commit requirement

Create exactly one implementation commit on PR #19 for this A/B change.

Suggested commit message:

```text
Test XeFG factory COM lifetime through swapchain init
```

Do not squash PR #19.

Do not merge PR #19.

## Hardware test after build

Primary reproduction:

```text
Monster Hunter Wilds
FSRFG input -> XeFG output
same configuration used for log5 fsrfg
```

Keep the existing PR19 binary trace enabled and keep all other variables unchanged.

### Result interpretation

#### Result A: startup progresses and `XeFGInitSwapchainResult` appears

This strongly confirms the `IDXGIFactory2` release-before-use defect as the log5 startup failure trigger.

Capture the new `OptiScaler_XeFGTrace.bin` and note whether the game reaches `D3D12GetSwapChainPtr`, FSRFG activation/resource events, and Present.

Do not immediately conclude that later FSRFG texture/frame issues are fixed; those may be separate defects.

#### Result B: same termination remains after `XeFGGetPropertiesResult`

The lifetime defect is still worth fixing, but it is not sufficient to explain log5.

Do not make additional fixes in the same commit. The next diagnostic phase should isolate pre-call queue / descriptor / exact `D3D12InitFromSwapChainDesc()` entry state.

#### Result C: `XeFGInitSwapchainResult` is recorded with an error result

Record the raw XeFG result and preserve the trace. Do not add fallback/recovery behavior in this task.

## Secondary regression sanity checks

After the MHW FSRFG reproduction, if convenient, perform minimal smoke checks on an already-known working XeFG path such as a DLSSG -> XeFG Capcom title.

The purpose is only to confirm that keeping the factory reference alive through init does not regress successful XeFG creation.

Do not expand this work order into a full game matrix.

## Deliverable

Report:

1. implementation commit SHA,
2. exact source diff summary,
3. build / CI result,
4. confirmation that no unrelated source changes were made,
5. confirmation that PR #19 remains Draft/Open and unmerged.

Do not claim the root cause is confirmed until the hardware A/B result is available.
