# Release 0.9 A1 — REFramework Late Callback / Removed Hook Lifetime Work Order

Date: 2026-09-15

## 1. Purpose

This work order addresses **A1 only** from the independent full-scope compatibility audit:

> A D3D12 swapchain callback can enter an REFramework static hook thunk, block on `hook_monitor_mutex`, and then resume after an OptiScaler-triggered XeFG pre-retire detach has already removed the physical instance hook. The callback then reads state from a hook object that has been cleared or invalidated.

This is a **Capcom + fork REFramework compatibility crash fix**, not a general D3D12 refactor.

The success criterion is narrow:

> A callback that entered REFramework before XeFG pre-retire/unhook, but acquires `hook_monitor_mutex` only after the physical instance hook was removed, must safely forward to the restored downstream/original swapchain method without dereferencing a null/removed REFramework hook object.

## 2. Frozen source bases

Implementation target:

- Repository: `onehoon/REFramework`
- Branch: `master`
- Audited HEAD: `4bf45b370e602f7f6a3ca54f308daa4e353aab8a`

Compatibility reference:

- Repository: `onehoon/OptiScaler`
- Branch: `reframework-0.9`
- Audited HEAD: `2e174f02832d4a25acd2679cb6c1ecc7a034a0d7`

`2e174f...` only adds audit/documentation on top of the already hardened 0.9 implementation; the relevant OptiScaler XeFG lifecycle source remains unchanged from the P5-P7 implementation.

### Source-of-truth rule

Design from these fork sources only.

- Do **not** backport OptiScaler master architecture.
- Do **not** use upstream/master behavior as proof that a design is correct.
- Do **not** broaden this into general REFramework D3D12 hook redesign.

## 3. Audit principle / scope guard

The project-wide audit rule remains in force:

> Only fix an issue when the OptiScaler 0.9 ↔ fork REFramework interaction can produce a concrete crash, invalid object access, deadlock, device failure, or lifecycle mismatch in the Capcom + REFramework environment.

Do not expand this work because code looks unusual, differs from Intel documentation, or could be refactored.

### Explicitly out of scope

The following audit items are **not part of this work order**:

- F1 / MHRise module unlink and REF export discovery.
- A2 factory-lock / FG-mutex inversion.
- A3 `ResizeBuffers1` presentation-queue identity change.
- F2 optional Destroy hook + initialization rollback.
- XeLL/fakenvapi policy changes.
- Intel XeFG result-policy changes.
- OptiScaler queue ownership changes.
- P5/P6/P7 redesign.
- Generic hook-monitor redesign.

MHRise is not a supported target for this phase and must not drive implementation scope.

## 4. Confirmed A1 failure mechanism

### 4.1 OptiScaler side is already ordered correctly

Current `reframework-0.9` performs the REF pre-retire handoff before entering final XeFG retirement.

The relevant flow is:

```text
Opti _swapchainLifecycleMutex
  -> PrepareREFForSwapchainRetire(...)
       -> REFramework_XeFG_PreRetireSwapchainV1(...)
  -> only after SafeDetached / SafeNotTracked
       -> ReleaseSwapchainLocked(...)
       -> optional FG Mutex owner 1
       -> final public proxy Release
       -> XeFG Destroy
```

A `Blocked` handoff quarantines recreation and prevents raw retirement.

Therefore **do not change OptiScaler ordering for A1**.

### 4.2 REF pre-retire physically removes the instance hook

`XeFGCompatibility::prepare_for_public_proxy_retire()` acquires `hook_monitor_mutex`, validates the exact runtime/context identity, and calls:

```cpp
hook->detach_xefg_binding_for_runtime_transition(...);
```

The detach path performs renderer reset and then:

```cpp
m_present_hook.reset();
m_swapchain_hook.reset();
```

before clearing the physical/semantic binding aliases.

This is correct for retiring the tracked borrowed presentation relationship.

### 4.3 The late callback window

The static D3D12 callback thunks acquire `hook_monitor_mutex` **after the callback has already entered the thunk**.

A valid schedule is therefore:

```text
Thread P — already dispatched to REF Present/Resize thunk
--------------------------------------------------------
enter D3D12Hook::present / resize_buffers / resize_target
    |
    +-- waits for hook_monitor_mutex

Thread R — Opti pre-retire handoff
----------------------------------
REFramework_XeFG_PreRetireSwapchainV1
    -> lock hook_monitor_mutex
    -> validate active XeFG binding
    -> renderer reset
    -> m_present_hook.reset()
    -> m_swapchain_hook.reset()
    -> clear binding aliases
    -> unlock hook_monitor_mutex

Thread P resumes
----------------
lock hook_monitor_mutex
read g_d3d12_hook / m_swapchain_hook
```

At the audited REF HEAD:

- `D3D12Hook::present()` can dereference `d3d12` or `m_swapchain_hook` after this transition without a complete late-entry fallback.
- `D3D12Hook::resize_buffers()` dereferences `m_swapchain_hook` without a late-entry fallback.
- `D3D12Hook::resize_target()` dereferences `m_swapchain_hook` without a late-entry fallback.
- `present1()` and `resize_buffers1()` check for missing hook state, but return `E_FAIL`; they avoid the same direct null dereference but do not correctly forward a callback that was legitimately dispatched before physical hook removal.

This is the A1 compatibility defect.

## 5. Required design

### 5.1 Fix ownership boundary in REF only

The production fix should be implemented in fork REFramework.

Expected primary files:

- `src/D3D12Hook.cpp`
- `src/D3D12Hook.hpp` only if a small helper declaration is required

OptiScaler production source changes are **not expected** for A1.

If implementation requires changing the P5-B/P7-A ABI or Opti pre-retire ordering, stop and re-evaluate the design before coding. A1 should not require an ABI revision.

### 5.2 Late callbacks must forward through the restored current vtable

`VtableHook` removal restores the target object's previous vtable before its hook object is destroyed. Therefore, after `m_swapchain_hook.reset()`, a callback that was already dispatched to the old REF thunk can recover by reading the **current vtable slot from the callback's `swap_chain` argument** and forwarding to that restored downstream method.

This is preferred over:

- dereferencing the removed `m_swapchain_hook`,
- retaining the retired REF hook object,
- retaining an extra COM reference after pre-retire returns,
- returning `E_FAIL` for an otherwise valid late callback,
- adding sleeps/timeouts/retries.

The fallback must not create a new ownership relationship with the retiring XeFG proxy.

### 5.3 Add one narrow late-forward helper

Use the existing safe-vtable utilities in `D3D12Hook.cpp` (`is_readable`, `read_vtable_slot`, and/or equivalent local helpers) rather than duplicating arbitrary pointer probing in every callback.

Conceptual example:

```cpp
template <typename Fn>
Fn resolve_late_swapchain_target(
    IUnknown* swapchain,
    size_t slot,
    Fn self) noexcept
{
    if (swapchain == nullptr || !is_readable(swapchain, sizeof(void*)))
        return nullptr;

    auto** vtable = *reinterpret_cast<void***>(swapchain);
    auto* target = read_vtable_slot(vtable, slot);

    if (target == nullptr ||
        target == reinterpret_cast<void*>(self))
    {
        return nullptr;
    }

    return reinterpret_cast<Fn>(target);
}
```

Exact implementation style may differ, but these invariants are mandatory:

1. Never dereference `m_swapchain_hook` after determining it is absent.
2. Never call the REF thunk again as the fallback target.
3. Never fabricate a COM owner from a pointer whose lifecycle has already detached.
4. The fallback target must be resolved while `hook_monitor_mutex` serializes physical hook removal/replacement.
5. Log the late-forward path under XeFG debug logging so runtime validation can prove that A1 was exercised.

Suggested diagnostic shape:

```text
[XeFG][LateCallback] kind = Present,
  action = forward_restored_vtable,
  swapchain = 0x...,
  slot = 8,
  target = 0x...,
  owner = ...
```

Do not spam normal logs; this should be debug-level / compatibility diagnostic output.

## 6. Per-callback requirements

### 6.1 `Present` — slot 8

Current code must distinguish three cases after taking `hook_monitor_mutex`:

1. valid phase-1 `m_present_hook` path;
2. valid instance `m_swapchain_hook` path;
3. late callback after the relevant physical hook was removed.

The third case must forward through the restored current `Present[8]` target.

Conceptual structure:

```cpp
auto* d3d12 = g_d3d12_hook;

if (d3d12 == nullptr) {
    return forward_late_present(swap_chain, sync_interval, flags, r9);
}

if (d3d12->m_is_phase_1) {
    if (d3d12->m_present_hook == nullptr)
        return forward_late_present(...);
} else {
    if (d3d12->m_swapchain_hook == nullptr)
        return forward_late_present(...);
}
```

Do not change normal XeFG `present_common()` behavior.

### 6.2 `Present1` — slot 22

Replace the current missing-hook `E_FAIL` behavior with safe late forwarding when the current vtable has already been restored.

A truly invalid/unresolvable target may still fail closed, but a valid callback dispatched before detach should not be turned into `E_FAIL` solely because REF completed physical unhook first.

### 6.3 `ResizeBuffers` — slot 13

Before using:

```cpp
d3d12->m_swapchain_hook->get_method<...>(13)
```

handle absent/currently detached hook state and forward through restored `ResizeBuffers[13]`.

Do not run REF renderer-reset or XeFG resize lifecycle bookkeeping on the late-forward path: the lifecycle has already detached.

### 6.4 `ResizeTarget` — slot 14

Apply the same rule as `ResizeBuffers`.

A late callback must not access retired REF binding/session state.

### 6.5 `ResizeBuffers1` — slot 39

Replace the current missing-hook `E_FAIL` path with restored-vtable forwarding when possible.

Do not include A3 queue-rebinding changes in this work.

## 7. Important ordering constraints

### 7.1 Keep the current pre-retire ordering

Do not move REF detach after Opti's final proxy release.

Required ordering remains:

```text
REF detach / physical hook removal
  -> return SafeDetached
  -> Opti FG retirement
  -> final public proxy Release
  -> XeFG Destroy
```

A1 fixes what happens to a callback that was already dispatched to an old REF thunk. It does **not** reverse P5-B/P7-A ordering.

### 7.2 Do not wait for callbacks while holding `hook_monitor_mutex`

Do not implement this as:

```text
lock hook_monitor_mutex
remove hook
wait until callback count == 0
```

A callback already waiting on `hook_monitor_mutex` could never drain, producing a deterministic deadlock.

A callback-quiescence counter/barrier is **not required by this work order** if restored-vtable forwarding fully closes A1. If an implementation introduces one, its lock order and progress proof must be reviewed separately before merge.

### 7.3 Do not retain the public XeFG proxy after handoff

The REF pre-retire contract intentionally stops borrowing/holding the presentation relationship before Opti consumes the final public proxy reference.

Do not solve A1 by storing a persistent `ComPtr` to the retiring proxy after `REFramework_XeFG_PreRetireSwapchainV1` returns.

That would alter the final-release contract fixed by P5-B.

## 8. Defensive requirements

Even with restored-vtable forwarding, keep explicit null/state checks in all five callbacks.

The late-forward helper must fail closed if:

- `swap_chain == nullptr`;
- the current vtable is unreadable;
- the target slot is null;
- the target points back to the same REF callback thunk.

Example:

```cpp
if (auto original = resolve_late_swapchain_target<PresentFn>(
        swap_chain, 8, &D3D12Hook::present))
{
    return original(swap_chain, sync_interval, flags, r9);
}

spdlog::error(
    "[XeFG][LateCallback] kind = Present, action = blocked, reason = no_safe_target");
return E_FAIL;
```

`E_FAIL` is acceptable only as the final fail-closed outcome when no safe restored target can be resolved. It must not remain the normal response to a known late callback with a valid restored vtable.

## 9. Validation requirements

### 9.1 Deterministic synthetic race test

Add or instrument a test/debug path that forces this schedule:

```text
Callback thread:
  enter REF callback thunk
  pause immediately before hook_monitor_mutex acquisition

Retire thread:
  execute REFramework_XeFG_PreRetireSwapchainV1
  remove instance hook
  finish SafeDetached

Callback thread:
  resume
```

