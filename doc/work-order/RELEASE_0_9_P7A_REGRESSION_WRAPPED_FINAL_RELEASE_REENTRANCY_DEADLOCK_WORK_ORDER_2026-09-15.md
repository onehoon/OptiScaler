# Release 0.9 P7-A Regression Fix Work Order — Wrapped Swapchain Final-Release / REF Pre-Retire Reentrancy Deadlock

Date: 2026-09-15

## Status

Implementation work order for a **confirmed shutdown regression** on the current `reframework-0.9` branch.

This is not a speculative robustness item. Monster Hunter: World reproduces a real process-exit hang after P7-A. The game window closes, but the process remains alive and Steam continues to report the game as running.

The regression boundary is confirmed by A/B testing:

```text
26326227939bbd3771d0fe20ee93cf2f75d5532e
P6: fail closed on XeLL lifecycle uncertainty (#30)
    -> normal process exit

b0621c37c4dd07f2bffb74a2b32dc0d3ea03f89f
P7-B: own XeFG D3D12 command queues across lifecycle (#32)
    -> shutdown hang reproduces
```

P7-C is therefore not required for the hang to occur. The first production change between the known-good P6 point and the bad branch that changes the relevant explicit XeFG retirement ordering is:

```text
d78cb589c889da5716e7a85eaf1760bf7e93d4be
P7-A: detach REF before XeFG lifecycle retirement (#31)
```

P7-A remains architecturally necessary, but its implementation missed an **outer wrapper-local lock** that exists above the XeFG lifecycle lock graph.

The required fix is:

> Preserve P7-A's REF-before-FG teardown ordering, but never call the REF pre-retire path while `WrappedIDXGISwapChain4::_localMutex` is held by the wrapper's primary final-release transaction. Reentrant `Release(0)` during that external handoff must be consumed by the already-owning finalizer and must never start a second teardown or `delete this`.

---

# 1. Frozen implementation baseline

## 1.1 Target repository and branch

Repository:

```text
onehoon/OptiScaler
```

Branch:

```text
reframework-0.9
```

Latest branch HEAD reviewed for this work order:

```text
e9ca4685f807ae760a9a2334b5c9620b81420532
docs: add A2 REF factory lock-order work order
```

Implement on the latest `reframework-0.9` branch, not on the historical P6 bisect point.

`26326227...` is only the last-known-good regression boundary.

All later production hardening that exists at current HEAD must be preserved, including:

- P7-B queue ownership/generation retirement;
- P7-C thread-aware `OwnedMutex` ownership;
- P5-B/P6 fail-closed lifecycle rules;
- current A1/A2 compatibility assumptions unless this confirmed regression specifically requires a documentation correction.

## 1.2 Current production files relevant to the regression

Primary files:

```text
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/wrapped/wrapped_swapchain.h
```

