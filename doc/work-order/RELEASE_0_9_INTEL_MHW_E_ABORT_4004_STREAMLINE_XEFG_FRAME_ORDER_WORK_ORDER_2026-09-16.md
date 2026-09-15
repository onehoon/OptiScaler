# Release 0.9 — Intel MHW E_ABORT 4004 Streamline/XeFG Frame-Order Work Order

**Status:** Implementation + diagnostic work order  
**Target branch:** `reframework-0.9`  
**Reviewed base:** `d6b0132f4f6faabf7235dba5b8d684edfd8e2831` (`fix: skip REF pre-retire during XeFG shutdown (#35)`)  
**Date:** 2026-09-16  

## 1. Objective

Investigate and harden the current Intel Monster Hunter Wilds runtime path where the latest OptiScaler build can fail during gameplay with:

```text
Fatal D3D error (7, E_ABORT, 0x80004004)
```

The critical reproduction property is:

> The failure is observed when OptiScaler file logging is disabled, while enabling the heavy trace/file logger materially reduces or hides the failure.

Treat this as a timing-sensitive runtime ordering/race problem until disproved.

This work is **not** a continuation of the PR34/PR35 process-shutdown bug. The failing run reaches normal gameplay and fails before process shutdown.

The first implementation must be a **narrow Streamline-input ↔ XeFG-present ordering A/B**, using the synchronization primitive already used by the FG present path. Do not begin with sleeps, logging delays, vendor-DLL special cases, or broad lifecycle changes.

---

## 2. Current evidence

Test data:

```text
C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004
```

Environment:

- Monster Hunter Wilds
- Intel GPU / native Intel XeFG implementation path
- latest `reframework-0.9` build at the time of capture
- OptiScaler logging disabled in the failing run
- REFramework logging available
- Capcom CrashReport / MiniDump available

Observed failure:

```text
Fatal D3D error (7, E_ABORT, 0x80004004)
```

Important observations from the captured REF log and dump:

1. The game runs and presents normally for minutes before failure.
2. REF XeFG binding remains on the same lifecycle generation before the failure.
3. No REF lifecycle detach/rebind sequence precedes the error.
4. Initial and later resize activity completes successfully.
5. Present activity stops first; REF hook-monitor timeout/quarantine occurs later and is therefore an effect, not the initiating failure.
6. The Capcom minidump is produced through the game's fatal-D3D path. It does not prove a direct AV inside `igxess_fg.dll` or the Intel graphics driver.
7. The failure is sensitive to OptiScaler logging being enabled/disabled.

Do not attribute the failure to PR35 shutdown handling unless new evidence directly shows shutdown involvement.

---

## 3. Why logging sensitivity matters

Current logging implementation:

```text
OptiScaler/Logger.cpp
```

Current defaults/config behavior include:

```text
LogLevel = trace
LogAsync = false
```

File logging uses a multithreaded sink and the logger is configured with:

```cpp
shared_logger->flush_on(spdlog::level::trace);
```

Therefore a logging-enabled run introduces substantial synchronous timing perturbation:

- sink locking,
- file writes,
- trace-level flushing,
- cross-thread scheduling changes.

Logging must **not** be used as the fix or as the primary validation mechanism. Final acceptance must be performed with OptiScaler file logging disabled.

---

## 4. Latest-code review: confirmed synchronization facts

### 4.1 `XeFG_Dx12::SetResource()` is already resource-locked

File:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Current code obtains an exclusive per-frame resource lock:

```cpp
auto fIndex = inputResource->frameIndex;
if (fIndex < 0)
    fIndex = GetIndex();

std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
```

It then mutates `_frameResources[fIndex]`, may perform copies/barriers, calls XeFG `D3D12TagFrameResource()`, and finally updates resource-ready state.

Therefore the root-cause statement must **not** be "SetResource has no mutex".

### 4.2 `NewFrame()` also uses the per-frame resource lock

File:

```text
OptiScaler/framegen/IFGFeature_Dx12.cpp
```

Current code:

```cpp
auto fIndex = GetIndex();
std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
_frameResources[fIndex].clear();
```

This protects the map clear itself.

### 4.3 `GetResource()` does not preserve pointer lifetime after return

Current implementation:

```cpp
Dx12Resource* IFGFeature_Dx12::GetResource(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    std::shared_lock<std::shared_mutex> lock(_resourceMutex[index]);

    if (!_frameResources[index].contains(type))
        return nullptr;

    auto& currentIndex = _frameResources[index];
    if (auto it = currentIndex.find(type); it != currentIndex.end())
        return &it->second;

    return nullptr;
}
```

