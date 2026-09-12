# MHW XeFG PR19 FSRFG Startup / Destroy-Hang Trace Expansion Work Order

Date: 2026-09-13

## Purpose

Extend the existing temporary diagnostic PR #19 (`diag/mhw-xefg-auto-ring-trace`) with one additional diagnostic commit focused on the separate Monster Hunter Wilds **FSRFG -> XeFG startup hang**.

Do **not** create a new PR for this work. Do **not** merge PR #19. The tester will build PR #19 locally and reproduce the FSRFG -> XeFG startup hang.

This follow-up is diagnostic-only. It must not attempt to fix FSRFG activation, XeFG teardown, swapchain ownership, timeout behavior, or synchronization.

The goal is to answer two questions precisely:

1. Why does the FSRFG input path never reach an active XeFG frame-generation state before the startup swapchain lifecycle ends?
2. Which exact teardown boundary calls `XeFGProxy::Destroy()(context)`, and does execution stop inside that SDK call?

The existing DLSSG/MHW trace coverage in PR #19 must remain intact.

---

## Current evidence from `log4 fsrfr`

The latest FSRFG -> XeFG reproduction is materially different from the previously investigated DLSSG -> XeFG `E_ABORT` crash.

The trace shows successful XeFG startup operations:

```text
XeFGCreateContextResult        S_OK
XeFGSetLoggingCallbackResult   S_OK
XeFGSetLatencyReductionResult  S_OK
XeFGGetPropertiesResult        S_OK
XeFGInitSwapchainResult        S_OK
XeFGGetSwapchainPtrResult      S_OK
```

The swapchain then continues through hundreds of successful native Present calls.

However, during the captured run there are no observed successful transitions into actual XeFG FG work:

```text
XeFGSetEnabledResult   0 occurrences
XeFGPresentEnter       0 occurrences
DispatchEnter          0 occurrences
SetResourceEnter       0 occurrences
```

The XeFG feature therefore appears to remain inactive/paused while the game is still presenting normally.

The final trace sequence is effectively:

```text
... normal native Present activity ...
SwapchainReleaseEnter
SwapchainReleaseEnter
SwapchainReleaseEnter
XeFGDestroyBegin
<trace ends>
```

There is no corresponding:

```text
XeFGDestroyResult
```

The existing implementation records `XeFGDestroyBegin` immediately before:

```cpp
auto result = XeFGProxy::Destroy()(context);
```

and `XeFGDestroyResult` only after that call returns.

This makes a hard block/hang inside or immediately around the XeFG SDK destroy call the highest-priority hypothesis for the FSRFG startup hang.

The accompanying REFramework log does not show a corresponding final ResizeHold failure. The startup XeFG bind and observed resize transitions complete normally, so this follow-up should remain focused on the OptiScaler FSRFG/XeFG activation and teardown path.

---

## Important separation from the DLSSG investigation

Do not mix the FSRFG hang diagnosis with the separate DLSSG -> XeFG MHW `E_ABORT` / frame-boundary investigation.

The current working distinction is:

### DLSSG -> XeFG

- XeFG activates and game play proceeds.
- The failure occurs much later.
- The latest trace exposed a suspicious frame-number jump / slot-lifecycle mismatch.
- Existing PR19 Dispatch/resource trace coverage remains required.

### FSRFG -> XeFG

- XeFG context/swapchain creation succeeds.
- XeFG does not reach normal active FG dispatch during the observed startup period.
- Native Present continues.
- Swapchain release/teardown begins.
- Trace terminates at `XeFGDestroyBegin` with no `XeFGDestroyResult`.

Treat these as separate failure modes unless future evidence proves a shared cause.

---

## Implementation rules

1. Implement this as an **additional commit on PR #19**.
2. Keep PR #19 open and draft.
3. Do not modify PR #18.
4. Do not create a separate FSRFG diagnostic PR.
5. Use only the existing `XeFGTrace` binary ring-buffer infrastructure.
6. Do not add normal diagnostic `LOG_*` traffic for the new events.
7. Do not add synchronous per-event file I/O or normal-event flushes.
8. Append new event IDs only. Do not renumber or reorder existing event values.
9. Update `tools/decode_xefg_trace.py` with the exact matching event numbers/names.
10. Extend the decoder self-test with at least one new FSRFG event and at least one new teardown event.

