# Release 0.9 P7-A Work Order — Close the REF ↔ OptiScaler XeFG Lock-Order Gap

Date: 2026-09-15

## Status

Implementation work order.

P5-A, P5-B, and P6 are complete. P7-A is the next lifecycle-hardening step for the `reframework-0.9` compatibility branch.

This work must remain narrowly focused on the lock-order boundary between the forked REFramework D3D12 hook monitor and OptiScaler's XeFG swapchain mutex during retirement/recreation.

The goal is:

> Every retirement of the currently tracked XeFG public swapchain must let REFramework drain and detach that presentation lifecycle **before** OptiScaler enters the FG swapchain teardown critical section.

Do not redesign REFramework's general D3D12 hook monitor. Do not import the experimental PR #17/#18 synchronization changes. Do not mix command-queue lifetime work from P7-B into this PR.

---

## 1. Verified baselines

### OptiScaler target

Repository: `onehoon/OptiScaler`

Branch: `reframework-0.9`

Baseline reviewed:

```text
26326227939bbd3771d0fe20ee93cf2f75d5532e
P6: fail closed on XeLL lifecycle uncertainty (#30)
```

This baseline already contains:

- P1-P4 XeFG swapchain/lifecycle ownership hardening;
- P5-A exact Destroy reconciliation on the REF side;
- P5-B final-public-proxy pre-retire handoff;
- P6 separate XeLL fail-closed lifecycle/quarantine.

### REFramework compatibility baseline

Repository: `onehoon/REFramework`

Branch: `master`

Baseline reviewed:

```text
4bf45b370e602f7f6a3ca54f308daa4e353aab8a
XeFG P5-B1: add final proxy pre-retire handoff (#54)
```

No REFramework source change is expected for P7-A unless implementation proves the already exported V1 handoff cannot safely cover a normal current-lifecycle retirement. The reviewed implementation does not depend on COM refcount finality, so the existing ABI is expected to be sufficient.

---

## 2. Current lock-order problem is code-proven

The current forked REFramework keeps `m_hook_monitor_mutex` held while invoking the original Present/Resize entry point.

### REF Present path

`D3D12Hook::present_common(...)` begins with:

```cpp
std::scoped_lock _{g_framework->get_hook_monitor_mutex()};
```

and later calls:

```cpp
result = original_call();
```

without dropping that mutex.

For the XeFG swapchain, the original call can enter OptiScaler's FG hook. `FGHooks::FGPresent(...)` may then acquire:

```cpp
fg->Mutex.lock(2);
```

and holds it across the underlying swapchain Present.

Therefore one valid current ordering is:

```text
REF hook_monitor_mutex
    -> Opti FG Mutex (owner tag 2)
```

### REF resize paths

The same ordering exists for resize.

`D3D12Hook::resize_buffers(...)` holds:

```cpp
std::scoped_lock _{g_framework->get_hook_monitor_mutex()};
```

while the original resize path can enter OptiScaler `hkResizeBuffers(...)`, which uses:

```cpp
OwnedLockGuard lg(fg->Mutex, 6677);
```

`D3D12Hook::resize_buffers1(...)` likewise holds the hook-monitor mutex while OptiScaler `hkResizeBuffers1(...)` uses:

```cpp
OwnedLockGuard lg(fg->Mutex, 6678);
```

So Present is not the only relevant caller.

---

## 3. Opposite teardown ordering also exists

Current `XeFG_Dx12::ReleaseSwapchainLocked(...)` optionally acquires:

```cpp
Mutex.lock(1);
```

and, while that FG mutex is still owned, eventually calls:

```cpp
DestroySwapchainContext();
```

which calls:

```cpp
XeFGProxy::Destroy()(context);
```

The forked REFramework hooks `xefgSwapChainDestroy` through `XeFGCompatibility::dispatch_destroy(...)`.

The current REF destroy dispatch performs hook-monitor work both before and after the Intel vendor call:

```text
active_binding_snapshot()
    -> hook_monitor_mutex

prepare_for_xefg_runtime_transition(...)
    -> hook_monitor_mutex
    -> releases it before the vendor Destroy call

original(context)

note_xefg_destroy_result(...)
    -> hook_monitor_mutex again
```

Therefore the opposite ordering is also reachable:

```text
Opti FG Mutex (owner tag 1)
    -> REF hook_monitor_mutex
```

The vendor Destroy itself is **not** executed while REF deliberately holds the hook-monitor mutex. The problem is that the REF Destroy hook must acquire that mutex around the vendor call while OptiScaler is still inside its FG teardown critical section.

Combined with Present/Resize, the current source contains the classic opposing order:

```text
Thread A: REF hook_monitor_mutex -> Opti FG Mutex
Thread B: Opti FG Mutex          -> REF hook_monitor_mutex
```

A deadlock requires the timing to overlap, but the conflicting acquisition order is source-proven and should be removed from the supported current REF + OptiScaler lifecycle.

---

## 4. P5-B already solved the final-proxy variant

Do not discard the work already done by P5-B.

`ReleaseSwapchainFromFinalProxyRelease(...)` currently performs:

```text
_swapchainLifecycleMutex
    -> validate current final proxy / stale generation
    -> REFramework_XeFG_PreRetireSwapchainV1(...)
       [REF hook_monitor_mutex; drain/detach current binding]
    -> ReleaseSwapchainLocked(...)
       -> optional Opti FG Mutex owner 1
       -> final public proxy Release
       -> XeFG Destroy
```

This is the correct relative position for the REF handoff:

> after lifecycle identity has been serialized/validated, but before OptiScaler acquires the FG swapchain mutex or consumes the public proxy.

The exported REF handoff holds a temporary COM keepalive, acquires the hook-monitor mutex, validates the exact runtime context plus compatible HWND, removes the instance presentation hook, clears the active borrowed binding, and returns only after that detach is complete.

As a result, an already-running REF Present/Resize must leave the hook-monitor critical section before P5-B can complete, and the retired presentation hook is removed before OptiScaler proceeds into final teardown.

**P7-A must preserve this final-proxy behavior exactly.**

---

## 5. Remaining gap after P5-B

The normal replacement/recreation paths do not use the P5-B handoff.

In both `XeFG_Dx12::CreateSwapchain(...)` and `CreateSwapchain1(...)`, when an old swapchain exists and `readyToRelease` is true, current code does:

```cpp
if (!ReleaseSwapchainLocked(_hwnd))
{
    ...
    return false;
}
```

This enters the FG teardown critical section directly.

So the current lifecycle differs by trigger:

```text
final COM proxy retirement
    -> REF pre-retire handoff
    -> FG teardown mutex
    -> Destroy

old swapchain retirement during recreation
    -> FG teardown mutex
    -> Destroy
    -> REF Destroy hook asks for hook_monitor_mutex
```

The second path is the P7-A gap.

There is also a public `XeFG_Dx12::ReleaseSwapchain(HWND)` wrapper that currently forwards directly to `ReleaseSwapchainLocked(hwnd)` after acquiring `_swapchainLifecycleMutex`. It must not remain a bypass around the retirement handoff.

---

## 6. Why the existing V1 export can be reused

The existing ABI is frozen and must not be changed:

```cpp
extern "C" __declspec(dllexport)
uint32_t WINAPI REFramework_XeFG_PreRetireSwapchainV1(
    IUnknown* public_proxy,
    void* xefg_context,
    HWND hwnd) noexcept;
```

Statuses remain:

```cpp
SafeNotTracked = 0,
SafeDetached   = 1,
Blocked        = 2,
```

Unknown future values remain fail-closed in the V1 OptiScaler caller.

The reviewed REF implementation does **not** inspect COM refcount or require that `public_proxy` is already at its final reference. It uses the proxy only as a temporary keepalive while detaching the exact current runtime binding.

Its actual safety decision is based on:

```text
active binding exists?
exact runtime context matches?
runtime slot valid?
HWND compatible when both sides provide one?
detach succeeds?
```