The shared lock is destroyed when `GetResource()` returns, but the caller keeps a raw pointer into the `unordered_map` element.

`XeFG_Dx12::Present()` then dereferences that pointer after the lock has already been released.

This is a real lifetime/synchronization weakness independent of the MHW reproduction. A concurrent `NewFrame()` clear or map mutation can invalidate the returned pointer.

Do **not** broaden Phase 1 into a repository-wide resource API rewrite, but record this as a required Phase 2 hardening item if the ordering A/B succeeds.

### 4.4 `HasResource()` currently reads the resource map without `_resourceMutex`

Current implementation:

```cpp
bool IFGFeature_Dx12::HasResource(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    return _frameResources[index].contains(type);
}
```

Streamline `reportResource()` uses this while deciding which frame index to tag.

This is another genuine synchronization gap, but it should not be mixed into the first A/B unless required to make the A/B compile or behave deterministically.

### 4.5 `Dispatch()` reads frame/resource state outside `_resourceMutex`

`XeFG_Dx12::Dispatch()` directly reads or modifies, among other state:

```text
_resourceReady[fIndex]
_noHudless[fIndex]
_noDistortionField[fIndex]
_frameResources[fIndex]
_camera*
_jitter*
_mvScale*
_reset*
_ftDelta*
_interpolation*
_frameCount / _lastDispatchedFrame
```

Examples include:

```cpp
if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) || ...)
    return false;
```

and:

```cpp
auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
...
SetResource(res);
```

The per-resource mutex therefore does not by itself define a complete frame transaction boundary.

### 4.6 Streamline frame bookkeeping lock is narrower than the XeFG transaction

File:

```text
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
```

`CheckForFrame()` uses:

```cpp
std::scoped_lock lock(_frameBoundaryMutex);
```

but this lock ends when `CheckForFrame()` returns.

After that, `setConstants()` and `reportResource()` continue to mutate/read FG state and call into the XeFG object outside `_frameBoundaryMutex`.

`markPresent()` uses `_frameBoundaryMutex` and then calls:

```cpp
State::Instance().currentFG->SetFrameCount(frameId);
```

There is no common lock joining all of these operations to the XeFG present transaction.

### 4.7 The present path already has a suitable transaction lock

File:

```text
OptiScaler/hooks/FG_Hooks.cpp
```

`FGHooks::FGPresent()` already serializes active FG presentation using `fg->Mutex`:

```cpp
if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
    Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
    !fg->Mutex.isOwnedByCurrentThread(2))
{
    fg->Mutex.lock(2);
    mutexUsed = true;
}

if (willPresent && fg != nullptr)
    fg->Present();

...

result = o_FGSCPresent(...);

...

if (mutexUsed && fg != nullptr)
    fg->Mutex.unlockThis(2);
```

Important: the lock spans not only `fg->Present()/Dispatch()` but also the underlying DXGI Present call and remains held until near the end of the hook.

Resize handling also participates in the same `OwnedMutex` domain (`owner = 6678`). XeFG retirement uses owner `1`.

This makes `fg->Mutex` the correct first A/B synchronization domain for Streamline input ordering.

---

## 5. Corrected root-cause hypothesis

The leading hypothesis is **not** that all XeFG resource access is unprotected.

The corrected hypothesis is:

> Streamline frame-input submission and XeFG Present/Dispatch do not currently share one transaction boundary. Per-resource locking protects parts of `_frameResources`, but frame counters, ready flags, constants, resource-index decisions, and XeFG API calls can still overlap the Present/Dispatch transaction. Heavy synchronous logging changes scheduling enough to mask the race.

Candidate bad ordering:

```text
Thread A — Streamline input
    CheckForFrame()
        _frameBoundaryMutex
    unlock _frameBoundaryMutex

    SetCamera/SetJitter/SetMVScale/SetReset/SetFrameTimeDelta
    reportResource()
        HasResource()              // currently unlocked map read
        SetResource()
            _resourceMutex[index]
            D3D12TagFrameResource()

                         overlap
                           ||
                           \/

Thread B — Present
    fg->Mutex owner 2
    XeFG_Dx12::Present()
        GetIndexWillBeDispatched()
        resource reads
        Dispatch()
            _resourceReady[] reads
            frame constants reads
            TagFrameConstants()
            SetPresentId()
    vendor/DXGI Present
```

This can violate the required happens-before relationship between the complete set of frame inputs and presentation even if individual API calls or individual resource-map mutations are locally synchronized.

---

## 6. Phase 1 — narrow synchronization A/B

### 6.1 Goal

Make Streamline XeFG input entry points participate in the **existing `fg->Mutex` transaction domain** used by `FGPresent()`.

