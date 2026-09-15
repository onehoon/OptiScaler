# Release 0.9 — XeFG Process-Shutdown REF Pre-Retire AV Work Order

**Status:** Implementation work order  
**Target branch:** `reframework-0.9`  
**Reviewed base:** `ac1e48fa09cd282bbc9e8d7caa91682b0bb90b18` (`fix: avoid REF reentrant deadlock during wrapped XeFG final release`)  
**Date:** 2026-09-15  

## 1. Objective

Fix the post-PR34 Monster Hunter Wilds shutdown crash without regressing the PR34 deadlock fix, P7-A lifecycle ordering during normal runtime, P7-B queue/generation ownership, P7-C mutex ownership, or Intel XeFG shutdown.

The first implementation must be a **minimal process-shutdown semantic correction**, not a broad runtime rewrite:

> When OptiScaler is already in process shutdown, `XeFG_Dx12::ReleaseSwapchain()` must not call the external REFramework pre-retire handoff before entering the existing shutdown-aware `ReleaseSwapchainLocked()` path.

This restores the pre-P7-A `ReleaseSwapchain()` shutdown behavior while preserving P7-A for live runtime retirement/recreation.

Do **not** revert PR34.

---

## 2. Confirmed regression history

### Last known good before P7-A

Commit:

```text
26326227939bbd3771d0fe20ee93cf2f75d5532e
```

Monster Hunter Wilds shut down normally.

At this revision, `XeFG_Dx12::ReleaseSwapchain(HWND)` acquired `_swapchainLifecycleMutex` and directly called `ReleaseSwapchainLocked(hwnd)`. It did **not** call REF pre-retire from the ordinary explicit-release path.

### P7-A

P7-A added:

```cpp
if (!PrepareREFForSwapchainRetire(
        State::Instance().currentFGSwapchain,
        hwnd,
        "explicit_release"))
    return false;

return ReleaseSwapchainLocked(hwnd);
```

That ordering is required for normal/live lifecycle retirement because REF must detach before Opti enters the XeFG teardown mutex/retirement section.

However, the new callback also runs when the wrapped internal XeFG swapchain reaches its final COM release during process termination.

### Before PR34

The shutdown path deadlocked because REF pre-retire temporarily `AddRef`/`Release`d the same wrapped swapchain while `WrappedIDXGISwapChain4::Release()` held its non-recursive `_localMutex`.

### PR34 / current base

PR34 (`ac1e48fa`) correctly fixed that deadlock by:

- adding terminal final-release ownership (`_finalReleaseInProgress`),
- consuming reentrant zero releases,
- snapshotting wrapper state under `_localMutex`,
- performing external FG retirement outside `_localMutex`,
- revalidating generation/aliases afterward,
- preserving one-time finalization/delete semantics.

The deadlock is gone and must remain fixed.

---

## 3. New NVIDIA shutdown failure after PR34

Test data:

```text
C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더 (2)
```

OptiScaler build:

```text
v0.9.5-pre4 (ac1e48fa)
```

Observed shutdown sequence:

```text
23:23:28.835181  XeFG Log: Destroy Context called
23:23:28.837635  WrappedIDXGISwapChain4::Release Count: 1, caller: libxess_fg.dll
23:23:28.845272  WrappedIDXGISwapChain4::Release Count: 0, caller: libxess_fg.dll
23:23:28.845393  [DXGI][WrapperLifecycle] action = final_release_begin
23:23:28.845413  [DXGI][WrapperLifecycle] action = final_release_fg_retire_begin
23:23:28.846208  WrappedIDXGISwapChain4::AddRef Count: 1, caller: dinput8.dll
23:23:28.849313  WrappedIDXGISwapChain4::Release Count: 0, caller: dinput8.dll
23:23:28.849323  [DXGI][WrapperLifecycle] action = reentrant_final_release_consumed
23:23:28.888055  [XeFG][Lifecycle] action = ref_pre_retire_complete,
                  trigger = explicit_release, outcome = safe_detached
23:23:28.888096  [XeFG][QueueLifecycle] action = retired
23:23:28.888110  [DXGI][WrapperLifecycle] action = final_release_fg_retire_complete
23:23:28.888125  [FG][QueueLifecycle] action = clear_generation
23:23:28.905885  Real swapchain released, refCount: 0
23:23:28.905901  [DXGI][WrapperLifecycle] action = final_release_complete
23:23:28.906xxx REFramework catches 0xC0000005 in libxess_fg.dll
```