Reference files whose semantics must be preserved:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/OwnedMutex.h
OptiScaler/hooks/FG_Hooks.cpp
```

Expected production fix should be narrow. Prefer changing only the wrapped swapchain files unless concrete implementation constraints require a small XeFG helper adjustment.

Do not redesign generic REF synchronization in this fix.

---

# 2. Confirmed failure signature

The failing Monster Hunter: World shutdown log ends with the following sequence:

```text
XeFG Log: Destroy Context called
WrappedIDXGISwapChain4::Release Count: 1, caller: libxess_fg.dll
WrappedIDXGISwapChain4::Release Count: 0, caller: libxess_fg.dll
MenuOverlayDx::CleanupRenderTarget
WrappedIDXGISwapChain4::AddRef Count: 1, caller: dinput8.dll
WrappedIDXGISwapChain4::Release Count: 0, caller: dinput8.dll
<no further OptiScaler progress>
```

Important observations:

1. The first `Release(0)` comes from the XeFG/vendor side.
2. The wrapper enters its final-release cleanup path.
3. During that finalization, REFramework (`dinput8.dll`) temporarily acquires and releases a COM reference on the same wrapper.
4. The second `Release(0)` is logged.
5. There is no subsequent wrapper cleanup completion, queue retirement, real-swapchain release, or process exit.

The corresponding REFramework lifecycle log reaches semantic XeFG detach completion (`reason = proxy_retire`) before the process remains stuck. This is consistent with temporary COM keepalive cleanup occurring while the OptiScaler call into the REF pre-retire export is still unwinding/returning.

This is a deterministic lock/reentrancy failure path observed during normal game shutdown, not a theoretical scheduler race.

---

# 3. Source-proven deadlock

## 3.1 Current wrapper final release owns `_localMutex`

Current `WrappedIDXGISwapChain4::Release()` does:

```cpp
ULONG ret = InterlockedDecrement(&_refcount);
...
if (ret == 0)
{
#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 999);
#endif

    ...
    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    ...
    releaseCompleted = fg->ReleaseSwapchain(_handle);
    ...
    const auto refCount = real != nullptr ? real->Release() : 0;
    ...
    delete this;
}
```

`USE_LOCAL_MUTEX` is enabled in `wrapped_swapchain.h`.

Therefore the wrapper holds `_localMutex` across `fg->ReleaseSwapchain(...)`.

## 3.2 `_localMutex` is non-recursive

Current `OwnedMutex` is backed by:

```cpp
std::shared_mutex mtx;
```

and `OwnedLockGuard` unconditionally calls:

```cpp
_mutex.lock(owner_id);
```

The P7-C thread-aware owner metadata does not make the underlying mutex recursive.

A same-thread or cross-thread second wrapper final-release path cannot reacquire the already-held `_localMutex`.

## 3.3 P7-A added REF pre-retire to normal explicit release

Current `XeFG_Dx12::ReleaseSwapchain(HWND)` does:

```cpp
std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
...
if (!PrepareREFForSwapchainRetire(
        State::Instance().currentFGSwapchain,
        hwnd,
        "explicit_release"))
{
    return false;
}

return ReleaseSwapchainLocked(hwnd);
```

This P7-A ordering is correct with respect to the XeFG FG mutex:

```text
REF pre-retire
    -> REF returns
    -> ReleaseSwapchainLocked
    -> optional FG Mutex owner 1
```

But the actual full call graph from wrapper final release is:

```text
WrappedIDXGISwapChain4::_localMutex owner 999
    -> XeFG _swapchainLifecycleMutex
       -> REF pre-retire / dinput8.dll
          -> same WrappedIDXGISwapChain4 AddRef
          -> same WrappedIDXGISwapChain4 Release(0)
             -> waits for WrappedIDXGISwapChain4::_localMutex owner 999
```

The outer wrapper cannot release `_localMutex` until `fg->ReleaseSwapchain()` returns.

The REF call cannot return until its temporary COM release completes.

The nested wrapper `Release(0)` cannot complete because the outer wrapper still owns `_localMutex`.

That is the confirmed shutdown deadlock.

---

# 4. Regression interpretation

P7-A solved one real lock-order problem but did not include the wrapper-local lock in the full lock graph.

P7-A's intended ordering remains valid:

```text
REF lifecycle detach
    before
XeFG FG teardown mutex / vendor Destroy
```

The missing rule is:

```text
Wrapped swapchain finalization local mutex
    must NOT be held across
