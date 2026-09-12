# MHW XeFG PR19 Present-Internal D3D12 Trace Expansion Work Order

Date: 2026-09-12
Repository: `onehoon/OptiScaler`
Target PR: #19 `Add low-overhead automatic XeFG crash trace`
Target branch: `diag/mhw-xefg-auto-ring-trace`
Implementation mode: **add one or more commits to PR #19; do not create a separate PR**
Scope: diagnostic instrumentation only

## Objective

Extend the existing PR #19 low-overhead XeFG ring trace to cover the currently uninstrumented D3D12 work inside `XeFG_Dx12::Present()` and the shared command-list/fence helpers it calls.

The latest Monster Hunter Wilds crash reproduced as:

```text
Fatal D3D error (7, E_ABORT, 0x80004004)
```

The existing PR #19 trace successfully narrowed the failure window but did **not** capture a negative HRESULT/XeFG result or `PrimaryFailureTrigger` before process termination.

The next commit must preserve the same low-overhead design and only fill the missing trace gap between `PresentMutexAcquired` and the first existing XeFG `Dispatch()` result events.

## Evidence from the latest PR #19 crash capture

Session files were collected from:

```text
GoogleDrive\ref-xefg\2dnRefactor\log
```

Relevant artifacts:

```text
OptiScaler_XeFGTrace.bin
re2_framework_log.txt
CrashReport\CrashReport.txt
CrashReport\EngineReport.txt
CrashReport\DxDiag.txt
```

CrashReport confirms:

```text
Fatal D3D error (7, E_ABORT, 0x80004004)
```

The existing PR #19 ring trace covered roughly the final 96 seconds and showed:

- no negative DXGI HRESULT in the retained window,
- no negative XeFG result,
- no `PrimaryFailureTrigger`,
- no ResizeBuffers / ResizeBuffers1 activity,
- no resize-fence activity,
- no swapchain create/destroy/release activity,
- steady-state Present activity only.

The retained Present path also did **not** show the suspected cross-thread `_skipPresent` / `_skipPresent1` false-bypass pattern. The outer Present and XeFG internal Present1 calls were observed on the same render thread in the retained steady-state window.

PR #18 mutex instrumentation also showed normal waiting/acquisition behavior in the relevant window.

The critical final trace sequence was approximately:

```text
PresentEnter
PresentDispatchBegin
PresentMutexDecision
PresentMutexWaitBegin
PresentMutexAcquired
<TRACE ENDS>
```

There was no subsequent:

```text
DxgiPresentBegin
DxgiPresent1Begin
XeFGTagFrameConstantsResult
XeFGSetPresentIdResult
XeFGTagFrameResourceResult
PresentMutexUnlock
PresentDispatchEnd
```

Therefore the next diagnostic target is the code executed **after Present mutex acquisition but before the already-traced DXGI Present / XeFG Dispatch result calls**.

## Current primary target

Inspect and instrument `XeFG_Dx12::Present()` on the PR #19 branch.

The current function performs untraced D3D12 operations before reaching `Dispatch()`, including UI / swapchain command-list handling and queue/fence work.

Representative operations include:

```cpp
_uiCommandList[fIndex]->Close();
_gameCommandQueue->ExecuteCommandLists(...);
_gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

_scCommandList[fIndex]->Close();
_gameCommandQueue->ExecuteCommandLists(...);

return Dispatch();
```

Also instrument the shared helper paths used before or during this phase, especially:

```cpp
IFGFeature_Dx12::GetUICommandList()
IFGFeature_Dx12::GetSCCommandList()
IFGFeature_Dx12::WaitForUIAllocator()
IFGFeature_Dx12::SubmitUICommandList()
```

These contain currently untraced HRESULT-producing calls such as:

```cpp
ID3D12CommandAllocator::Reset()
ID3D12GraphicsCommandList::Reset()
ID3D12GraphicsCommandList::Close()
ID3D12CommandQueue::Signal()
ID3D12Fence::SetEventOnCompletion()
```

The purpose is to identify whether one of these calls returns `E_ABORT (0x80004004)` or another failure immediately before MHW enters its fatal D3D handler.

## Required trace additions

Extend `XeFGTrace::EventType` with narrowly scoped events for the missing Present-internal D3D12 section.

