# Release 0.9 A2 — REFramework Factory Monitor / OptiScaler FG Mutex Lock-Order Work Order

Date: 2026-09-15

## 1. Purpose

This work order addresses **A2 only** from the independent OptiScaler ↔ REFramework compatibility audit, revalidated against the current fork sources after A1 was merged.

A2 is a conditional but source-proven lock-order inversion between:

- REFramework's recursive `hook_monitor_mutex`, and
- OptiScaler's frame-generation `FG Mutex`.

The dangerous cycle is:

```text
Present thread P
----------------
Opti FG Mutex
  -> Intel XeFG / internal presentation callback
     -> REF D3D12 Present callback
        -> waits for REF hook_monitor_mutex

Swapchain recreation thread R
-----------------------------
REF hook_monitor_mutex
  -> REF D3D12Hook::create_swapchain()
     -> downstream/original CreateSwapChainForHwnd
        -> Opti factory hook / XeFG recreate path
           -> waits for Opti FG Mutex
```

Therefore:

```text
P owns F, waits M
R owns M, waits F
```

where:

- `F` = OptiScaler FG mutex
- `M` = REFramework `hook_monitor_mutex`

This can produce a real CPU deadlock/hang when active XeFG presentation overlaps a swapchain recreation on the relevant factory hook chain.

The success criterion is narrow:

> During an active XeFG factory transition, REFramework must not hold `hook_monitor_mutex` while calling the downstream/original `CreateSwapChainForHwnd` chain that may enter OptiScaler and acquire the FG mutex. REFramework must still preserve its existing detach/reset/rehook semantics and must not introduce a new nested-factory, early-rehook, or hook-monitor recovery race.

This is a **Capcom + fork REFramework compatibility deadlock fix**, not a generic factory-hook refactor.

---

## 2. Frozen source bases

### REFramework implementation target

- Repository: `onehoon/REFramework`
- Branch: `master`
- Audited HEAD: `79b29c073eae08e818b5f220f01f25eb6382f086`
- This HEAD includes merged PR #55 / A1 late-callback handling.

### OptiScaler compatibility reference

- Repository: `onehoon/OptiScaler`
- Branch: `reframework-0.9`
- Audited HEAD: `63cf9dc1ffe0010f59c420a28694803c29a1bd21`

The OptiScaler HEAD above adds documentation on top of the hardened 0.9 lifecycle code. The relevant `FG_Hooks.cpp`, `DxgiFactory_Hooks.cpp`, and `XeFG_Dx12.cpp` behavior is unchanged by that documentation commit.

### Source-of-truth rule

Design from these fork sources only.

- Do **not** backport OptiScaler master architecture.
- Do **not** assume OptiScaler master is more correct because it is newer.
- Do **not** import unmerged experimental PR behavior.
- Do **not** broaden this into general REFramework D3D12 synchronization cleanup.

---

## 3. Scope guard

### In scope

Only the A2 lock boundary around REFramework's factory callback is in scope:

- `D3D12Hook::create_swapchain()`
- active XeFG / nested XeFG factory-transition serialization
- release/reacquire of `hook_monitor_mutex` around the downstream factory call
- safe rehook ownership after downstream return
- minimal diagnostics required to prove the new ordering

Expected production modification:

- `onehoon/REFramework/src/D3D12Hook.cpp`

`D3D12Hook.hpp` should not need changes unless a very small state/helper declaration materially improves correctness.

### Explicitly out of scope

Do not include:

- A1 redesign; PR #55 is now the required baseline.
- A3 `ResizeBuffers1` presentation-queue identity work.
- F2 optional Destroy-hook rollback hardening.
- F1 / MHRise module unlink handling. MHRise is not a supported target for this phase.
- XeLL/fakenvapi changes.
- Intel XeFG result-policy changes.
- OptiScaler queue ownership changes.
- OptiScaler FG mutex redesign.
- OptiScaler production-code changes for A2.
- Streamline `linkSwapchainToCmdQueue` refactoring unless a separate source-proven lock cycle is demonstrated.
- generic D3D11/D3D12 hook-monitor architecture changes.

---

## 4. Current source-proven lock graph

## 4.1 REF factory callback currently holds the monitor across downstream code

At REFramework HEAD `79b29c0...`, `D3D12Hook::create_swapchain()` does:

```cpp
std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

bool hook_was_nullptr = g_d3d12_hook == nullptr;

if (g_d3d12_hook != nullptr && g_framework->get_d3d12_hook() != nullptr) {
    g_framework->on_reset();
    g_d3d12_hook->unhook();
}

const auto result = create_swap_chain_fn(
    factory,
    device,
    hwnd,
    desc,
    p_fullscreen_desc,
    p_restrict_to_output,
    swap_chain);

...

if (!hook_was_nullptr) {
    g_framework->hook_d3d12();
}
```