REF pre-retire or another external COM/lifecycle call that can AddRef/Release the same wrapper
```

Do not revert P7-A globally.

A global revert would re-open the original REF-monitor ↔ FG-mutex retirement/recreation problem that P7-A was created to close.

The fix must remove only the newly exposed wrapper-finalization lock cycle.

---

# 5. Required implementation design

## 5.1 Add a wrapper-local terminal finalization owner

Add a per-wrapper atomic terminal flag, for example:

```cpp
std::atomic_bool _finalReleaseInProgress { false };
```

Location:

```text
WrappedIDXGISwapChain4
```

This must be instance-local, not global and not `thread_local`.

Semantics:

- it is set only when a real primary final-release path has been selected;
- the preserve-swapchain path must remain able to restore the reference and return without entering terminal finalization;
- once terminal finalization is claimed, it is never reset because that wrapper is on its destruction path;
- only the primary finalizer may perform lifecycle teardown, release the underlying real swapchain, or `delete this`.

## 5.2 Reentrant `Release(0)` must not enter teardown

When a nested `Release()` reaches zero while `_finalReleaseInProgress == true`, it must:

```text
log the reentrant final release
return 0
```

and must **not**:

- acquire `_localMutex`;
- call `MenuOverlayDx::CleanupRenderTarget`;
- mutate current FG generation state;
- call `fg->ReleaseSwapchain`;
- release `_real`;
- retire queue generation;
- call `delete this`.

This converts the observed REF temporary `AddRef -> Release` pair into a harmless balanced reentrant COM operation owned by the existing primary finalizer.

Recommended diagnostic shape:

```text
[DXGI][WrapperLifecycle] action = reentrant_final_release_consumed,
wrapper = ..., generation = ..., thread = ...
```

Do not make the nested path wait for the primary finalizer. It is running inside the external handoff that the primary finalizer itself is waiting to return from; waiting would recreate the deadlock through a different primitive.

## 5.3 Do not claim terminal finalization before the existing preserve path

Current special handling may do:

```cpp
if (ret == 0 && ... FGPreserveSwapChain ... && !isShuttingDown)
{
    AddRef();
    return ret;
}
```

Preserve that behavior.

The terminal guard must not permanently mark a wrapper that is intentionally resurrected by the supported preserve-swapchain path.

A practical structure is:

```text
InterlockedDecrement
    -> log
    -> if finalization already active and this call reached zero:
         consume reentrant zero-release
    -> existing preserve-swapchain path
    -> if not zero: return
    -> atomically claim primary finalizer
    -> perform terminal teardown
```

Use compare/exchange or equivalent atomic ownership. Do not use an unprotected plain boolean.

---

# 6. Split the wrapper local-mutex scope around the external XeFG/REF retirement call

## 6.1 Preserve the existing state-detach ordering as much as possible

The current final-release block performs wrapper-local state detachment before `fg->ReleaseSwapchain()`:

```text
snapshot current wrapper/generation
CleanupRenderTarget
clear State::currentSwapchain alias when this
clear State::currentWrappedSwapchain alias when this
exchange _real -> local variable
clear State::currentRealSwapchain alias when matching
snapshot fg/current FG proxy
calculate canReleaseCurrentFgLifecycle
```

Keep these semantics unless a concrete source dependency requires otherwise.

The important change is the **mutex lifetime**, not a broad reorder of lifecycle ownership.

## 6.2 `_localMutex` must be released before `fg->ReleaseSwapchain()`

Required shape:

```text
primary finalizer claimed
    -> lock wrapper _localMutex
       -> snapshot/detach wrapper-local and global aliases
       -> capture real pointer locally
       -> decide whether current FG lifecycle must retire
    -> unlock wrapper _localMutex

    -> call fg->ReleaseSwapchain(hwnd)
       -> P7-A REF pre-retire remains intact
       -> REF may AddRef/Release this wrapper
       -> nested Release(0) sees finalization owner and returns without locking
       -> REF handoff completes
       -> XeFG lifecycle teardown completes

    -> optional short wrapper _localMutex scope for pure post-retire metadata updates
    -> final external real-swapchain Release outside wrapper _localMutex
    -> primary finalizer deletes wrapper exactly once
