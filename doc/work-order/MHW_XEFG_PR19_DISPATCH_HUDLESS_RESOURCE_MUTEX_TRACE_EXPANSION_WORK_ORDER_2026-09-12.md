# MHW XeFG PR19 Dispatch / Hudless Resource-Mutex Trace Expansion Work Order

Date: 2026-09-12

## Purpose

Extend the existing temporary diagnostic PR #19 (`diag/mhw-xefg-auto-ring-trace`) with one additional diagnostic commit.

Do **not** create a new PR for this work. Do **not** merge PR #19. The tester will build PR #19 locally and reproduce the Monster Hunter Wilds Intel XeFG crash again.

The goal of this follow-up is to instrument the remaining unobserved portion of `XeFG_Dx12::Dispatch()` after the previous PR19 trace expansion narrowed the crash to the interval immediately after `XeFGPresentBeforeDispatch` and before the first existing `XeFGTagFrameResourceResult` event.

This is diagnostic-only work. Do not attempt to fix synchronization, resource ownership, or XeFG behavior in this commit.

---

## Current evidence

The latest MHW crash reproduced the same game error:

```text
Fatal D3D error (7, E_ABORT, 0x80004004)
```

The game CrashReport also reproduced the same final access-violation signature as the previous sample.

The new PR19 trace expansion showed that the final Present thread successfully progressed through:

```text
PresentEnter
PresentDispatchBegin
PresentMutexDecision
PresentMutexWaitBegin
PresentMutexAcquired
XeFGPresentEnter
XeFGPresentBeforeUiWork
XeFGPresentAfterUiWork
XeFGPresentBeforeScWork
XeFGPresentAfterScWork
XeFGPresentBeforeDispatch
```

and then the trace ended.

The following were **not** observed for the final Present:

```text
XeFGTagFrameResourceResult
XeFGEnableDebugFeatureResult
XeFGTagFrameConstantsResult
XeFGSetPresentIdResult
XeFGPresentAfterDispatch
DxgiPresentBegin
```

Therefore the failure is now inside the early part of `XeFG_Dx12::Dispatch()`.

On normal neighboring frames, the first trace event after `XeFGPresentBeforeDispatch` is usually a `XeFGTagFrameResourceResult`, which strongly points at the late Hudless resource path in `Dispatch()`:

```cpp
if (!_noHudless[fIndex])
{
    auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
    if (res->validity != FG_ResourceValidity::ValidNow)
    {
        res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
        res->frameIndex = fIndex;
        SetResource(res);
    }
}
```

`SetResource()` then eventually calls:

```cpp
XeFGProxy::D3D12TagFrameResource(...)
```

The existing trace records the result only **after** that XeFG call returns.

This leaves three important possibilities to distinguish:

1. `Dispatch()` fails before the Hudless lookup or during direct `_frameResources[fIndex]` access.
2. `SetResource()` blocks or races while acquiring `_resourceMutex[fIndex]`.
3. `XeFGProxy::D3D12TagFrameResource()` itself never returns normally for the crashing frame.

---

## Important synchronization concern to observe, not fix yet

`Dispatch()` currently directly accesses and mutates `_frameResources[fIndex]` before entering `SetResource()`:

```cpp
auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
res->frameIndex = fIndex;
SetResource(res);
```

Meanwhile `NewFrame()` clears the same frame-slot map while holding `_resourceMutex[fIndex]`:

```cpp
std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
_frameResources[fIndex].clear();
```

`SetResource()` also acquires `_resourceMutex[fIndex]`, but only after it has already received the `Dx12Resource*` pointer.

This creates a plausible timing-sensitive race / stale-pointer window involving direct `std::unordered_map` access, map clear/insert/rehash, and the late Hudless resource path.

For this PR19 follow-up, **do not fix this behavior yet**. Instrument the exact boundaries first so the next crash can prove or disprove this path.

---

## Required implementation

Add a small set of new `XeFGTrace::EventType` values and corresponding decoder names.

Suggested names:

```cpp
DispatchEnter,
DispatchAfterIndexResolve,
DispatchBeforeHudlessLookup,
DispatchAfterHudlessLookup,
DispatchBeforeHudlessSetResource,
SetResourceEnter,
SetResourceBeforeMutexWait,
SetResourceAfterMutexAcquire,
SetResourceBeforeTagFrameResource,
SetResourceAfterTagFrameResource,
```