The `std::scoped_lock` remains alive over the call to `create_swap_chain_fn(...)`.

`m_hook_monitor_mutex` is a `std::recursive_mutex`, so same-thread recursive REF calls can progress, but another thread entering REF Present/Resize cannot acquire the monitor while the factory thread is inside downstream code.

That is the outer lock responsible for A2.

## 4.2 OptiScaler hooks the same factory operation

Current `reframework-0.9` hooks factory vtable slot 15 / `CreateSwapChainForHwnd` through Detours.

The Opti factory path can enter:

```cpp
FGHooks::CreateSwapChainForHwnd(...)
```

which can create/recreate the XeFG swapchain lifecycle.

Therefore REF's `create_swap_chain_fn(...)` is not guaranteed to be a leaf DXGI call. Depending on installation/chaining order it can execute OptiScaler production code and vendor XeFG creation/recreation code before returning.

A2 must treat this downstream call as **external code** that can acquire the FG mutex or trigger code that does.

## 4.3 Opti Present can own the FG mutex while Intel reaches REF

Current `FGHooks::hkFGPresent()` conditionally acquires the FG mutex with owner tag 2 for active frame generation and keeps it across the actual presentation path.

The Intel XeFG presentation path can reach the REF-hooked internal/presentation swapchain callback while that FG mutex is still owned.

Therefore this direction is valid:

```text
Opti FG Mutex
  -> REF hook_monitor_mutex
```

## 4.4 Opti P7-A ordering is already correct

Current `XeFG_Dx12.cpp` performs REF pre-retire before entering `ReleaseSwapchainLocked()`.

Examples include explicit release, final-proxy release, and recreate paths:

```text
PrepareREFForSwapchainRetire(...)
  -> REFramework_XeFG_PreRetireSwapchainV1(...)
  -> REF monitor is released on return

then

ReleaseSwapchainLocked(...)
  -> optional FG mutex
  -> final proxy release / XeFG Destroy
```

Therefore **do not modify OptiScaler production ordering for A2**.

The remaining inversion is the outer REF factory monitor held around downstream `CreateSwapChainForHwnd`.

---

## 5. Why A1 / PR #55 is a required prerequisite

A2 cannot safely be fixed by simply releasing `hook_monitor_mutex` unless callbacks that were already dispatched to the old REF thunk can survive physical unhook.

PR #55 now provides that guarantee:

- old `Present` / `Present1` / `ResizeBuffers` / `ResizeTarget` / `ResizeBuffers1` callbacks detect missing or different XeFG instance state;
- they forward through the callback object's restored current vtable;
- they do not dereference the removed `m_swapchain_hook`;
- `Present1` performs the retired-instance raw identity gate before `QueryInterface`, preserving P5-B ownership.

That means the A2 factory thread can:

```text
lock REF monitor
  -> reset/unhook old REF D3D12 instance
unlock REF monitor
  -> allow already-entered callback to finish through A1 fallback
  -> call downstream factory code without owning REF monitor
```

Do **not** revert or bypass A1 to implement A2.

---

## 6. Required design: XeFG factory split phase in REF only

### 6.1 Preserve the legacy path outside XeFG transitions

Do not globally rewrite factory synchronization unless necessary.

When there is no active XeFG binding and no XeFG factory transition already in flight, preserve the existing behavior as closely as possible.

The new split-phase path should activate when either:

1. the current REF D3D12 hook has an active XeFG binding; or
2. a split XeFG factory transition is already in flight.

Condition (2) is mandatory because nested/reentrant factory calls may occur after the first call has already unhooked `g_d3d12_hook`. A nested call must **not** fall back to the old monitor-held downstream behavior, otherwise it can recreate the same A2 cycle.

Conceptually:

```cpp
const bool active_xefg =
    g_d3d12_hook != nullptr &&
    g_d3d12_hook->get_xefg_lifecycle_snapshot().active;

const bool split_factory_transition =
    active_xefg || xefg_factory_state.in_flight != 0;
```

Exact naming may differ.

### 6.2 Use `unique_lock`, not a second lock held over external code

For the split path, replace the all-scope `scoped_lock` pattern with a lock that can be deliberately released and reacquired:

```cpp
auto& monitor = g_framework->get_hook_monitor_mutex();
std::unique_lock lifecycle_lock{monitor};
```

Required ordering:

```text
REF monitor locked
  -> snapshot transition state
  -> renderer reset / old REF hook unhook
  -> publish in-flight factory-transition state
REF monitor unlocked
  -> downstream/original CreateSwapChainForHwnd
REF monitor locked again
  -> post-call diagnostics
  -> transition completion/revalidation
  -> rehook only if this transition still owns that responsibility
REF monitor unlocked
```

### 6.3 Do not use another mutex across `create_swap_chain_fn(...)`

Do **not** replace `hook_monitor_mutex` with another non-recursive mutex held across the downstream call.

The factory chain can be nested/reentrant. A second mutex held over external code can turn A2 into another deterministic recursion deadlock.

The solution must be based on:

- a small transition state guarded by `hook_monitor_mutex`, and
- no REF lifecycle mutex held while downstream factory code executes.

### 6.4 Suppress background hook-monitor recovery during the split window

Current code implicitly suppresses hook-monitor recovery because it owns `hook_monitor_mutex` across the entire factory call.

Once A2 releases that mutex, the background monitor must not interpret the intentionally unhooked state as a broken hook and rehook in the middle of the factory transition.

Use the existing REFramework mechanism:

```cpp
auto do_not_hook = g_framework->acquire_do_not_hook_d3d();
```

Keep this guard alive across the complete split factory transaction, including the downstream call and post-call revalidation.

It is acceptable to acquire this guard for the whole `create_swapchain()` invocation even on the legacy path if that keeps the diff simpler; the legacy path already owns `hook_monitor_mutex`, so the extra recovery suppression should not change its intended behavior.

Do not invent a second hook-monitor suppression subsystem.

---

## 7. Required nested/concurrent factory-transition state

A plain local `hook_was_nullptr` is not sufficient after the monitor is released.

Consider two calls:

```text
Factory call A
  -> sees active XeFG hook
  -> unhooks REF
  -> releases monitor
  -> enters downstream code

Factory call B (nested or another thread)
  -> enters while A is still downstream
  -> g_d3d12_hook is now null
```

If B uses the legacy path, it may hold the monitor over downstream code and recreate A2. If A rehooks while B is still downstream, REF can also consume/rebind state too early.

Maintain a small state object, accessed only while holding `hook_monitor_mutex`.

Conceptual shape:

```cpp
struct XeFGFactoryTransitionState {
    uint32_t in_flight{};
    bool rehook_required{};
    uint64_t sequence{}; // optional, useful for diagnostics
};

XeFGFactoryTransitionState g_xefg_factory_transition{};
```

This can be namespace-private in `D3D12Hook.cpp`; do not expose it publicly unless required.

### Entry rules

Under `hook_monitor_mutex`:

1. Decide whether the call joins the split XeFG factory transition.
2. If split, increment `in_flight` **before** releasing the monitor.
3. If an active REF D3D12 hook is actually reset/unhooked, set `rehook_required = true`.
4. Perform the existing `on_reset()` + `unhook()` while the monitor is still held.
5. Then release the monitor before the downstream call.

### Exit rules

Immediately after the downstream call returns:

1. Reacquire `hook_monitor_mutex`.
2. Run existing candidate diagnostics while serialized if desired.
3. Decrement `in_flight`.
4. Do not rehook while `in_flight != 0`.
5. When `in_flight == 0`, consume `rehook_required` exactly once.
6. Re-read current REF hook state before rehooking; never use the pre-call pointer as authority.

Required final revalidation:

```cpp
if (state.in_flight == 0 && state.rehook_required) {
    if (g_d3d12_hook == nullptr &&
        g_framework->get_d3d12_hook() != nullptr)
    {
        g_framework->hook_d3d12();
    }

    // If another valid path has already installed a current hook,
    // do not tear it down just to recreate the old pre-call state.
    state.rehook_required = false;
}
```

The exact code can differ, but these semantics are mandatory.

### Important

The last **exiting** split factory call owns the rehook decision, not necessarily the first or outermost C++ stack frame.

This is what prevents:

- early rehook while another nested/concurrent factory call is still executing;
- stale `hook_was_nullptr` decisions;
- rehooking over a newer valid hook;
- nested calls falling back to monitor-held external execution.

---

## 8. Conceptual implementation sketch

This is a design example, not literal code to paste unchanged.