```

The critical invariant is:

> No call path that can enter the REF pre-retire export may execute while wrapper `_localMutex` owner 999 is held.

## 6.3 Prefer no wrapper local lock around the final underlying COM `real->Release()`

The current code also calls:

```cpp
real->Release();
```

inside the wrapper finalization lock scope.

Once the finalization owner exists, there is no benefit in holding the wrapper-local mutex across this final external COM call.

Move the underlying `real->Release()` outside `_localMutex` as part of the same narrow cleanup if doing so does not require unrelated refactoring.

Do not broaden this work into arbitrary lock removal from normal `Present`, `ResizeBuffers`, `ResizeBuffers1`, or other live-object operations.

## 6.4 `delete this` belongs only to the primary finalizer

There must be exactly one destruction owner.

The reentrant REF/dinput8 `Release(0)` must never call `delete this`.

The primary finalizer remains responsible for:

- lifecycle release result handling;
- current FG proxy/generation cleanup;
- P7-B queue-generation retirement;
- underlying real swapchain Release;
- final wrapper destruction.

---

# 7. Preserve P7-A, P7-B and P7-C semantics

## 7.1 Preserve P7-A

Do not remove these behaviors:

```text
CreateSwapchain old-lifecycle recreation
    -> REF pre-retire before ReleaseSwapchainLocked

CreateSwapchain1 old-lifecycle recreation
    -> REF pre-retire before ReleaseSwapchainLocked

explicit ReleaseSwapchain
    -> REF pre-retire before ReleaseSwapchainLocked

final XeFG public proxy release
    -> REF pre-retire before final proxy consumption / FG teardown