Expected result:

```text
no null dereference
no access through removed m_swapchain_hook
late callback resolves restored current vtable slot
original/downstream method is called exactly once
no recursive call back into the same REF thunk
```

Run this separately for:

- Present
- Present1
- ResizeBuffers
- ResizeTarget
- ResizeBuffers1

### 9.2 Normal-path regression

Verify that when the hook is still active:

- normal `Present` / `Present1` behavior is unchanged;
- REF render callbacks still execute;
- resize renderer reset/hold behavior is unchanged;
- XeFG binding generation is unchanged;
- P5-B pre-retire status remains `SafeDetached` for a matching active binding;
- P7-A lock ordering is unchanged.

### 9.3 Capcom manual validation

Priority games:

1. Monster Hunter Wilds
2. Dragon's Dogma 2
3. Resident Evil 9 / current supported RE Engine XeFG test target

Exercise:

- launch into gameplay;
- XeFG enable/disable where supported;
- Alt+Tab / borderless transitions;
- repeated resolution/window resize;
- swapchain recreation;
- menu/gameplay transitions;
- repeated exit-to-title / reload where the game recreates presentation state.

Required evidence:

- no access violation at `D3D12Hook::present`, `resize_buffers`, or `resize_target` after pre-retire;
- no `E_FAIL` solely because a valid late `Present1`/`ResizeBuffers1` arrived after detach;
- diagnostic log proves late callbacks, if observed, are forwarded to a non-REF restored target;
- no new P5-B/P7-A regression.

## 10. Expected patch size

Keep the implementation small.

Expected scope:

```text
REFramework
  src/D3D12Hook.cpp
  src/D3D12Hook.hpp     (only if needed for a helper/type)
```

No OptiScaler production source file should change for the intended A1 fix.

If the patch starts modifying XeFG runtime registry, XeFG result semantics, command-queue lifecycle, fakenvapi, or OptiScaler teardown, the implementation has exceeded this work order.

## 11. Review checklist

Before merge, verify all of the following:

- [ ] Base is fork REF `master` at or rebased cleanly from `4bf45b370e602f7f6a3ca54f308daa4e353aab8a`.
- [ ] Opti compatibility reference remains `reframework-0.9`.
- [ ] No Opti master architecture/backport was imported.
- [ ] `Present` cannot dereference null `g_d3d12_hook` / removed `m_swapchain_hook` after monitor acquisition.
- [ ] `ResizeBuffers` cannot dereference a removed instance hook.
- [ ] `ResizeTarget` cannot dereference a removed instance hook.
- [ ] `Present1` late callbacks no longer return `E_FAIL` when a safe restored target exists.
- [ ] `ResizeBuffers1` late callbacks no longer return `E_FAIL` when a safe restored target exists.
- [ ] Late fallback never targets the same REF thunk.
- [ ] Late fallback skips REF renderer/binding bookkeeping that was already detached.
- [ ] No persistent COM keepalive survives the pre-retire handoff.
- [ ] No callback-drain wait is performed while `hook_monitor_mutex` is held.
- [ ] P5-B final-proxy ordering is unchanged.
- [ ] P7-A lock ordering is unchanged.
- [ ] Normal Present/Resize paths remain behaviorally unchanged.
- [ ] Synthetic late-entry race validation passes.
- [ ] Capcom + Opti 0.9 + fork REF manual validation shows no regression.

## 12. Suggested PR title

`Fix late D3D12 callbacks after XeFG pre-retire detach`

## 13. Stop conditions

Stop and request design review instead of broadening the patch if any of these becomes necessary:

- changing `REFramework_XeFG_PreRetireSwapchainV1` ABI;
- modifying OptiScaler final-release ordering;
- retaining the XeFG public proxy beyond handoff return;
- adding a global callback wait while holding `hook_monitor_mutex`;
- changing A2/A3/F1/F2 behavior in the same PR;
- restructuring generic D3D12 hook ownership unrelated to the confirmed late-entry path.

The goal is to close **A1 only**, with the smallest compatibility-safe change.