Suggested events:

```cpp
XeFGPresentEnter,
XeFGPresentBeforeUiWork,
XeFGPresentAfterUiWork,
XeFGPresentBeforeScWork,
XeFGPresentAfterScWork,
XeFGPresentBeforeDispatch,
XeFGPresentAfterDispatch,

UiCommandListCloseResult,
UiQueueSignalResult,
UiFenceSetEventResult,
UiFenceWaitResult,
UiAllocatorResetResult,
UiCommandListResetResult,

ScCommandListCloseResult,
ScAllocatorResetResult,
ScCommandListResetResult,
```

If there are existing helper names/events that fit better, use those rather than duplicating semantics.

Do **not** add events merely for verbosity. Each event should answer one of these questions:

1. Did execution reach this stage?
2. Which thread executed it?
3. Which command list / allocator / queue / fence was involved?
4. What raw HRESULT or wait result was returned?
5. Which frame/index was active?

## Required `XeFG_Dx12::Present()` instrumentation

Add low-overhead trace records around the currently unobserved sections.

Example shape:

```cpp
XeFGTrace::Record(
    XeFGTrace::EventType::XeFGPresentEnter,
    reinterpret_cast<uint64_t>(_swapChain),
    reinterpret_cast<uint64_t>(_swapChainContext),
    reinterpret_cast<uint64_t>(_gameCommandQueue),
    0,
    0,
    0,
    0,
    FGHooks::TraceFlags(),
    static_cast<uint32_t>(fIndex));
```

Before and after UI command-list submission:

```cpp
if (_uiCommandListResetted[fIndex])
{
    auto closeResult = _uiCommandList[fIndex]->Close();

    XeFGTrace::Record(
        XeFGTrace::EventType::UiCommandListCloseResult,
        reinterpret_cast<uint64_t>(_swapChain),
        reinterpret_cast<uint64_t>(_uiCommandList[fIndex]),
        reinterpret_cast<uint64_t>(_gameCommandQueue),
        _uiAllocatorFenceValues[fIndex],
        0,
        0,
        closeResult,
        0,
        static_cast<uint32_t>(fIndex));

    if (FAILED(closeResult))
        XeFGTrace::FlushAfterFailureBestEffort();

    if (SUCCEEDED(closeResult))
        _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);

    auto signalResult = _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

    XeFGTrace::Record(
        XeFGTrace::EventType::UiQueueSignalResult,
        reinterpret_cast<uint64_t>(_swapChain),
        reinterpret_cast<uint64_t>(_uiFence),
        reinterpret_cast<uint64_t>(_gameCommandQueue),
        _uiAllocatorFenceValues[fIndex],
        0,
        0,
        signalResult,
        0,
        static_cast<uint32_t>(fIndex));

    if (FAILED(signalResult))
        XeFGTrace::FlushAfterFailureBestEffort();
}
```

Preserve existing behavior. Do **not** add new early returns or recovery logic in this diagnostic commit unless required solely to avoid dereferencing an invalid object already guarded in the existing code.

## Required `IFGFeature_Dx12` instrumentation

Instrument raw HRESULTs in these helper operations:

### `WaitForUIAllocator()`

Record:

```cpp
_uiFence->SetEventOnCompletion(...)
WaitForSingleObject(...)
```

Store the wait result in `aux0` or an equivalent existing field.

### `SubmitUICommandList()`

Record:

```cpp
_uiCommandList[index]->Close()
_gameCommandQueue->Signal(...)
```

### `GetUICommandList()`

Record:

```cpp
_uiCommandAllocator[index]->Reset()
_uiCommandList[index]->Reset(...)
```

### `GetSCCommandList()`

Record:

```cpp
_scCommandAllocator[index]->Reset()
_scCommandList[index]->Reset(...)
_scCommandList[i]->Close()
```

Where practical, include:

```text
threadId         // already captured by XeFGTrace::Record
fIndex/index
command list pointer
allocator pointer
queue pointer
fence pointer/value
raw HRESULT
```

## Failure trigger behavior

Keep PR #19's current policy:

- first negative HRESULT / negative XeFG result becomes the primary failure trigger,
- `0x80004004 (E_ABORT)` must remain explicitly identifiable,
- at most one best-effort file flush after the first failure,
- no flush on successful hot-path events.