Therefore the V1 handoff is suitable for the broader semantic meaning already implied by its name: **the current public proxy/lifecycle is about to be retired**, whether the trigger is final COM release or explicit replacement before recreation.

Do not add a V2 export merely to distinguish the trigger.

If implementation uncovers a concrete REF-side assumption that contradicts this reviewed behavior, stop and split P7-A into coordinated REF + OptiScaler PRs instead of guessing around the ABI.

---

# P7-A implementation

## 7. Recommended PR shape

Repository: `onehoon/OptiScaler`

Base: `reframework-0.9`

Recommended title:

```text
P7-A: detach REF before all XeFG lifecycle retirement
```

Expected primary file:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

`XeFG_Dx12.h` may be touched only for a small private helper declaration if needed.

No REFramework file is expected to change.

Do not touch:

```text
OwnedMutex.h
FG_Hooks.cpp synchronization semantics
thread_local Present/Resize guards
D3D12 command queue ownership
XeLL lifecycle policy
XeFG result-code policy
```

---

## 8. Add one narrow retirement-handoff helper

Avoid copying subtly different P5-B status handling into three paths.

A recommended shape is:

```cpp
bool XeFG_Dx12::PrepareREFForSwapchainRetire(
    IUnknown* publicProxy,
    HWND hwnd,
    const char* trigger)
{
    if (publicProxy == nullptr || _swapChainContext == nullptr)
        return true;

    const auto decision =
        PrepareREFForXeFGProxyRetire(publicProxy, _swapChainContext, hwnd);

    if (decision == RefHandoffDecision::Blocked)
    {
        _swapchainRecreationBlocked = true;
        LOG_ERROR(
            "[XeFG][Lifecycle] action = ref_pre_retire_blocked, "
            "trigger = {}, proxy = {:X}, context = {:X}, hwnd = {:X}",
            trigger,
            (size_t) publicProxy,
            (size_t) _swapChainContext,
            (size_t) hwnd);
        return false;
    }

    if (decision == RefHandoffDecision::Safe)
    {
        LOG_INFO(
            "[XeFG][Lifecycle] action = ref_pre_retire_complete, "
            "trigger = {}, proxy = {:X}, context = {:X}",
            trigger,
            (size_t) publicProxy,
            (size_t) _swapChainContext);
    }
    else
    {
        LOG_DEBUG(
            "[XeFG][Lifecycle] action = ref_pre_retire_skipped, "
            "trigger = {}, reason = export_unavailable",
            trigger);
    }

    return true;
}
```

The exact helper name/log wording may differ.

Required semantics:

```text
Blocked
    -> set XeFG recreation quarantine
    -> stop before ReleaseSwapchainLocked
    -> do not call vendor Destroy
    -> do not consume/release the tracked public proxy as part of this retirement

SafeDetached / SafeNotTracked
    -> safe to continue

export unavailable after successful module scan
    -> preserve old/no-REF compatibility path

module discovery failure
    -> existing P5-B fail-closed behavior remains Blocked
```

Do not weaken `FindREFXeFGPreRetire()` discovery semantics from P5-B2.

---

## 9. Reuse the helper in the existing final-proxy path without changing behavior

The current final-proxy path is already correct.

It is acceptable either to:

1. leave its existing handoff block structurally unchanged; or
2. replace only that block with the new helper while preserving identical ordering and failure behavior.

If factoring it through the helper, the sequence must remain:

```text
acquire _swapchainLifecycleMutex
    -> stale-final-proxy check
    -> REF pre-retire handoff
    -> ReleaseSwapchainLocked
```

Never move the handoff before the P4/P5 stale-current proxy identity check.

Never move it inside the owner-1 FG mutex section.

Never release the final COM reference before handoff success.

---

## 10. Add the handoff to `CreateSwapchain(...)` old-lifecycle retirement

Current branch enters `ReleaseSwapchainLocked(_hwnd)` directly when replacing an existing lifecycle.

Before that call, identify the exact public proxy being retired from the already serialized current state:

```cpp
IUnknown* retiringProxy = State::Instance().currentFGSwapchain;
```