---

## Required trace expansion: FSRFG activation path

The trace needs to identify whether FSRFG input is detected, whether a new Opti FG frame is created, whether `EvaluateState()` is reached, and why XeFG never reaches `SetEnabled(true)` / active Present.

Instrument the relevant FSRFG input path used by MHW. Preserve support for both currently implemented FSRFG-style input paths where practical, but do not duplicate large instrumentation blocks unnecessarily.

The repository currently has FSRFG input handling in paths including:

```text
OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
```

The implementer must first confirm which path MHW uses in this reproduction and instrument that path at minimum.

If a small shared/helper-level instrumentation point can safely distinguish both paths using an aux field, prefer that over duplicated event sets.

### Suggested new events

Append events similar to:

```cpp
FSRFGFrameBoundaryEnter,
FSRFGStartNewFrameBefore,
FSRFGStartNewFrameAfter,
FSRFGEvaluateStateBefore,
FSRFGEvaluateStateAfter,
FSRFGConfigObserved,
FSRFGActivateDecision,
FSRFGActivateBefore,
FSRFGActivateAfter,
FSRFGPresentCallbackEnter,
FSRFGPresentCallbackExit,
```

Exact names may be adjusted for codebase naming consistency, but retain equivalent visibility.

### Event payload requirements

Where applicable, capture through existing record fields:

- active FG input enum/value
- current Opti `_frameCount` / `FrameCount()`
- current slot/index
- incoming FSRFG frame ID when available
- `frameGenerationEnabled`
- `FGEnabled`
- `IsActive()`
- `IsPaused()`
- current FG pointer
- current FG swapchain pointer
- thread ID from the trace infrastructure

Do not dereference unsafe pointers only for diagnostics.

---

## FSRFG frame-boundary instrumentation

Where FSRFG calls:

```cpp
fg->StartNewFrame();
```

add before/after markers.

Preferred shape:

```cpp
XeFGTrace::Record(EventType::FSRFGStartNewFrameBefore, ...);
const auto frame = fg->StartNewFrame();
XeFGTrace::Record(EventType::FSRFGStartNewFrameAfter, ...);
```

If the existing caller does not consume the return value, preserve current behavior; use the returned frame only as trace metadata if convenient.

Capture:

- frame count before
- frame count after
- resulting slot/index
- external FSRFG frame ID if the callback provides one

Do not change frame-boundary semantics in this diagnostic commit.

---

## FSRFG `EvaluateState()` instrumentation

Where the FSRFG path calls:

```cpp
fg->EvaluateState(_device, _fgConst);
```

record immediately before and after.

Example:

```text
FSRFGEvaluateStateBefore
FSRFGEvaluateStateAfter
```

Capture:

- `FrameCount()`
- `IsActive()`
- `IsPaused()`
- relevant FSRFG input identifier/path identifier

The purpose is to prove whether FSRFG startup reaches XeFG state evaluation at all before the swapchain teardown begins.

---

## `frameGenerationEnabled` / activation decision instrumentation

The FSRFG input path contains logic equivalent to:

```cpp
if (config->frameGenerationEnabled &&
    !fg->IsActive() &&
    Config::Instance()->FGEnabled.value_or_default())
{
    if (!fg->IsPaused())
    {
        fg->Activate();
        fg->ResetCounters();
    }
}
```

Add one decision event before this branch evaluates or immediately after obtaining the required state.

Suggested event:

```text
FSRFGActivateDecision
```

Encode at minimum:

- `frameGenerationEnabled`
- `FGEnabled`
- `IsActive()`
- `IsPaused()`
- current frame count

A compact bit field in `aux0` / `aux1` is preferred to avoid expanding the trace record layout.

Suggested bit representation if convenient:

```text
bit 0 = frameGenerationEnabled
bit 1 = global FGEnabled
bit 2 = fg IsActive
bit 3 = fg IsPaused
```

Do not add a new trace-record version solely for this.

If `fg->Activate()` is actually called, record:

```text
FSRFGActivateBefore
FSRFGActivateAfter
```

The after event must record the resulting `IsActive()` / `IsPaused()` state.