Do not add separate `FlushViewOfFile()` / `FlushFileBuffers()` calls throughout the new instrumentation. Let the existing `Record()` / primary-failure mechanism handle first-failure preservation.

## Very important: `ExecuteCommandLists` has no HRESULT

`ID3D12CommandQueue::ExecuteCommandLists()` returns `void`.

Do not invent a result code for it.

If useful, add lightweight stage markers:

```text
UiExecuteCommandListsBegin
UiExecuteCommandListsEnd
ScExecuteCommandListsBegin
ScExecuteCommandListsEnd
```

These are optional but useful because a GPU/driver-side fault can occur asynchronously around queue submission. The markers can show whether control returned from the submission call before the trace stopped.

Do not perform extra synchronization merely to diagnose it.

## Do not change behavior in this commit

This PR extension is **diagnostic-only**.

Do not:

- convert `_skipPresent*` / `_skipResize*` to `thread_local`,
- alter PR #18 `OwnedMutex` behavior,
- serialize Present/Resize further,
- add a new mutex,
- change queue/fence ownership,
- change command-list execution order,
- change resource lifetime,
- change swapchain lifecycle,
- add retry logic,
- swallow or transform HRESULTs,
- add a new INI option,
- enable normal OptiScaler logging,
- add hot-path `LOG_*` calls for this investigation,
- increase flush frequency.

The objective is observation, not correction.

## Decoder update

Update `tools/decode_xefg_trace.py` for every new event ID.

Keep compatibility with the current trace format/version if the record layout does not change.

Do not bump the binary format version solely because new event enum values were added.

The decoder must still expose:

```text
seq
qpc_delta_ms
thread
event
swapchain
object_or_context
aux_pointer
mutex_owner
mutex_owner_thread
fence
result
flags
aux0
aux1
failure_source_event
e_abort
```

Self-test should include at least one new D3D12 helper result event.

## Validation

Required before updating PR #19:

```text
Release x64 OptiScaler.vcxproj build PASS
python tools/decode_xefg_trace.py --self-test PASS
python -m py_compile tools/decode_xefg_trace.py PASS
git diff --check PASS
```

Code-review check:

- no new general logger call on the traced hot path,
- no successful Present causes disk flush,
- no behavior-changing synchronization added,
- all newly captured HRESULTs use the raw returned value,
- `ExecuteCommandLists` is represented only by stage markers, never a fake HRESULT.

## Runtime test procedure

Use the same MHW Intel XeFG reproduction setup.

Tester procedure:

```text
1. Build PR #19 after this additional commit.
2. Keep normal OptiScaler logging OFF.
3. Keep REF debug logging OFF unless already required for the baseline comparison.
4. Run MHW until Fatal D3D error (7, E_ABORT, 0x80004004) reproduces.
5. Send:
   - OptiScaler_XeFGTrace.bin
   - re2_framework_log.txt
   - the full CrashReport folder
```

No INI option should be required for the trace.

## Success criteria

The next crash trace should answer which of these happened first:

### Case A — explicit D3D12 helper failure

Example:

```text
PresentMutexAcquired
UiAllocatorResetResult = S_OK
UiCommandListResetResult = S_OK
UiCommandListCloseResult = E_ABORT
PrimaryFailureTrigger source=UiCommandListCloseResult
```

or equivalent for queue Signal / fence setup / SC command list.

### Case B — execution stops inside a void/asynchronous queue call

Example:

```text
UiExecuteCommandListsBegin
<no UiExecuteCommandListsEnd>
```

This would strongly prioritize queue submission / driver asynchronous failure.

### Case C — all Present-internal D3D12 work succeeds and `Dispatch()` begins

Example:

```text
ScCommandListCloseResult = S_OK
XeFGPresentBeforeDispatch
XeFGTagFrameConstantsResult = SUCCESS
...
```

Then the next investigation should move beyond the Present-internal gap rather than continuing to modify synchronization speculatively.

## Expected outcome

This commit should convert the current evidence:

```text
PresentMutexAcquired
<unknown gap>
Fatal D3D E_ABORT
```

into a precise last-known D3D12/XeFG operation with thread, object pointers, frame/index, and raw result while preserving the logging-OFF timing profile as much as practical.