Then perform the REF handoff **before** `ReleaseSwapchainLocked()`:

```cpp
if (!PrepareREFForSwapchainRetire(
        retiringProxy,
        _hwnd,
        "recreate"))
{
    LOG_ERROR(
        "[XeFG][Lifecycle] action = recreate_aborted, "
        "api = CreateSwapchain, reason = ref_pre_retire_failed");
    return false;
}

if (!ReleaseSwapchainLocked(_hwnd))
{
    ... existing failure path ...
}
```

This code already executes while `_swapchainLifecycleMutex` is held by `CreateSwapchain(...)`.

That is intentional. Required order:

```text
_swapchainLifecycleMutex
    -> REF handoff / hook_monitor_mutex
    -> REF handoff returns and releases hook_monitor_mutex
    -> ReleaseSwapchainLocked
    -> optional FG Mutex owner 1
```

Do not acquire the FG mutex before the REF handoff.

---

## 11. Apply the identical rule to `CreateSwapchain1(...)`

`CreateSwapchain1(...)` has the same old-lifecycle release branch.

Use the exact same retirement helper and failure semantics.

Do not allow the two create APIs to drift into different lock ordering.

---

## 12. Close the public `ReleaseSwapchain(HWND)` bypass

Current implementation is effectively:

```cpp
std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
...
return ReleaseSwapchainLocked(hwnd);
```

Before forwarding to the raw teardown helper, perform the same current-lifecycle handoff when a current public proxy/context exists.

Recommended shape:

```cpp
auto* retiringProxy = State::Instance().currentFGSwapchain;
if (!PrepareREFForSwapchainRetire(retiringProxy, hwnd, "explicit_release"))
    return false;

return ReleaseSwapchainLocked(hwnd);
```

The helper should treat `publicProxy == nullptr` or `_swapChainContext == nullptr` as no tracked public lifecycle to detach, not as a reason to invent a new failure mode.

---

## 13. Keep `ReleaseSwapchainLocked(...)` as the post-handoff teardown primitive

Do not put a new REF call inside the owner-1 mutex section.

The role of `ReleaseSwapchainLocked(...)` after P7-A should be conceptually:

```text
caller serialized lifecycle
caller prepared/detached REF if a current public lifecycle exists
    -> ReleaseSwapchainLocked
       -> mark release in progress
       -> optional FG Mutex owner 1
       -> local FG teardown
       -> optional final proxy release
       -> XeFG Destroy / XeLL teardown
       -> release local objects
```

Add a short source comment above `ReleaseSwapchainLocked(...)` or at its callers documenting the invariant:

```cpp
// Any current REF-tracked XeFG presentation lifecycle must be detached before
// entering this function's FG mutex section.
```

Do not add a second call to the REF export after `Mutex.lock(1)`.

---

## 14. Do not solve this by shrinking REF's hook-monitor mutex in P7-A

It is tempting to change `D3D12Hook::present_common`, `resize_buffers`, and `resize_buffers1` to release the hook-monitor mutex around `original_call()`.

Do **not** do that in this PR.

The REF mutex currently protects more than a single pointer read:

- `g_d3d12_hook` lifetime and replacement;
- `m_swapchain_hook` lifetime;
- renderer/reset callbacks;
- XeFG presentation-session transitions;
- monitor-driven rehook/recovery.

Dropping it around the original call without a separate lifetime pin/in-flight-call protocol would create a different class of UAF/stale-hook races.

P7-A should use the lifecycle detach boundary already created by P5-B instead of redesigning generic REF hook synchronization.

---

## 15. Do not solve this with PR #17 / PR #18

The open experimental master PRs remain out of scope:

```text
#17 thread-local FG reentrancy guards
#18 thread-aware OwnedMutex ownership
#19 diagnostic trace
```

They are not merged into the supported branch and do not remove the cross-component lock-order issue described here.

P7-A must work against the current `reframework-0.9` mutex implementation.

---

## 16. P5-B and P6 invariants that must remain unchanged

P7-A must not regress:

### P5-B