Do not rewrite the resource container in this phase.

### 6.2 Scope

Files expected to change:

```text
OptiScaler/OwnedMutex.h
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
```

Potentially the Streamline header if a small local helper must be declared there.

Do not modify REFramework.

Do not modify PR34/PR35 lifecycle logic.

### 6.3 Add owner-agnostic same-thread ownership query

`OwnedMutex` currently exposes:

```cpp
bool isOwnedByCurrentThread(uint32_t owner) const;
```

Add an overload or equivalent method that answers whether **any** logical owner is held by the current OS thread:

```cpp
bool isOwnedByCurrentThread() const
{
    return owner.load(std::memory_order_acquire) != 0 &&
           ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
}
```

Keep the existing owner-specific overload unchanged.

Reason:

- Streamline input may theoretically be re-entered on a thread that already owns the FG mutex under another logical owner.
- `OwnedMutex` is not recursive.
- blindly calling `lock()` from the same OS thread would deadlock.
- when the same thread already owns the FG mutex, the required serialization already exists and the input scope should not take/unlock ownership itself.

### 6.4 Add a small conditional RAII transaction helper

Use a dedicated logical owner constant, e.g.:

```cpp
namespace
{
constexpr uint32_t kStreamlineInputMutexOwner = 3;

class ScopedStreamlineFGTransaction
{
  public:
    explicit ScopedStreamlineFGTransaction(IFGFeature_Dx12* fg) : _fg(fg)
    {
        if (_fg == nullptr)
            return;

        if (!Config::Instance()->FGUseMutexForSwapchain.value_or_default())
            return;

        if (State::Instance().activeFgOutput != FGOutput::XeFG)
            return;

        if (_fg->Mutex.isOwnedByCurrentThread())
            return;

        _fg->Mutex.lock(kStreamlineInputMutexOwner);
        _owns = true;
    }

    ~ScopedStreamlineFGTransaction()
    {
        if (_owns && _fg != nullptr)
            _fg->Mutex.unlockThis(kStreamlineInputMutexOwner);
    }

    ScopedStreamlineFGTransaction(const ScopedStreamlineFGTransaction&) = delete;
    ScopedStreamlineFGTransaction& operator=(const ScopedStreamlineFGTransaction&) = delete;

  private:
    IFGFeature_Dx12* _fg {};
    bool _owns {};
};
}
```

The exact class name is flexible. The semantics are not.

### 6.5 Apply the transaction at the outer Streamline entry points

The transaction must be acquired **before** `_frameBoundaryMutex` or `_resourceMutex` can be acquired.

Required order:

```text
fg->Mutex
    -> _frameBoundaryMutex
        -> _resourceMutex[index]
```

Never introduce the reverse order.

Apply to:

#### `setConstants()`

Immediately after resolving and validating `fgOutput`:

```cpp
auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(State::Instance().currentFG);
if (fgOutput == nullptr)
    return false;

ScopedStreamlineFGTransaction transaction(fgOutput);

CheckForFrame(fgOutput, frameId);
...
```

This protects:

- frame-boundary advancement,
- `EvaluateState()`,
- camera/jitter/MV/reset/frame-time setters,
- frame-index dependent constant writes,

against concurrent Present/Dispatch.

#### `reportResource()`

Immediately after resolving and validating `fgOutput` and before `CheckForFrame()` / `HasResource()` / `SetResource()`:

```cpp
auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);
if (fgOutput == nullptr || !Config::Instance()->FGEnabled.value_or_default())
    return false;

ScopedStreamlineFGTransaction transaction(fgOutput);

CheckForFrame(fgOutput, frameId);
...
```

This serializes frame-index selection, readiness decisions, resource tagging, and XeFG `D3D12TagFrameResource()` against the Present transaction.

#### `evaluateState()`

Protect this entry point as well when it operates on the active XeFG object. It reads frame counters and may mutate FG state (`FGchanged`, active/paused flow).

Do not leave it as a third unsynchronized Streamline path.

#### `markPresent()`

Change the ordering so the FG transaction is established before `_frameBoundaryMutex`:

Conceptually:

```cpp
void Sl_Inputs_Dx12::markPresent(uint64_t frameId)
{
    auto* fg = reinterpret_cast<IFGFeature_Dx12*>(State::Instance().currentFG);
    ScopedStreamlineFGTransaction transaction(fg);

    std::scoped_lock lock(_frameBoundaryMutex);
    _isFrameFinished = true;
    _lastFrameId = static_cast<uint32_t>(frameId);

    if (fg != nullptr)
        fg->SetFrameCount(frameId);
}
```