---

## XeFG `Activate()` boundary

Existing `XeFGSetEnabledResult` records the SDK result if `XeFG_Dx12::Activate()` reaches:

```cpp
XeFGProxy::SetEnabled()(_swapChainContext, true);
```

For this investigation, add a low-overhead entry/decision marker inside `XeFG_Dx12::Activate()` so that a missing `XeFGSetEnabledResult` can be explained.

Suggested events:

```text
XeFGActivateEnter
XeFGActivateEligibility
```

`XeFGActivateEligibility` should encode the conditions that gate the SDK call:

```cpp
_swapChainContext != nullptr
_fgContext != nullptr
!_isActive
IsLowResMV()
nativeAA
ForceFGRenderSizeMVs quirk
FGXeFGIgnoreInitChecks
```

Use a bit field rather than changing behavior.

This is important because the FSRFG sample had zero `XeFGSetEnabledResult` events. We need to know whether:

1. `Activate()` was never called, or
2. it was called but its eligibility condition rejected activation before `SetEnabled(true)`.

Do not relax any condition in this commit.

---

## FSRFG Present-callback boundary

`FGHooks::FGPresent()` calls the FSRFG present callback only when the current FG feature is active and not paused.

Add a small marker around the FSRFG-specific callback only if doing so does not duplicate an already unambiguous existing event.

Suggested events:

```text
FSRFGPresentCallbackEnter
FSRFGPresentCallbackExit
```

Use an aux field to distinguish legacy `FSRFG` from `FSRFG30` if both are covered.

Do not change callback order or Present behavior.

---

## Required trace expansion: swapchain teardown path

The second half of this work order is higher priority than the FSRFG activation tracing because the current sample already terminates at `XeFGDestroyBegin`.

Instrument the complete final-proxy -> release -> FG-context -> XeFG-context teardown transaction.

Append events similar to:

```cpp
XeFGReleaseFromFinalProxyEnter,
XeFGReleaseFromFinalProxyExit,
XeFGReleaseLockedEnter,
XeFGReleaseLockedBeforeDestroyFGContext,
XeFGReleaseLockedAfterDestroyFGContext,
XeFGReleaseLockedBeforeFinalProxyRelease,
XeFGReleaseLockedAfterFinalProxyRelease,
XeFGReleaseLockedBeforeDestroySwapchainContext,
XeFGReleaseLockedAfterDestroySwapchainContext,
XeFGDestroyFGContextEnter,
XeFGDestroyFGContextExit,
XeFGDestroySwapchainContextEnter,
XeFGDestroySwapchainContextBeforeSdkDestroy,
XeFGDestroySwapchainContextAfterSdkDestroy,
XeFGDestroySwapchainContextExit,
```

Some events can be omitted if an existing event already provides exactly the same boundary. Do not create redundant pairs merely for naming symmetry.

However, the following boundaries are mandatory:

1. entry into `ReleaseSwapchainFromFinalProxyRelease()`
2. entry into `ReleaseSwapchainLocked()`
3. immediately before and after `DestroyFGContext()`
4. immediately before and after the `releaseFinalProxy()` callback
5. immediately before calling `DestroySwapchainContext()`
6. entry into `DestroySwapchainContext()`
7. immediately before `XeFGProxy::Destroy()(context)`
8. immediately after `XeFGProxy::Destroy()(context)` returns
9. exit from `DestroySwapchainContext()` if reached

The existing `XeFGDestroyBegin` and `XeFGDestroyResult` may satisfy boundaries 7 and 8 if their placement remains exactly adjacent to the SDK call. Do not duplicate them unless required to distinguish function entry from SDK-call entry.

---

## `ReleaseSwapchainFromFinalProxyRelease()` instrumentation

Current flow includes the final-proxy callback and the lifecycle lock.

Record an entry event with:

- `hwnd`
- current FG pointer
- current FG swapchain pointer
- `_swapChainContext`
- current thread ID via standard trace infrastructure

Record immediately before and after the final-proxy callback executes:

```cpp
releaseFinalProxy();
```

Suggested markers:

```text
XeFGFinalProxyReleaseBefore
XeFGFinalProxyReleaseAfter
```