```

The bug is not that REF is detached before teardown.

The bug is that the wrapper finalizer currently enters that external handoff while holding an unrelated non-recursive wrapper-local mutex.

## 7.2 Preserve P7-B

Do not change:

- `_ownedGameCommandQueue` ownership;
- generation-scoped queue publication;
- `FGHooks::RetireQueueGeneration` policy;
- queue/fence state retirement ordering.

A successful wrapper finalization must still reach the existing queue retirement path.

A key post-fix log expectation is that shutdown progresses far enough to emit:

```text
[XeFG][QueueLifecycle] action = retired
```

when applicable.

## 7.3 Preserve P7-C

Do not modify `OwnedMutex` to fix this regression.

Specifically do not:

- replace `std::shared_mutex` with a recursive mutex;
- weaken thread-aware owner validation;
- allow arbitrary cross-thread unlock;
- special-case owner `999` inside `OwnedMutex`.

Making `_localMutex` recursive would only allow the nested `Release(0)` to continue into duplicate teardown and can convert the hang into double-release/use-after-free/double-delete corruption.

This regression must be fixed at the wrapper finalization ownership boundary.

---

# 8. Do not suppress or alter REF COM behavior

Do not modify REFramework merely to stop its temporary AddRef/Release pair.

The pre-retire export is allowed to hold a temporary COM keepalive while it validates and detaches the current lifecycle.

OptiScaler must tolerate that valid callback/reentrancy behavior without holding an incompatible wrapper-local lock.

Do not:

- special-case `dinput8.dll` by module name;
- ignore REF releases based on caller address;
- patch REF's export ABI;
- remove REF keepalive logic;
- add sleeps, polling, timeouts, or retry loops around the deadlock.

The fix must be ownership/ordering based, not caller-name based.

---

# 9. Recommended code skeleton

The exact code may differ, but the resulting control flow should look like this:

```cpp
ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);
    LOG_TRACE(...);

    if (ret == 0 && _finalReleaseInProgress.load(std::memory_order_acquire))
    {
        LOG_DEBUG("[DXGI][WrapperLifecycle] action = reentrant_final_release_consumed, wrapper = {:X}",
                  (size_t) this);
        return 0;
    }

    // Existing supported preserve behavior remains before terminal ownership.
    if (ret == 0 && ShouldPreserveSwapchain())
    {
        AddRef();
        return ret;
    }

    if (ret != 0)
        return ret;

    bool expected = false;
    if (!_finalReleaseInProgress.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        LOG_DEBUG("[DXGI][WrapperLifecycle] action = reentrant_final_release_consumed, wrapper = {:X}",
                  (size_t) this);
        return 0;
    }

    auto& state = State::Instance();

    IFGFeature_Dx12* fg = nullptr;
    IUnknown* fgProxyBeforeRelease = nullptr;
    IDXGISwapChain* real = nullptr;
    uint64_t wrapperGeneration = 0;
    bool canReleaseCurrentFgLifecycle = false;

    {
#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif
        // Keep the current state-detach/snapshot semantics here.
        // No REF/XeFG retirement call from this scope.
        // Exchange _real into local `real` before leaving the scope if preserving
        // current ordering requires it.
    }

    bool releaseCompleted = false;

    // CRITICAL: wrapper _localMutex is not held here.
    if (canReleaseCurrentFgLifecycle)
    {
        if (fg->Mutex.getOwner() == 1)
        {
            LOG_WARN(... existing deferred diagnostic ...);
        }
        else
        {
            releaseCompleted = fg->ReleaseSwapchain(_handle);
        }
    }

    {
#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif
        // Pure local/global metadata reconciliation only.
        // Do not call REF or another external COM lifecycle from this scope.
    }

    // Prefer outside wrapper local lock.
    const auto realRefCount = real != nullptr ? real->Release() : 0;
    LOG_DEBUG(...);

    delete this;
    return 0;
}
```

This is a design sketch, not a literal patch.

The implementation must preserve all existing generation identity checks and only move the external lifecycle call outside the wrapper-local lock.

---

# 10. Failure handling and fail-closed behavior

The new finalization owner is terminal for that wrapper.

Do not reset `_finalReleaseInProgress` after a failed XeFG lifecycle release and then allow another `Release(0)` to become a second finalizer.

Current wrapper behavior already proceeds with local wrapper destruction even when `fg->ReleaseSwapchain()` reports failure, while logging the lifecycle failure. Preserve or deliberately improve that behavior only if supported by the existing branch contract; do not silently create a second finalization attempt.

If implementation discovers that a failed REF handoff requires the wrapper to remain physically alive, stop and split that as a separate lifecycle-policy change. Do not improvise a resurrection state machine in this regression fix.

The regression fix should remain focused on the confirmed shutdown deadlock.

---

# 11. Required diagnostics

Add only low-volume lifecycle diagnostics.

Required events:

```text
[DXGI][WrapperLifecycle] action = final_release_begin
[DXGI][WrapperLifecycle] action = reentrant_final_release_consumed
[DXGI][WrapperLifecycle] action = final_release_fg_retire_begin
[DXGI][WrapperLifecycle] action = final_release_fg_retire_complete
[DXGI][WrapperLifecycle] action = final_release_complete
```

Include where useful:

```text
wrapper pointer
wrapper generation
current FG generation
thread id
releaseCompleted
```

Do not add per-frame logging.

The implementation should make it obvious from logs that:

```text
outer wrapper finalizer claimed ownership
    -> REF temporary AddRef/Release occurred
    -> nested zero-release was consumed without local-lock acquisition
    -> REF pre-retire returned
    -> XeFG queue/lifecycle retired
    -> real swapchain released
    -> primary wrapper finalizer completed