- final proxy stale-generation/current-proxy check;
- optional REF discovery without a hard import dependency;
- scan failure fail-closed;
- unknown ABI status fail-closed;
- final proxy reference preserved when REF handoff blocks;
- no REF call while owner-1 FG mutex is held.

### P6

- XeFG destroyed before XeLL;
- separate XeLL recreation quarantine;
- fakenvapi expected-old unpublish before XeLL destroy;
- failed XeLL retirement remains fail-closed.

P7-A only changes **when REF's current presentation lifecycle is detached before entering existing teardown**.

---

## 17. Expected lock behavior after P7-A

### Concurrent REF Present already in flight

```text
Thread A
REF present_common
    holds hook_monitor_mutex
    -> Opti FGPresent may hold/use FG Mutex

Thread B
Opti CreateSwapchain replacement
    holds _swapchainLifecycleMutex
    -> calls REF pre-retire export
    -> waits only for hook_monitor_mutex
    -> DOES NOT hold FG Mutex yet
```

Thread A can finish, release its FG mutex, return through REF, and release `hook_monitor_mutex`.

Thread B then acquires the REF mutex inside the export, detaches the old presentation lifecycle, returns, and only afterward enters `ReleaseSwapchainLocked()` / owner-1 FG mutex.

No circular wait is formed.

### Concurrent REF ResizeBuffers / ResizeBuffers1 already in flight

The same reasoning applies because P7-A waits at the REF handoff **before** entering owner-1 teardown locking.

### XeFG Destroy after detach

The REF runtime Destroy hook still acquires `hook_monitor_mutex` around the vendor call for lifecycle reconciliation.

That is acceptable after the old presentation lifecycle has been drained/detached, because the retiring swapchain no longer has an REF Present/Resize instance path that can hold `hook_monitor_mutex` and then wait for the same Opti FG mutex.

Do not claim that P7-A removes every `FG Mutex -> hook_monitor_mutex` acquisition from the process. It removes the supported current-lifecycle circular dependency by establishing the detach barrier first.

---

# Validation

## 18. Static validation

Required:

```text
git diff --check
clang-format --dry-run --Werror on every changed C/C++ file
Release x64 OptiScaler.dll build
```

If CI exposes only clang-format, local Release build evidence must be included in the PR body.

No new import dependency on REFramework/dinput8 is allowed.

Verify with `dumpbin /DEPENDENTS` if the build environment already uses that P5-B validation.

---

## 19. Source audit

Before opening the PR, audit every `ReleaseSwapchainLocked(...)` call in `XeFG_Dx12.cpp`.

Expected current callers are:

```text
CreateSwapchain(...)
CreateSwapchain1(...)
ReleaseSwapchain(...)
ReleaseSwapchainFromFinalProxyRelease(...)
```

For every path that can retire the **current** public lifecycle, prove one of:

```text
A. REF pre-retire handoff happened after lifecycle identity validation and before ReleaseSwapchainLocked
or
B. the path is the stale-final-proxy release-only branch and does not retire the current lifecycle
```

There must be no current-lifecycle path that reaches the owner-1 FG mutex first.

---

## 20. Failure-injection / logic tests

### Test A — REF handoff blocked during recreation

Force V1 to return `Blocked` for the current old lifecycle.

Expected:

```text
CreateSwapchain/CreateSwapchain1 returns failure
ReleaseSwapchainLocked is not entered
vendor xefgSwapChainDestroy is not called
old public proxy is not consumed by P7-A
_swapchainRecreationBlocked == true
```

### Test B — REF export absent

Run without REFramework or with a build that does not export V1.

Expected:

```text
successful complete module scan + export absent
    -> existing legacy teardown path
    -> no hard dependency / no load failure
```

### Test C — current REF, active binding

Expected:

```text
pre-retire -> SafeDetached
old REF presentation binding removed
then ReleaseSwapchainLocked
then XeFG Destroy
```

### Test D — current REF, no active binding

Expected:

```text
pre-retire -> SafeNotTracked
teardown continues
```

### Test E — identity mismatch