This is a critical boundary because the proxy may be destroyed by the callback and the current code explicitly avoids touching it afterward.

Do not add any new `AddRef`/`Release` operations for tracing.

---

## `ReleaseSwapchainLocked()` instrumentation

At function entry record:

```text
XeFGReleaseLockedEnter
```

Capture:

- `hwnd`
- `_hwnd`
- `_swapChainContext`
- `_fgContext`
- `_swapchainReleaseInProgress`
- configured swapchain-mutex policy if convenient

Add stage events immediately around:

```cpp
DestroyFGContext();
```

and immediately before:

```cpp
DestroySwapchainContext();
```

If `ReleaseObjects()` is reached after the SDK context is destroyed, a single `XeFGReleaseLockedBeforeReleaseObjects` event is optional but useful.

Do not change lifecycle mutex behavior, lock order, or error handling.

---

## `DestroyFGContext()` instrumentation

Add entry and exit events:

```text
XeFGDestroyFGContextEnter
XeFGDestroyFGContextExit
```

Record whether:

- `_isActive`
- `_fgContext`
- `_swapChainContext`

are nonzero / active at entry.

This function currently calls `Deactivate()` and then clears the FG-context alias before releasing internal objects.

Do not modify the order.

The trace should allow us to determine whether FSRFG teardown occurs while XeFG was never active, already deactivated, or partially initialized.

---

## `DestroySwapchainContext()` instrumentation

The existing function already records:

```text
XeFGDestroyBegin
XeFGDestroyResult
```

Retain those events exactly.

Add a function-entry event before early-return checks if practical:

```text
XeFGDestroySwapchainContextEnter
```

Encode:

- `_swapChainContext`
- `State::Instance().isShuttingDown`
- `_swapchainRecreationBlocked`

If the function returns early because `_swapChainContext == nullptr` or shutdown is in progress, record a compact exit/reason event if this can be done without adding heavy logic.

The key SDK-call region must remain equivalent to:

```cpp
XeFGTrace::Record(EventType::XeFGDestroyBegin, ...);
auto result = XeFGProxy::Destroy()(context);
XeFGTrace::Record(EventType::XeFGDestroyResult, ...);
```

Do not wrap the SDK call in a worker thread, timeout, future, SEH handler, or cancellation mechanism in this diagnostic commit.

---

## Thread / lifecycle correlation

For all teardown stage events, preserve the existing trace thread ID.

Where possible, place the following in pointer/aux fields without changing the record layout:

- current FG pointer
- current FG swapchain pointer
- `_swapChainContext`
- `_fgContext`
- swapchain-release owner thread
- release-in-progress flag
- mutex owner / owner thread where already available

The next trace should make it obvious whether all release/destroy work is happening on the same render/present thread or whether another thread enters the lifecycle transaction.

Do not add new synchronization solely to make tracing easier.

---

## Decoder updates

Update `tools/decode_xefg_trace.py` so every new event has a stable readable name.

Requirements:

1. Existing event numbers remain unchanged.
2. New events are appended after the current highest event ID.
3. `PRIMARY_FAILURE_EVENT` remains unchanged.
4. The trace header and record binary layout remain version 1 unless a layout change is absolutely unavoidable. A layout change should not be needed for this work.
5. Existing traces from earlier PR19 builds must still decode.
6. `--self-test` must include at least:
   - one FSRFG activation/boundary event
   - one final-proxy/release event
   - one destroy-stage event

Do not treat a normal stage marker as a failure event.

---

## Failure flushing policy

Preserve the existing PR19 behavior:

- no flush for normal stage events
- no flush before entering `Destroy()`
- no flush merely because a function does not return quickly
- one best-effort failure flush only for an actual negative raw HRESULT / XeFG result according to the existing PR19 policy

The memory-mapped ring is specifically intended to survive this type of hard hang without introducing ordinary file-logging timing changes.

Do not add spdlog/file logging as a fallback.

---

## Explicit non-goals

Do **not** implement any of the following in this commit:

- No `Destroy()` timeout.
- No asynchronous/threaded `Destroy()` wrapper.
- No forced context leak as a workaround.
- No skip-destroy policy.
- No `TerminateThread`, cancellation, watchdog, or recovery thread.
- No new COM `AddRef`/`Release` ownership experiment.
- No change to final-proxy ownership policy.
- No lifecycle mutex redesign.
- No `_skipPresent*` / `_skipResize*` change.
- No `OwnedMutex` change.
- No frame-boundary production fix.
- No DLSSG frame-ID fix in this commit.
- No REF changes.
- No INI/config switch.
- No general logging increase.

This work is observation-only.

---

## Expected next-run interpretation

### Case A: no FSRFG frame-boundary event is observed

The FSRFG game/plugin path is not reaching OptiScaler's expected input callback before the startup swapchain teardown.

Next investigation should focus on FSRFG hook/config interception before touching XeFG lifecycle code.

### Case B: FSRFG frame-boundary events occur but `FSRFGActivateDecision` always reports paused

The primary startup issue is the pause/target-frame lifecycle preventing activation.

Correlate the target/reset/resize sequence before proposing any production change.

### Case C: `FSRFGActivateBefore` occurs but no `XeFGActivateEnter`

The FSRFG adapter/caller path is failing between its activation decision and the actual virtual feature call.

Inspect only that boundary next.

### Case D: `XeFGActivateEnter` occurs, eligibility rejects activation, and no `XeFGSetEnabledResult` appears

The XeFG activation gate is the reason FG never becomes active.

Use the eligibility bitmask to identify the exact failed prerequisite.

### Case E: XeFG activation succeeds but teardown still occurs immediately afterward

The FSRFG hang is not merely caused by never activating. Correlate the first activation with swapchain release ownership and startup re-creation.

### Case F: final proxy release markers complete, `DestroySwapchainContext()` begins, then trace ends at existing `XeFGDestroyBegin`

This is the strongest expected confirmation of the current hypothesis:

> OptiScaler successfully reaches the XeFG SDK destroy call, but `XeFGProxy::Destroy()(context)` does not return.

At that point, do not immediately add a timeout. First inspect whether the context is being destroyed in a legal SDK lifecycle state and whether any proxy/swapchain reference or internal Present activity is still outstanding.

### Case G: trace stops before `XeFGDestroyBegin`

The hang is in OptiScaler teardown before the SDK call. The last boundary will identify whether it is:

- `DestroyFGContext()` / `Deactivate()`
- final-proxy release
- lifecycle mutex ownership
- release-object cleanup
- entry into `DestroySwapchainContext()`

### Case H: `XeFGDestroyResult` appears successfully but no teardown exit event follows

The SDK destroy call is not the hang. Investigate OptiScaler cleanup immediately after context destruction instead.

---

## Validation requirements

Before pushing the implementation commit to PR #19:

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

5. Confirm all existing event IDs are unchanged.
6. Confirm all new event IDs are appended only.
7. Confirm trace record/header layout remains compatible with earlier PR19 traces.
8. Confirm no behavior changes were introduced.
9. Confirm no new ordinary diagnostic `LOG_*` calls were added for this work.
10. Confirm there is no timeout, worker-thread, ownership, or synchronization change around `Destroy()`.

---

## Tester configuration after implementation

Use the FSRFG-specific reproduction setup:

- Monster Hunter Wilds
- Intel GPU
- FSRFG input -> XeFG output
- PR #19 diagnostic OptiScaler build including this additional commit
- fork REFramework
- normal OptiScaler logging OFF
- REF debug/XeFG diagnostics OFF
- otherwise same game/config environment as `log4 fsrfr`

If the process becomes unresponsive, do not wait for a clean shutdown before collecting the trace. Preserve the memory-mapped trace file as soon as practical after terminating the hung game.

Collect at minimum:

```text
OptiScaler_XeFGTrace.bin
re2_framework_log.txt
```

If Windows or MHW creates any crash/hang dump or report, collect that as well, but the binary XeFG trace remains the primary artifact.

---

## Commit / PR handling

Implementation must be pushed as an **additional commit on PR #19** (`diag/mhw-xefg-auto-ring-trace`).

Suggested commit message:

```text
Trace FSRFG startup and XeFG teardown boundaries
```

Do not merge PR #19 after implementation.

Leave PR #19 open/draft for the tester's local Release x64 build and runtime reproduction.