```

---

# 12. Validation matrix

## 12.1 Mandatory regression test — Monster Hunter: World

Configuration:

```text
fork REFramework + OptiScaler reframework-0.9
XeFG active
same configuration that reproduces the current shutdown hang
```

Run at least 3 clean launch -> gameplay -> normal exit cycles.

Required result for every run:

```text
game window closes
MonsterHunterWorld.exe exits
Steam returns from Running to normal state
no manual process kill required
```

Required log progression:

```text
primary wrapper final release begins
REF proxy-retire handoff runs
reentrant dinput8.dll AddRef/Release may occur
reentrant Release(0) is consumed
REF pre-retire completes/returns
XeFG teardown continues
queue generation retires when applicable
real swapchain release completes
wrapper final release completes
```

There must be no terminal log ending immediately after:

```text
WrappedIDXGISwapChain4::Release Count: 0, caller: dinput8.dll
```

## 12.2 Capcom REF compatibility smoke test

Test at least one additional supported Capcom title that uses the same REF + XeFG integration path.

Suggested candidates:

```text
Dragon's Dogma 2
or another currently supported fork-REF Capcom title
```

Check:

- startup;
- XeFG activation;
- resize / borderless or alt-tab transition if normally supported;
- normal game exit;
- no persistent Steam Running state.

## 12.3 Non-REF XeFG smoke test

Run one XeFG title without REFramework.

Required:

- no behavior change during normal swapchain final release;
- no false reentrant-final-release diagnostic under ordinary use;
- no queue/generation leak;
- clean process exit.

## 12.4 Preserve-swapchain path

If `FGPreserveSwapChain` is enabled in a supported scenario, verify that an ordinary zero-release handled by the existing preserve logic does not permanently set `_finalReleaseInProgress`.

The wrapper must remain usable after the preserved zero-release exactly as before.

## 12.5 Resize / recreation regression check

P7-A recreate behavior must remain unchanged:

```text
REF pre-retire
    -> ReleaseSwapchainLocked
    -> recreate
```

Verify there is no regression in:

- `CreateSwapchain` replacement;
- `CreateSwapchain1` replacement;
- `ResizeBuffers` / `ResizeBuffers1` lifecycle behavior;
- P7-B command queue generation cleanup.

---

# 13. Acceptance criteria

The PR is ready only when all of the following are true:

1. Current `reframework-0.9` HEAD is the implementation base.
2. `26326227` remains documented only as the known-good regression boundary; no later hardening is reverted wholesale.
3. Wrapper terminal finalization has a single per-instance owner.
4. Reentrant `Release(0)` during primary finalization does not acquire `_localMutex` and cannot run duplicate teardown or `delete this`.
5. `fg->ReleaseSwapchain()` is no longer called while wrapper `_localMutex` owner 999 is held.
6. P7-A REF-before-FG teardown ordering remains intact.
7. P7-B queue ownership/generation retirement remains intact.
8. P7-C thread-aware `OwnedMutex` behavior remains intact.
9. No recursive-mutex conversion is used as the fix.
10. No caller-module (`dinput8.dll`) special case is used.
11. MHW exits normally in repeated runs and Steam no longer remains stuck in Running.
12. At least one additional REF Capcom smoke test and one non-REF XeFG smoke test pass.
13. Logs prove the nested REF release is consumed and the primary finalizer reaches completion.

---

# 14. Explicit non-goals

Do not combine this regression fix with:

- A2 REF factory-monitor implementation;
- new REF export ABI versions;
- general COM wrapper redesign;
- generic DXGI wrapper refcount modernization;
- P7-B queue changes;
- P7-C mutex changes;
- XeLL policy changes;
- Intel XeFG result-code policy changes;
- Streamline lifecycle redesign;
- broad shutdown-state-machine work.

If another issue is discovered while implementing this work order, document it separately unless it directly blocks this confirmed regression fix.

---

# 15. Documentation follow-up

After the production fix is validated, update any compatibility/audit text that currently treats the P7-A ordering as fully sufficient.

The corrected statement should be:

> P7-A's REF-before-FG-mutex ordering is required and remains correct, but wrapper final-release callers must not hold `WrappedIDXGISwapChain4::_localMutex` across that external REF handoff. The wrapper finalizer requires its own reentrancy ownership rule.

Do not rewrite A2 or other audit documents as part of the implementation PR unless needed to remove a directly false acceptance condition.

---

# 16. Recommended PR title

```text
fix: avoid REF reentrant deadlock during wrapped XeFG final release
```

Recommended commit scope:

```text
wrapped_swapchain: split final-release lock around XeFG retire handoff
```

The intended patch should be small, source-proven, and limited to the real shutdown failure path demonstrated above.