This proves PR34's reentrant-final-release protection itself is functioning as designed.

### Minidump evidence

`reframework_crash.dmp` was captured and inspected.

Confirmed exception state:

```text
Exception: 0xC0000005
Module:    libxess_fg.dll
RIP:       libxess_fg.dll + 0x22a140
RCX:       0x1c8be19d0
RSI:       0x1c8be19d0
RDI:       0x1c8be19d0
RAX:       0x6168735f70756f72
```

The instruction at `libxess_fg.dll + 0x22a140` is an indirect `jmp rax` thunk.

`RAX = 0x6168735f70756f72` is string-like data (little-endian ASCII contains `roup_sha`), not a valid executable target. This is strong evidence of stale/corrupted internal dispatch/lifetime state rather than a REF exception-handler failure.

The public XeFG proxy at failure time was:

```text
public proxy:       0x1c8be19d0
internal wrapper:   0x1c8cc0d70
XeFG context:       0x18fb017b0
internal_same:      false
```

REFramework only reports/captures the exception. The faulting module is `libxess_fg.dll`.

---

## 4. Intel control result

Test data:

```text
C:\GoogleDrive\ref-xefg\Release-09\mhw\Intel
```

Same OptiScaler build:

```text
v0.9.5-pre4 (ac1e48fa)
```

Intel follows the same structural final-release sequence:

```text
igxess_fg.dll -> WrappedIDXGISwapChain4::Release(0)
final_release_begin
final_release_fg_retire_begin
REF AddRef / Release(0)
reentrant_final_release_consumed
REF LifecycleDetach complete
ref_pre_retire_complete
QueueLifecycle retired
clear_generation
real swapchain refCount = 0
final_release_complete
```

Intel then continues without an exception or crash dump.

Intel identities:

```text
public proxy:       0x5cbf2270
internal wrapper:   0x6e88df60
context:            0x7231b1c0
internal_same:      false
```

Intel uses the same public `libxess_fg.dll` API layer/version (`v1.3.1`) but loads the native implementation modules including `igxess_fg.dll` and `igxell.dll`.

Therefore:

- this is **not** proof that P7-A ordering is universally invalid;
- this is **not** evidence that PR34's terminal-owner guard is broken;
- the NVIDIA/generic `libxess_fg` path is less tolerant of the process-shutdown REF pre-retire sequence than the Intel native path.

The fix should nevertheless be based on **shutdown ownership semantics**, not DLL caller-name special cases.

---

## 5. Latest-code review

### 5.1 `WrappedIDXGISwapChain4::Release()`

File:

```text
OptiScaler/wrapped/wrapped_swapchain.cpp
```

PR34 currently performs three phases:

1. terminal-owner claim and state snapshot under `_localMutex`,
2. `fg->ReleaseSwapchain(_handle)` outside `_localMutex`,
3. generation/alias revalidation under `_localMutex`, followed by real swapchain release and one-time delete.

Keep this architecture.

Do not move `fg->ReleaseSwapchain()` back under `_localMutex`.

### 5.2 `XeFG_Dx12::ReleaseSwapchain()`

File:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Current code always runs REF pre-retire:

```cpp
bool XeFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock())
    {
        LOG_WARN("[XeFG][Lifecycle] action = release_swapchain_deferred, "
                 "reason = lifecycle_transaction_in_progress");
        return false;
    }

    if (!PrepareREFForSwapchainRetire(
            State::Instance().currentFGSwapchain,
            hwnd,
            "explicit_release"))
        return false;

    return ReleaseSwapchainLocked(hwnd);
}
```

### 5.3 `ReleaseSwapchainLocked()` already has shutdown ownership semantics

Current code intentionally does not destroy the XeFG swapchain context once OptiScaler is shutting down:

```cpp
if (!State::Instance().isShuttingDown &&
    (_swapChainContext != nullptr || XeLLProxy::ContextRecreationBlocked()))
{
    if (!DestroySwapchainContext())
        ...
}

_swapChainContext = nullptr;
ReleaseObjects();
```