The exact enum names may be adjusted for local naming consistency, but keep one-to-one stage visibility.

Do not renumber or reorder existing PR19 event values. Append new events after the current final event so existing trace decoding remains stable.

Update `tools/decode_xefg_trace.py` with the same numeric/event mapping and extend the synthetic self-test with at least one newly added event.

---

## Instrument `XeFG_Dx12::Dispatch()`

### 1. Dispatch entry

Record immediately at function entry, before meaningful state/resource work:

```cpp
bool XeFG_Dx12::Dispatch()
{
    XeFGTrace::Record(EventType::DispatchEnter, ...);
    ...
}
```

Capture at minimum:

- `_swapChain`
- `_swapChainContext`
- current thread ID through normal trace record infrastructure
- `_frameCount`

### 2. Index resolution

Immediately after:

```cpp
UINT64 willDispatchFrame = 0;
auto fIndex = GetDispatchIndex(willDispatchFrame);
```

record `DispatchAfterIndexResolve`.

Use aux fields to capture:

- `fIndex`
- low 32 bits of `willDispatchFrame`

If `fIndex < 0`, preserve the result in the event before returning.

### 3. Hudless lookup boundary

Immediately before this access:

```cpp
auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
```

record:

```text
DispatchBeforeHudlessLookup
```

Immediately after obtaining the pointer, record:

```text
DispatchAfterHudlessLookup
```

Capture at least:

- `fIndex`
- `res` pointer
- `res->resource` pointer, if it is safe to read only after the lookup completes
- current validity value in an aux field if convenient

Do not add locking for the sake of tracing.

### 4. Before late Hudless `SetResource()`

Immediately before:

```cpp
SetResource(res);
```

record:

```text
DispatchBeforeHudlessSetResource
```

This event should carry:

- `fIndex`
- `res`
- `res->resource`
- `res->cmdList`
- `res->validity`

Do not change how the resource is mutated or passed.

---

## Instrument `XeFG_Dx12::SetResource()`

### 5. Function entry

At the very beginning of `SetResource()`, before resource-type logic and before the resource mutex is acquired, record:

```text
SetResourceEnter
```

Capture:

- `inputResource`
- `inputResource->resource` when safe
- resource type
- frame index as known at that point

This is important because the current hypothesis includes the possibility that a stale or invalid `inputResource` pointer is already present before the mutex is acquired.

Do not add defensive behavior changes in this diagnostic commit.

### 6. Resource mutex wait/acquire

Current code eventually executes:

```cpp
std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
```

Split this only as required to expose wait/acquire markers without changing semantics.

Preferred shape:

```cpp
XeFGTrace::Record(EventType::SetResourceBeforeMutexWait, ...);
std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
XeFGTrace::Record(EventType::SetResourceAfterMutexAcquire, ...);
```

Do not replace the mutex, change its type, use `try_lock`, or add timeouts.

Capture `fIndex` and resource pointers in both events.

### 7. XeFG `D3D12TagFrameResource` call boundary

The existing code records only the result after this call:

```cpp
auto result = XeFGProxy::D3D12TagFrameResource()(
    _swapChainContext,
    fResource->cmdList,
    frameId,
    &resourceParam);
```

Add an explicit **before-call** event:

```text
SetResourceBeforeTagFrameResource
```

and either reuse the existing `XeFGTagFrameResourceResult` as the after-call event or add:

```text
SetResourceAfterTagFrameResource
```

if that makes correlation clearer.

Preferred minimal-overhead implementation:

- Add `SetResourceBeforeTagFrameResource` immediately before the SDK call.
- Keep the existing `XeFGTagFrameResourceResult` immediately after the call.
- Only add `SetResourceAfterTagFrameResource` if needed for decoding clarity; it is not strictly required if `XeFGTagFrameResourceResult` already provides the exact return boundary.

The before-call event must capture:

- `_swapChainContext`
- `resourceParam.pResource`
- `fResource->cmdList`
- `frameId`
- `fIndex`
- resource type

This is the most important new boundary.

If the next crash ends on `SetResourceBeforeTagFrameResource` with no subsequent `XeFGTagFrameResourceResult`, we will have direct evidence that the XeFG SDK call did not return normally.

---

## Optional distortion marker

The same early-Dispatch pattern exists for the late distortion resource path:

```cpp
if (!_noDistortionField[fIndex])
{
    ...
    SetResource(res);
}
```

If instrumentation can be added with only one or two extra stage markers, record a resource-type field so the same `SetResource*` events distinguish Hudless vs Distortion automatically.

Do not duplicate a large event set only for distortion.

---

## Failure flushing

Retain the existing PR19 policy:

- No normal per-event file flush.
- No new general `LOG_*` diagnostics.
- No `FlushFileBuffers()` or equivalent on successful hot-path events.
- Continue to perform at most one best-effort trace flush only after a real failed raw HRESULT / negative XeFG result.

Do **not** add a flush at `DispatchEnter`, mutex wait/acquire, or before `D3D12TagFrameResource()`.

The purpose is to preserve the logging-off timing characteristics as much as possible.

---

## Explicit non-goals

Do not include any of the following in this commit:

- No resource-mutex redesign.
- No new lock around the entire `Dispatch()` function.
- No snapshot/copy fix yet.
- No `_frameResources` container replacement.
- No generation counter fix yet.
- No `_skip*` change.
- No `OwnedMutex` change.
- No Present/Resize lifecycle change.
- No XeFG warning/result-policy change.
- No REF changes.
- No INI/config option.
- No normal debug/file logging.
- No SEH/VEH crash handler.

This commit is diagnostic-only.

---

## Expected next-crash interpretation

### Case A: trace ends at `DispatchBeforeHudlessLookup`

Strong evidence for a failure during direct `_frameResources[fIndex]` access / container race.

Next production investigation should focus on protecting lookup and avoiding raw pointers into a concurrently mutated `unordered_map`.

### Case B: trace reaches `DispatchAfterHudlessLookup` but not `SetResourceEnter`

Strong evidence for stale/invalid `Dx12Resource*` use or failure while reading/mutating the returned object before the call.

### Case C: trace reaches `SetResourceBeforeMutexWait` but not `SetResourceAfterMutexAcquire`

Strong evidence for synchronization/deadlock/blocking around `_resourceMutex[fIndex]`.

### Case D: trace reaches `SetResourceAfterMutexAcquire` and then ends before `SetResourceBeforeTagFrameResource`

The failure remains inside `SetResource()` pre-tag resource manipulation. Add only the next smallest stage split after inspecting that exact path.

### Case E: trace reaches `SetResourceBeforeTagFrameResource` but there is no `XeFGTagFrameResourceResult`

This is the most important expected outcome.

It strongly indicates the XeFG `D3D12TagFrameResource()` call does not return normally for the crashing frame.

At that point, correlate the input resource pointer, command-list pointer, frameId, fIndex, and resource type against neighboring successful frames to determine whether OptiScaler supplied a stale/invalid resource or whether the SDK/driver failed despite apparently valid input.

### Case F: `XeFGTagFrameResourceResult` returns a negative result / `E_ABORT`-equivalent failure

The existing PR19 failure trigger should capture and flush it. Use the raw result and exact source event as the primary failure.

---

## Validation requirements

Before committing to PR #19:

1. Build `OptiScaler.vcxproj` Release x64 successfully.
2. Run:

```text
python tools/decode_xefg_trace.py --self-test
```

3. Run:

```text
python -m py_compile tools/decode_xefg_trace.py
```

4. Run:

```text
git diff --check
```

5. Confirm existing event numbers remain unchanged and new events are appended only.
6. Confirm there are no behavior changes beyond trace calls and any mechanically necessary lock-construction split that preserves identical locking semantics.
7. Confirm no new normal `LOG_*` calls were added.

---

## Tester configuration after implementation

Use the same reproduction setup:

- Monster Hunter Wilds
- Intel GPU
- XeFG
- PR #19 diagnostic OptiScaler build including this additional commit
- fork REFramework
- OptiScaler normal logging OFF
- REF debug/XeFG diagnostics OFF
- no additional INI changes

After the crash, collect:

```text
OptiScaler_XeFGTrace.bin
re2_framework_log.txt
CrashReport.txt
MiniDump.dmp
```

The most important artifact remains `OptiScaler_XeFGTrace.bin`.

---

## Commit / PR handling

Implementation must be pushed as an **additional commit on PR #19**.

Suggested commit message:

```text
Trace XeFG Dispatch resource handoff boundaries
```

Do not merge PR #19 after implementation. Leave it open/draft for local tester builds.