If REF still tracks a different runtime context/generation, V1 must return `Blocked` and OptiScaler must not destroy the uncertain lifecycle.

Do not weaken exact context matching to make this test pass.

---

## 21. Runtime stress validation

Primary matrix:

```text
Monster Hunter Wilds
fork REFramework master >= 4bf45b37
OptiScaler reframework-0.9 P7-A build
Intel GPU
XeFG enabled
```

Exercise repeatedly:

```text
launch
menu/game transition
window mode change
resize if game permits it
FG enable/disable
swapchain recreation trigger if reproducible
return to title / reload
exit
```

Also smoke-test:

```text
Dragon's Dogma 2
RE9 / PRAGMATA path if available
one non-Capcom XeFG title without REFramework
```

Watch specifically for:

```text
hang during resize/recreation
hang during final proxy release
stuck hook-monitor recovery
repeated ref_pre_retire_blocked
stale generation teardown
new XeFG Destroy quarantine
XeLL quarantine regression
```

Logging-off validation matters because prior MHW failures were timing-sensitive.

---

## 22. Diagnostic logging required for the PR

Keep logging sparse and lifecycle-oriented.

For each new non-final handoff, log at least:

```text
trigger = recreate / explicit_release
proxy
context
handoff outcome = safe_detached / safe_not_tracked / unavailable / blocked
```

Do not add per-frame logging.

Do not add sleeps/yields/retry loops as a synchronization substitute.

---

## 23. Explicit non-goals

Do not include any of the following:

- broad REFramework hook-monitor scope changes;
- new cross-DLL mutex sharing;
- callbacks executed by REF while holding its hook-monitor mutex;
- `OwnedMutex` redesign;
- PR #17/#18 backport;
- new Present/Resize thread-local guards;
- arbitrary timeouts;
- force-release of COM references;
- Intel Destroy retry loops;
- XeFG result-code policy changes;
- XeLL lifecycle changes;
- D3D12 command queue ownership changes;
- shutdown-policy redesign;
- unrelated master backports.

---

## 24. Review checklist

A reviewer should be able to answer **yes** to all of the following:

1. Does final-proxy retirement still perform stale/current identity validation before REF handoff?
2. Does `CreateSwapchain` perform REF handoff before releasing the old current lifecycle?
3. Does `CreateSwapchain1` do the same?
4. Does public `ReleaseSwapchain` avoid bypassing the handoff?
5. Can `Blocked` stop teardown before owner-1 FG mutex acquisition?
6. Is `NotAvailable` still compatible with no REF / old REF?
7. Is module-enumeration failure still fail-closed per P5-B2?
8. Is there any new REF call while owner-1 FG mutex is already held? The answer must be **no**.
9. Is P5-B final proxy release ordering unchanged?
10. Are P6 XeLL ordering/quarantine semantics unchanged?
11. Are PR #17/#18/#19 still untouched/unmerged?
12. Does Release x64 build pass?

---

## 25. Definition of Done

P7-A is complete when:

1. Every retirement of the current XeFG public lifecycle passes through the existing REF V1 pre-retire barrier before OptiScaler enters owner-1 FG teardown locking.
2. `CreateSwapchain` and `CreateSwapchain1` no longer call `ReleaseSwapchainLocked` directly for a current REF-trackable lifecycle without that barrier.
3. Public `ReleaseSwapchain` cannot bypass the same rule.
4. Final-proxy P5-B behavior is preserved.
5. A blocked/mismatched REF lifecycle prevents vendor Destroy and recreation.
6. No hard REFramework dependency is added.
7. No generic REF hook-monitor mutex redesign is included.
8. Release x64 build and formatting checks pass.
9. MHW current fork REF + XeFG stress testing shows no recreation/final-release hang regression.
10. Non-REF XeFG smoke testing still works.

---

## 26. Follow-up after P7-A

Keep the next work separate:

```text
P7-B — D3D12 command-queue lifetime ownership
```

Current queue storage still contains raw-pointer lifetime risks (`currentCommandQueue`, `_gameCommandQueue`). P7-A must not absorb that work.