`DestroySwapchainContext()` also immediately returns when `isShuttingDown` is true.

This means current code already treats process shutdown as a vendor/process-owned teardown for the XeFG context itself.

The inconsistency is that `ReleaseSwapchain()` still performs an external REF semantic pre-retire callback immediately before entering that shutdown-aware path.

### 5.4 `State::isShuttingDown`

`State::isShuttingDown` is set in `DLL_PROCESS_DETACH` in:

```text
OptiScaler/dllmain.cpp
```

Existing FG code already uses this flag to bypass special handling during shutdown, including `FGHooks::hkFGRelease()` and `FGPresent()`.

Do not create a second generic shutdown flag unless instrumentation proves this one is false at the failing final-release point.

### 5.5 Public-proxy release already bypasses custom retirement on shutdown

`FGHooks::hkFGRelease()` currently does:

```cpp
if (skipReleaseChecks ||
    State::Instance().currentFGSwapchain != This ||
    State::Instance().isShuttingDown)
{
    return o_FGRelease(This);
}
```

Therefore the public proxy already follows vendor-owned release semantics during process shutdown.

The internal wrapped swapchain path should not reintroduce an external REF pre-retire transaction during that same process-shutdown phase.

---

## 6. Required implementation — Phase 1

### 6.1 Add proof logging first

Before changing behavior, add a single structured log at the beginning of the current `ReleaseSwapchain()` transaction:

```cpp
auto& state = State::Instance();

LOG_DEBUG(
    "[XeFG][ShutdownOwnership] action = evaluate, trigger = explicit_release, "
    "shutting_down = {}, proxy = {:X}, context = {:X}, thread = {}",
    state.isShuttingDown,
    (size_t) state.currentFGSwapchain,
    (size_t) _swapChainContext,
    GetCurrentThreadId());
```

The MHW NVIDIA reproducer is expected to show:

```text
shutting_down = true
```

at the terminal internal-wrapper release.

If it shows `false`, **do not add caller-name or GPU-vendor heuristics**. Stop the behavioral change and report the log because the ownership signal must then be moved earlier/propagated explicitly.

### 6.2 Skip only REF pre-retire during confirmed process shutdown

If `state.isShuttingDown == true`, do not call `PrepareREFForSwapchainRetire()` from the ordinary `ReleaseSwapchain()` path.

Recommended minimal shape:

```cpp
bool XeFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock())
    {
        LOG_WARN("[XeFG][Lifecycle] action = release_swapchain_deferred, "
                 "reason = lifecycle_transaction_in_progress");
        return false;
    }

    auto& state = State::Instance();

    LOG_DEBUG(
        "[XeFG][ShutdownOwnership] action = evaluate, trigger = explicit_release, "
        "shutting_down = {}, proxy = {:X}, context = {:X}, thread = {}",
        state.isShuttingDown,
        (size_t) state.currentFGSwapchain,
        (size_t) _swapChainContext,
        GetCurrentThreadId());

    if (state.isShuttingDown)
    {
        LOG_INFO(
            "[XeFG][ShutdownOwnership] action = ref_pre_retire_skipped, "
            "trigger = explicit_release, reason = process_shutdown, "
            "proxy = {:X}, context = {:X}",
            (size_t) state.currentFGSwapchain,
            (size_t) _swapChainContext);
    }
    else if (!PrepareREFForSwapchainRetire(
                 state.currentFGSwapchain,
                 hwnd,
                 "explicit_release"))
    {
        return false;
    }

    return ReleaseSwapchainLocked(hwnd);
}
```

### Why this is the required first patch

It is deliberately narrow:

- live runtime recreation still performs P7-A REF pre-retire;
- ordinary non-shutdown explicit release still performs P7-A REF pre-retire;
- final-proxy release logic is unchanged;
- PR34 wrapper terminal ownership is unchanged;
- the existing shutdown-aware `ReleaseSwapchainLocked()` behavior is retained;
- no vendor-specific branch is introduced;
- the process-shutdown behavior of ordinary `ReleaseSwapchain()` moves back toward the known-good P6 behavior.

At P6 (`26326227`), ordinary `ReleaseSwapchain()` directly called `ReleaseSwapchainLocked()` without REF pre-retire, and MHW shut down normally.