```cpp
HRESULT WINAPI D3D12Hook::create_swapchain(...) {
    auto create_swap_chain_fn =
        s_create_swapchain_hook->get_original<decltype(D3D12Hook::create_swapchain)*>();

    if (g_inside_d3d12_hook) {
        return create_swap_chain_fn(...);
    }

    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    // Prevent background hook-monitor recovery while the intentional
    // split-phase transition temporarily leaves REF unhooked.
    auto do_not_hook = g_framework->acquire_do_not_hook_d3d();

    auto& monitor = g_framework->get_hook_monitor_mutex();
    std::unique_lock lifecycle_lock{monitor};

    auto* current_hook = g_d3d12_hook;
    const bool active_xefg =
        current_hook != nullptr &&
        current_hook->get_xefg_lifecycle_snapshot().active;

    const bool split =
        active_xefg || g_xefg_factory_transition.in_flight != 0;

    const bool legacy_hook_was_null = current_hook == nullptr;

    if (split) {
        ++g_xefg_factory_transition.in_flight;
        ++g_xefg_factory_transition.sequence;
    }

    if (current_hook != nullptr && g_framework->get_d3d12_hook() != nullptr) {
        if (split) {
            g_xefg_factory_transition.rehook_required = true;
        }

        g_framework->on_reset();
        current_hook->unhook();
    }

    if (split) {
        lifecycle_lock.unlock();
    }

    const auto result = create_swap_chain_fn(...);

    if (split) {
        lifecycle_lock.lock();
    }

    // Keep the existing post-call candidate diagnostics here.

    if (split) {
        if (g_xefg_factory_transition.in_flight == 0) {
            // Fail closed: state corruption should be logged and must not
            // blindly rehook from a stale transition.
            ...
        } else {
            --g_xefg_factory_transition.in_flight;
        }

        if (g_xefg_factory_transition.in_flight == 0 &&
            g_xefg_factory_transition.rehook_required)
        {
            if (g_d3d12_hook == nullptr &&
                g_framework->get_d3d12_hook() != nullptr)
            {
                g_framework->hook_d3d12();
            }

            g_xefg_factory_transition.rehook_required = false;
        }
    } else if (!legacy_hook_was_null) {
        // Preserve the current non-XeFG path.
        g_framework->hook_d3d12();
    }

    return result;
}
```

Implementation may use a small helper or scope guard, but avoid turning this into a new subsystem.

---

## 9. Lock-order contract after the fix

The A2 production contract must become:

```text
REF XeFG factory recreation
---------------------------
M acquire
  -> reset/unhook old REF binding
  -> mark split factory transition in-flight
M release
  -> downstream factory / Opti XeFG recreation
     -> Opti may acquire F
     -> Opti pre-retire may briefly acquire M if still applicable
     -> no outer REF M is held here
M acquire
  -> post-call state revalidation
  -> rehook only at final in-flight completion
M release
```

The old deadlock cycle must no longer exist:

```text
FORBIDDEN:
M -> downstream Opti factory -> wait F
```

Opti remains allowed to do:

```text
F -> Intel callback -> briefly acquire M
```

because there is no longer an opposing factory thread that owns M while waiting for F.

---

## 10. Interaction with Opti P5-B / P7-A

Do not change the Opti handoff ABI.

During REF factory pre-phase, `unhook()` may already clear the currently tracked REF XeFG binding before Opti later executes its own pre-retire handoff inside downstream recreation.

In that case Opti may receive `SafeNotTracked`. That is acceptable and is already compatible with the current contract because REF has actually removed its borrowed presentation relationship before Opti enters FG retirement.

Required invariant:

```text
REF relationship detached
  BEFORE
Opti FG retirement / final proxy release / XeFG Destroy
```

A2 changes **who initiates the earlier detach in this factory-recreation schedule**, not the required retirement order.

Do not add a new Opti callback, ABI version, or reverse call for A2.

---

## 11. Fail-closed / defensive requirements

1. `in_flight` must never underflow.
2. Transition state must only be read or written while `hook_monitor_mutex` is held.
3. Do not store raw `D3D12Hook*` across the unlocked downstream interval and then dereference it after relock.
4. Re-read `g_d3d12_hook` and `g_framework->get_d3d12_hook()` after downstream return.
5. Do not force-unhook a newer valid hook merely because the pre-call state had an active hook.
6. Do not rehook until all split factory calls have exited.
7. Do not wait for Present callback counts while holding `hook_monitor_mutex`.
8. Do not add sleeps, polling loops, or timeout-based deadlock avoidance.
9. Do not hold another mutex across external factory code as a substitute for the monitor.
10. Keep A1 restored-vtable late forwarding intact.

If transition bookkeeping becomes inconsistent, log the state and fail closed rather than guessing ownership.

---

## 12. Diagnostics

Add sparse debug-only diagnostics sufficient to prove ordering and nesting.

Suggested shape:

```text
[D3D12][FactoryTransition]
  sequence = N,
  stage = enter,
  split = true,
  in_flight = 1,
  active_xefg = true,
  thread_id = ...

[D3D12][FactoryTransition]
  sequence = N,
  stage = downstream_enter,
  monitor_released = true,
  in_flight = 1

[D3D12][FactoryTransition]
  sequence = N,
  stage = downstream_return,
  result = 0x...,
  in_flight = 1

[D3D12][FactoryTransition]
  sequence = N,
  stage = rehook,
  reason = final_in_flight_exit
```

For nested calls, logs must show `in_flight > 1` and no rehook until it returns to zero.

Do not promote these to noisy per-frame logging.

---

## 13. Validation requirements

### 13.1 Build/static validation

At minimum:

```text
cmake --preset vs2026
cmake --build build --config Release -- /m:1 /v:minimal
python dev/audit_direct_access_clang.py
git diff --check
```

Existing XeFG pre-retire export must remain present.

### 13.2 Deterministic A2 concurrency test / debug harness

Force or instrument the following schedule:

```text
P: acquire Opti FG mutex
P: enter Intel/vendor presentation path
P: arrive at REF D3D12 callback and block before/acquiring M

R: enter REF factory create callback
R: acquire M
R: reset/unhook old REF instance
R: mark factory transition in-flight
R: release M

P: acquire M
P: exercise A1 late-forward path if applicable
P: return from REF callback
P: release FG mutex

R: call downstream Opti factory path
R: acquire FG mutex successfully
R: downstream factory returns
R: reacquire M
R: final revalidation / rehook
```

Pass conditions:

- no deadlock;
- no timeout-based escape required;
- downstream factory call begins with no outer REF monitor ownership;
- A1 late callback remains safe;
- rehook happens once and only after split `in_flight` reaches zero.

### 13.3 Nested factory test

Force a nested factory call while the outer split transition is downstream.

Expected:

```text
outer enter -> in_flight 1
nested enter -> in_flight 2
nested return -> in_flight 1 -> NO rehook
outer return -> in_flight 0 -> single rehook
```

Reverse completion order should also be tested if a synthetic multi-thread harness is available.

### 13.4 Normal non-XeFG regression

Verify the legacy non-XeFG factory path remains behaviorally unchanged:

- normal REF D3D12 startup;
- normal factory create;
- Present hook recovery;
- no unexpected hook-monitor suppression after the call returns.

### 13.5 Capcom runtime validation

At minimum test supported Capcom targets already used by this fork, especially:

- Monster Hunter Wilds
- Dragon's Dogma 2
- a newer RE Engine / RE9-class target when available

Exercise:

- initial XeFG creation;
- repeated window/borderless/fullscreen transitions that recreate the swapchain;
- resolution / graphics changes that force factory recreation;
- Alt+Tab / focus transitions where applicable;
- repeated recreate cycles while FG is actively presenting.

Collect:

- `[D3D12][FactoryTransition]`
- `[XeFG][LateCallback]`
- `[XeFG][ProxyRetire]`
- Opti `[XeFG][Lifecycle]`
- any device-removed / E_ABORT / hang evidence

A runtime hang is a failure even if eventual hook-monitor recovery would have occurred.

---

## 14. Merge acceptance criteria

A2 is complete only when all of the following are true:

1. REF no longer owns `hook_monitor_mutex` across downstream `CreateSwapChainForHwnd` during an active/in-flight XeFG factory transition.
2. Nested/reentrant factory calls join the split transition even after the first call has cleared `g_d3d12_hook`.
3. Hook-monitor background recovery is suppressed during the intentional unlocked/unhooked interval.
4. Rehook does not occur until all split factory calls have returned.
5. Final rehook decision is based on current state after relock, not stale pre-call pointers.
6. A1 late-callback handling remains intact.
7. P5-B/P7-A ordering remains intact.
8. Non-XeFG factory behavior is not unnecessarily redesigned.
9. No OptiScaler production changes are required.
10. Build/static validation passes and runtime logs demonstrate the intended ordering.

---

## 15. Expected PR shape

Preferred implementation should remain small and reviewable:

- Primary repository: `onehoon/REFramework`
- Primary file: `src/D3D12Hook.cpp`
- Expected scope: one focused PR
- No OptiScaler production-code changes
- No A3/F2/F1 work mixed into the PR

Suggested PR title:

```text
Fix XeFG factory lock inversion with split REF monitor scope
```

Suggested commit theme:

```text
fix: release REF monitor across XeFG factory downstream call
```

The implementation should be judged by the lock-order and lifecycle invariants in this document, not by code similarity with OptiScaler master or any unrelated upstream branch.