Do not acquire `fg->Mutex` while already holding `_frameBoundaryMutex`.

### 6.6 Keep the change XeFG-specific

Do not silently serialize all Streamline + FSR-FG paths in the first A/B.

Gate the transaction to:

```text
activeFgOutput == FGOutput::XeFG
```

This keeps the experiment narrow and prevents unrelated FSR-FG performance or behavior changes from contaminating the result.

### 6.7 Do not add sleeps

Forbidden as a fix or diagnostic substitute:

```cpp
Sleep(...)
std::this_thread::sleep_for(...)
YieldProcessor loops used as timing padding
artificial logger writes/flushes
```

The test must prove correctness through ordering, not delay.

---

## 7. Diagnostics for Phase 1

Do not add heavy per-resource logging that recreates the original heisenbug masking condition.

If diagnostics are needed, keep them minimal and trace-only, for example only on contention or re-entry:

```text
[XeFG][FrameOrder] action = input_wait, entry = reportResource, owner = <current owner>
[XeFG][FrameOrder] action = input_reentrant, entry = setConstants, owner = <current owner>
```

Do not log every successful lock acquisition at info/debug level in production.

The decisive validation signal is **behavior with file logging disabled**, not the diagnostic log.

---

## 8. Phase 1 acceptance test

### Primary test — Intel MHW, logging OFF

Use the same configuration that previously produced E_ABORT 4004.

Required:

```text
LogToFile = false
LogToConsole = false
LogToDebug = false
```

Run at least:

- 5 independent game launches,
- at least 10 minutes of actual gameplay per run,
- include normal menu/gameplay transitions,
- include the same display/resize behavior seen in the failing setup when practical,
- cleanly exit each run.

Pass criteria:

```text
5 / 5 runs:
- no Fatal D3D error (7, E_ABORT, 0x80004004)
- no Capcom crash report generated by this error
- no REF exception dump
- FG remains active and producing frames
- no present hang
- clean process exit
```

If the historical failure rate is lower than 1 per 10 minutes, extend the test duration enough to exceed the prior failure window rather than declaring success early.

### Control — Intel MHW, logging ON

Run 2 sessions with normal Opti logging enabled to ensure the new synchronization does not create a logger-dependent deadlock or severe performance regression.

### Regression — NVIDIA MHW shutdown

PR35 already fixed the NVIDIA shutdown AV by skipping REF pre-retire during confirmed process shutdown.

Verify at least 3 clean exits:

- no shutdown hang,
- no `reframework_crash.dmp`,
- PR35 `ref_pre_retire_skipped` shutdown behavior preserved.

### Regression — Intel MHW shutdown

At least 3 clean exits.

### Additional smoke tests

When available:

- another Capcom + REF + XeFG title,
- a non-REF XeFG title,
- runtime resize / borderless transition,
- FG enable/disable toggle.

### CI

- clang-format clean,
- normal solution/build CI green.

---

## 9. Phase 1 success interpretation

If the logging-OFF E_ABORT disappears after only the Streamline transaction synchronization is added, treat this as strong evidence that the failure was caused by missing happens-before ordering between Streamline frame input and XeFG Present/Dispatch.

Do **not** immediately add Intel-only or MHW-only conditionals. The synchronization rule is architectural and should remain based on the active FG input/output path, not game/vendor name.

If the failure remains unchanged, revert or isolate the A/B before moving to Phase 2. Do not stack speculative fixes until the effect of this synchronization is known.

---

## 10. Phase 2 — resource/frame-state hardening after a successful A/B

Phase 2 is separate from the first diagnostic patch.

The latest-code review exposed synchronization weaknesses that should be cleaned up if Phase 1 confirms an ordering problem.

### 10.1 Replace raw-pointer-after-unlock resource access

Do not keep this API contract long term:

```cpp
Dx12Resource* GetResource(...)
```

where the returned pointer outlives `_resourceMutex` ownership.

Preferred directions:

#### Option A — snapshot accessor

```cpp
std::optional<Dx12Resource> GetResourceSnapshot(FG_ResourceType type, int index = -1)
{
    if (index < 0)
        index = GetIndex();

    std::shared_lock lock(_resourceMutex[index]);

    auto it = _frameResources[index].find(type);
    if (it == _frameResources[index].end())
        return std::nullopt;

    return it->second;
}
```

Callers that only read metadata should use the snapshot.

#### Option B — callback-under-lock accessor

Where pointer identity is required, execute the callback while the shared lock remains owned rather than returning a raw map pointer.

Do not return a borrowed map element after unlocking.

### 10.2 Lock `HasResource()`

At minimum:

```cpp
bool IFGFeature_Dx12::HasResource(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    std::shared_lock lock(_resourceMutex[index]);
    return _frameResources[index].contains(type);
}
```

Review all call sites for lock recursion before merging.

### 10.3 Do not deadlock `Dispatch()` by naively wrapping `_resourceMutex`

`Dispatch()` currently may take an element from `_frameResources[fIndex]` and call `SetResource()`, which itself obtains the exclusive `_resourceMutex[fIndex]`.

Therefore this is forbidden:

```cpp
std::unique_lock lock(_resourceMutex[fIndex]);
// ...
SetResource(...); // self-deadlock
```

If Dispatch needs a wider protected region, snapshot/copy the required state under the lock, release it, then call code paths that reacquire the lock.

### 10.4 Consider a real frame-state transaction abstraction

If Phase 1 proves the architectural ordering issue and the existing `fg->Mutex` becomes too broad or expensive, a later refactor may introduce a dedicated frame-input transaction lock/state snapshot.

That refactor must cover the state as one coherent frame unit:

```text
frame count/index
resource readiness
frame constants
resource metadata
resource-tag completion
present ID
```

Do not introduce a new lock until the Phase 1 A/B proves that ordering is the relevant dimension.

---

## 11. Lock-order rules

The following ordering is mandatory for the first implementation:

```text
FG transaction mutex (`fg->Mutex`)
    -> Streamline `_frameBoundaryMutex`
        -> per-frame `_resourceMutex[index]`
```

Never acquire in reverse order.

Lifecycle locks remain outside this frame-input experiment. Do not pull `_swapchainLifecycleMutex` into frame submission.

Do not hold `_resourceMutex` while waiting for `fg->Mutex`.

Do not hold `_frameBoundaryMutex` while first acquiring `fg->Mutex`.

---

## 12. Non-goals / forbidden changes

Do not in this PR:

- revert PR34,
- revert PR35,
- change REF lifecycle ABI,
- change P7-A live pre-retire ordering,
- change P7-B queue-generation ownership,
- change P7-C final-release ownership,
- special-case `igxess_fg.dll` or `libxess_fg.dll` by caller name,
- special-case Intel/NVIDIA PCI vendor ID,
- special-case Monster Hunter Wilds by game name,
- add arbitrary sleeps,
- force logging on,
- make logging asynchronous and call that the fix,
- rewrite the entire resource container in the same first PR,
- change command queue selection without direct evidence.

---

## 13. Required PR structure

Keep the first PR small and auditable.

Suggested title:

```text
fix: serialize Streamline XeFG frame input with present
```

Suggested scope:

```text
OwnedMutex.h
Streamline_Inputs_Dx12.cpp
(optional) Streamline_Inputs_Dx12.h
```

PR description must include:

1. reproduction: Intel MHW E_ABORT 4004 with Opti logging OFF,
2. why synchronous logging suggests a timing-sensitive race,
3. latest-code finding that `SetResource()` is already per-resource locked,
4. missing cross-transaction ordering between Streamline frame inputs and Present,
5. exact lock order,
6. logging-OFF validation results,
7. NVIDIA shutdown regression result to prove PR35 is preserved.

Do not claim the root cause is confirmed until the logging-OFF A/B passes.

---

## 14. Stop conditions

Stop and report before broadening the patch if any of these occur:

- new deadlock in `setConstants`, `reportResource`, `markPresent`, Present, or ResizeBuffers,
- same-thread mutex re-entry that is not handled by the owner-agnostic current-thread check,
- large frametime regression caused by contention,
- FG input callbacks are invoked from inside the Present transaction in a way that changes required ordering,
- E_ABORT remains at the same rate after the narrow synchronization patch,
- new evidence points to device removal, queue submission failure, or a specific XeFG API error instead.

If one of these occurs, collect the smallest possible targeted evidence before changing architecture.

---

## 15. Definition of done

Phase 1 is complete only when all are true:

1. latest `reframework-0.9` behavior is preserved outside the targeted synchronization,
2. Streamline XeFG input participates in the existing FG transaction mutex,
3. same-thread re-entry cannot self-deadlock the non-recursive `OwnedMutex`,
4. lock order is `FG mutex -> frameBoundary -> resourceMutex`,
5. no sleeps or logging dependencies are introduced,
6. Intel MHW passes the logging-OFF reproduction matrix,
7. NVIDIA MHW shutdown remains fixed,
8. Intel shutdown remains clean,
9. CI/clang-format pass,
10. the PR documents whether the A/B confirmed or rejected the frame-order hypothesis.

Phase 2 resource accessor cleanup should be proposed separately after Phase 1 evidence is available.