---

## 7. Do not change these paths in Phase 1

### 7.1 Do not modify `ReleaseSwapchainFromFinalProxyRelease()`

The observed crash comes from the wrapped internal swapchain final-release path using ordinary `ReleaseSwapchain()`.

Also, `FGHooks::hkFGRelease()` already forwards public-proxy release directly to the vendor when `isShuttingDown` is true.

Do not broaden this patch into the final-proxy path without new evidence.

### 7.2 Do not skip `ReleaseSwapchainLocked()` in Phase 1

The first experiment is specifically:

```text
skip external REF pre-retire during process shutdown
+ keep existing shutdown-aware Opti cleanup
```

Do not simultaneously skip the entire XeFG retirement transaction, because that would prevent us from identifying which part fixes the AV.

### 7.3 Do not change Intel/native-runtime behavior based on module names yet

Intel currently exits normally.

Do not add code such as:

```cpp
if (GetModuleHandleW(L"igxess_fg.dll") == nullptr) ...
```

in the Phase 1 production path.

Runtime-module classification is a fallback only if a process-shutdown semantic fix causes an Intel regression or if the NVIDIA crash survives Phase 1.

---

## 8. Phase 2 fallback — only if Phase 1 still crashes

If all of the following are true:

1. logs prove `shutting_down = true`,
2. REF pre-retire is skipped,
3. NVIDIA still crashes in `libxess_fg.dll` after `final_release_complete`,

then the remaining conflict is likely the entire Opti explicit lifecycle retirement initiated from the vendor's internal wrapped swapchain final release.

At that point, implement an explicit **vendor-owned shutdown retirement result** rather than returning a misleading generic `bool`.

Suggested design direction:

```cpp
enum class XeFGSwapchainReleaseResult
{
    Released,
    VendorOwnedShutdown,
    Deferred,
    Failed,
};
```

Add a dedicated wrapped-final-release entry point or release-origin parameter rather than relying on `_ReturnAddress()` / caller DLL names.

Example direction only:

```cpp
enum class XeFGSwapchainReleaseOrigin
{
    Explicit,
    WrappedFinalRelease,
    FinalProxyRelease,
};
```

For:

```text
origin == WrappedFinalRelease
&& State::Instance().isShuttingDown
```

allow the vendor/process teardown to own the XeFG context/public proxy lifetime while Opti performs only safe local metadata cleanup.

Requirements for Phase 2:

- wrapper terminal finalization still occurs exactly once;
- no call into REF or XeFG vendor teardown from the internal wrapper final release;
- stale generation/queue aliases are cleared only when identity still matches;
- no newer lifecycle may be retired by an old wrapper;
- do not manufacture COM ownership from tracking aliases;
- do not call `Release()` on `State::*Swapchain` aliases merely to clean metadata.

Do not implement Phase 2 unless Phase 1 fails with evidence.

---

## 9. Optional runtime classification if later required

If evidence eventually requires different shutdown policy for native Intel vs generic/non-Intel XeFG runtime, classify the **runtime implementation**, not the GPU vendor or return address.

Preferred diagnostic signal:

```cpp
GetModuleHandleW(L"igxess_fg.dll") != nullptr
```

This reflects the actual Intel native XeFG implementation observed in the passing control log.

Do not use:

```text
State::isRunningOnNvidia
Util::WhoIsTheCaller(...)
caller == "libxess_fg.dll"
caller == "igxess_fg.dll"
```

as production ownership logic.

If runtime classification becomes necessary, centralize it in `XeFGProxy` or `XeFG_Dx12` and log the classification once per lifecycle.

---

## 10. Required logging

Keep the following PR34 logs:

```text
[DXGI][WrapperLifecycle] action = final_release_begin
[DXGI][WrapperLifecycle] action = reentrant_final_release_consumed
[DXGI][WrapperLifecycle] action = final_release_fg_retire_begin
[DXGI][WrapperLifecycle] action = final_release_fg_retire_complete
[DXGI][WrapperLifecycle] action = final_release_complete
```

Add:

```text
[XeFG][ShutdownOwnership] action = evaluate
[XeFG][ShutdownOwnership] action = ref_pre_retire_skipped
```

The skip log must contain at least:

```text
trigger
reason
shutting_down or implied process_shutdown
public proxy
XeFG context
thread id (evaluate log)
```

Do not log caller DLL names as logic inputs.

---

## 11. Validation matrix

### A. NVIDIA — Monster Hunter Wilds — required blocker

Use the same environment that produced:

```text
reframework_crash.dmp
libxess_fg.dll + 0x22a140
```

Run at least **5 complete launch -> gameplay/menu -> exit cycles**.

Required results for every run:

- process exits completely;
- Steam no longer reports the game as running;
- no shutdown hang;
- no `reframework_crash.dmp`;
- no REF `Exception occurred: c0000005`;
- no crash in `libxess_fg.dll`;
- PR34 `reentrant_final_release_consumed` may still appear and is not itself an error;
- `ShutdownOwnership evaluate` shows `shutting_down = true` on the terminal wrapped release;
- `ref_pre_retire_skipped, reason = process_shutdown` appears;
- `final_release_complete` appears.

### B. Intel — Monster Hunter Wilds — required regression control

Use:

```text
C:\GoogleDrive\ref-xefg\Release-09\mhw\Intel
```

Run at least **5 complete exits**.

Required:

- no dump;
- no hang;
- no regression in XeFG startup/present;
- clean exit remains clean;
- process-shutdown skip may appear, but normal live REF detach/rebind behavior must remain intact before shutdown.

### C. Live swapchain recreation / resize — required

On at least one REF + XeFG title:

- change resolution/window mode or trigger known swapchain recreation;
- verify non-shutdown `PrepareREFForSwapchainRetire(..., "recreate" / "explicit_release")` still executes;
- verify REF rebinds to the new generation;
- verify no stale generation/queue retirement;
- verify XeFG remains functional.

### D. Non-REF XeFG smoke test

Verify that a non-REF XeFG game still:

- creates XeFG,
- presents generated frames,
- resizes/recreates if applicable,
- exits without hang/crash.

---

## 12. Explicitly forbidden changes

Do **not**:

- revert PR34 / `ac1e48fa`;
- remove `_finalReleaseInProgress`;
- move external FG retirement back under wrapper `_localMutex`;
- make `_localMutex` recursive;
- suppress all `dinput8.dll` AddRef/Release calls;
- suppress all final `Release(0)` calls;
- globally revert P7-A REF pre-retire during live runtime;
- hard-code `libxess_fg.dll` or `igxess_fg.dll` caller strings;
- branch on NVIDIA vendor identity as the first fix;
- clear or release tracking aliases as though they owned COM references;
- combine Phase 1 and Phase 2 into one untestable patch.

---

## 13. Likely files to modify

Phase 1 should normally require only:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

No wrapper change should be necessary for the first patch.

Only touch additional files if required for tests or structured diagnostics.

---

## 14. Acceptance criteria

The patch is acceptable only when all of the following are true:

1. NVIDIA MHW exits 5/5 without hang or `reframework_crash.dmp`.
2. Intel MHW remains clean 5/5.
3. PR34 reentrant final-release protection remains present and exercised.
4. Non-shutdown P7-A REF pre-retire remains active.
5. P7-B generation/queue ownership remains intact.
6. P7-C mutex semantics remain intact.
7. No caller-name/GPU-vendor special case is introduced.
8. Live swapchain recreation still detaches/rebinds REF correctly.
9. Non-REF XeFG smoke test passes.
10. Logs clearly prove whether shutdown pre-retire was skipped and why.

---

## 15. Reviewer note

This work order **extends rather than invalidates** the earlier P7-A / PR34 deadlock fix.

The sequence of findings is:

```text
P6:
  normal shutdown

P7-A:
  correct live REF-before-XeFG retirement ordering
  + new process-shutdown REF pre-retire edge

pre-PR34:
  that edge deadlocks via wrapper reentrant Release(0)

PR34:
  deadlock correctly removed
  -> deeper NVIDIA generic-runtime AV becomes observable

Intel control:
  same PR34 structural path is tolerated

Current target:
  preserve P7-A for live lifecycle transitions
  preserve PR34 terminal-owner safety
  restore vendor-owned semantics for the external REF handoff during process shutdown
```

Keep the patch narrow enough that each of those properties remains independently testable.
