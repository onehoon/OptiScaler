# Work Order: XeFG Owner-Scoped Swapchain Lifecycle Cleanup

**Repository:** `onehoon/OptiScaler`  
**Target branch:** `master`  
**Baseline:** `9241d397c044e7d9c2b8286d271602095df71dd7`  
**Relevant merged change:** `3809b22150da2e5eb723f58d9e8eadb58705a553` / PR #6  
**Date:** 2026-09-09

---

## 1. Goal

Refine PR #6 without restoring any force-release/refcount-draining behavior.

PR #6 was correct to remove `Release()`-until-zero patterns, but it broadened cleanup authority too far by allowing XeFG teardown paths to clear wrapper/real-swapchain State aliases that belong to different ownership domains.

The corrected rule is:

> **Each object owner releases and clears only the identities it actually owns. XeFG proxy teardown must not act as the cleanup authority for wrapper or real-swapchain objects.**

This is a narrow ownership/lifecycle correction, not a general DXGI refactor.

---

## 2. What remains correct from PR #6

Do **not** revert PR #6 wholesale.

Keep these invariants:

1. Never drain COM references by repeatedly calling `Release()` until the returned count reaches zero or a threshold.
2. `State::currentSwapchain`, `currentFGSwapchain`, `currentWrappedSwapchain`, and `currentRealSwapchain` do not by themselves grant ownership or permission to call `Release()`.
3. `WrappedIDXGISwapChain4` releases the real swapchain reference represented by `_real` exactly once when the wrapper reaches its legitimate final release.
4. The final XeFG public proxy identity must be cleared from State before the final COM reference is consumed.
5. A failed `xefgSwapChainDestroy()` remains fail-closed and must not be bypassed by force releasing unrelated objects.

Intel explicitly exposes `XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE`; a live external XeFG proxy reference is a lifecycle condition to respect, not a refcount to drain.

---

## 3. What needs correction

The questionable part of PR #6 is not the removal of the drains. It is the replacement cleanup policy.

Current code allows XeFG lifecycle paths to snapshot and clear:

```cpp
State::Instance().currentWrappedSwapchain
State::Instance().currentRealSwapchain
```

when an XeFG teardown succeeds.

That is too broad. A successful XeFG proxy/context teardown does not prove that the wrapper or underlying real swapchain has reached its own final lifetime boundary.

Clearing those aliases from an unrelated lifecycle transaction can hide a still-live object from later cleanup/state tracking.

Use owner-scoped cleanup instead:

```text
XeFG public proxy final release
    -> clear only aliases equal to that XeFG public proxy

WrappedIDXGISwapChain4 final release
    -> clear wrapper alias
    -> clear the real-swapchain alias matching its owned _real object
    -> release its owned _real reference exactly once

Other State aliases
    -> remain untouched unless their actual owner/lifetime path proves they are no longer valid
```

---

## 4. Files in scope

Primary:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/State.h
```

Reference-only unless required by a concrete compile/runtime issue:

```text
OptiScaler/hooks/DxgiFactory_Hooks.cpp
OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp
OptiScaler/with_dx12/dx11_with_dx12_sc.cpp
OptiScaler/Util.cpp
```

Do not modify REFramework in this PR.

---

## 5. Task A — Narrow the State ownership comment

Keep the important warning that storing a raw pointer in `State` does not itself create a COM owner.

However, avoid wording that implies one global cleanup transaction owns every swapchain identity.

Preferred wording:

```cpp
// Swapchain tracking aliases.
// Storing a pointer here does not acquire a COM reference and does not grant
// permission to Release() it. Lifetime and cleanup authority remain with the
// concrete object owner that acquired or accepted the corresponding reference.
IDXGISwapChain* currentSwapchain = nullptr;
IDXGISwapChain* currentWrappedSwapchain = nullptr;
IDXGISwapChain* currentRealSwapchain = nullptr;
IDXGISwapChain* currentFGSwapchain = nullptr;
```

Do not convert these fields to `ComPtr` in this work.

---

## 6. Task B — Remove cross-owner alias clearing from XeFG recreate paths

Audit both:

```cpp
XeFG_Dx12::CreateSwapchain(...)
XeFG_Dx12::CreateSwapchain1(...)
```

PR #6 currently snapshots `currentRealSwapchain` and `currentWrappedSwapchain`, calls `ReleaseSwapchainLocked()`, then clears those aliases if their identities did not change.

Remove that replacement cleanup block.

The desired shape is intentionally simpler:

```cpp
else if (readyToRelease)
{
    LOG_INFO("Releasing old swapchain");

    if (!ReleaseSwapchainLocked(_hwnd))
    {
        LOG_ERROR("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain, "
                  "reason = release_not_completed");
        return false;
    }

    // Do not clear currentWrappedSwapchain/currentRealSwapchain here.
    // Their concrete owner paths are responsible for their lifetime/alias cleanup.
}
```

Apply the equivalent change to `CreateSwapchain1()`.

### Required behavior

- Do not restore `currentRealSwapchain->Release()`.
- Do not add a one-shot `Release()` through the State alias.
- Do not add a retry loop.
- Do not clear wrapper/real aliases merely because XeFG context teardown completed.

If `ReleaseSwapchainLocked()` fails, preserve the existing fail-closed behavior.

---

## 7. Task C — Remove cross-owner alias clearing from `hkFGRelease()`

Current PR #6 code snapshots:

```cpp
auto* wrappedAliasBeforeRelease = state.currentWrappedSwapchain;
auto* realAliasBeforeRelease = state.currentRealSwapchain;
```

and clears both after the FG/XeFG release transaction succeeds.

Remove that logic.

For the XeFG path, `hkFGRelease()` should coordinate the final XeFG public-proxy release and context teardown, but it should not become the cleanup authority for `WrappedIDXGISwapChain4` or its underlying real swapchain.

Desired conceptual flow:

```text
hkFGRelease(This)
    -> detect actual final public-proxy release
    -> enter XeFG final-proxy lifecycle transaction
    -> clear aliases that equal This / final XeFG proxy
    -> consume final XeFG proxy COM reference exactly once
    -> destroy XeFG context
    -> return

NO currentWrappedSwapchain cleanup here
NO currentRealSwapchain cleanup here
NO Release through either State alias
```

Be careful because `hkFGRelease()` is shared by multiple FG outputs. Keep the change narrowly scoped and do not introduce unrelated FSRFG/DLSSG behavior changes.

---

## 8. Task D — Keep the final XeFG proxy snapshot fix

Keep the corrected `ReleaseSwapchainFromFinalProxyRelease()` invariant introduced by PR #6.

The final proxy identity must be captured before the callback can invalidate State:

```cpp
auto& state = State::Instance();
auto* const finalProxy = state.currentFGSwapchain;
```

Before the final COM `Release()`:

```cpp
if (state.currentSwapchain == finalProxy)
    state.currentSwapchain = nullptr;

if (state.currentFGSwapchain == finalProxy)
    state.currentFGSwapchain = nullptr;

releaseFinalProxy();
```

Do not clear `currentWrappedSwapchain` or `currentRealSwapchain` from this helper unless a concrete code path proves that either field contains the exact same XeFG public proxy identity. Do not assume that based on naming alone.

The ordering invariant is:

```text
capture final proxy identity
    -> clear State aliases equal to that proxy
    -> final proxy COM Release
    -> xefgSwapChainDestroy
```

Never clear the identity after the final COM release, because the object may already be destroyed.

---

## 9. Task E — Keep wrapper-owned cleanup in `WrappedIDXGISwapChain4::Release()`

PR #6 fixed two useful wrapper ownership details. Keep them.

On the wrapper's legitimate final release:

```cpp
if (state.currentSwapchain == this)
    state.currentSwapchain = nullptr;

if (state.currentWrappedSwapchain == this)
    state.currentWrappedSwapchain = nullptr;

auto* real = std::exchange(_real, nullptr);

if (state.currentRealSwapchain == real)
    state.currentRealSwapchain = nullptr;

const auto refCount = real != nullptr ? real->Release() : 0;
```

This is the correct place to clear wrapper/real aliases because the wrapper is actually ending its own lifetime and releasing the real-swapchain reference it owns/accepted from creation.

Do not use the returned `refCount` as permission to call `Release()` again.

Do not restore the old disabled `while (refCount > 0)` block.

---

## 10. Task F — Preserve fail-closed same-HWND recreation semantics for this PR

Do not invent a new recreation state machine or automatic retry mechanism in this cleanup.

Current behavior after PR #1/F-02 is intentionally conservative:

```text
same-HWND replacement requested
    -> old XeFG lifecycle teardown attempted
    -> if Intel Destroy succeeds: continue
    -> if Intel Destroy reports a real failure such as POINTER_STILL_IN_USE:
         keep the old context identity
         block recreation
         return failure
```

Keep that behavior in this PR.

Most importantly:

```text
POINTER_STILL_IN_USE
    != permission to drain references
    != permission to clear unrelated wrapper/real aliases
```

If runtime testing proves that a supported game routinely calls `CreateSwapChain*` for the same HWND while retaining the previous public proxy, document that exact sequence in the PR. Do not solve it here by force release.

A future dedicated change may evaluate safe proxy reuse/preserve behavior, but that requires its own COM contract review because returning an existing proxy from a factory-style creation path must provide the correct caller-owned reference (normally requiring a balanced `AddRef()`) and must verify descriptor compatibility. That is explicitly out of scope here.

---

## 11. Task G — Audit `oldSwapChain`, but do not redesign it without evidence

Review the current `oldSwapChain` suppression path in `FG_Hooks.cpp` only to ensure this PR does not make it stale or cause a live object's legitimate `Release()` to be swallowed.

Required checks:

- `oldSwapChain` is assigned only after a replacement actually succeeds.
- A failed XeFG recreate must not mark the still-live old proxy as a dead/stale pointer.
- If the new and previous proxy identities are the same, clear `oldSwapChain` rather than suppressing future release of that live proxy.

The current `previousFGSwapchain` / `newFGSwapchain` identity checks were added for this reason. Preserve them unless a concrete defect is found.

Do not broaden this into a rewrite of the old-swapchain mechanism.

---

## 12. Explicitly out of scope

Do not combine any of the following with this PR:

- XeFG positive-warning / `d87d8ca` result-status policy changes.
- Backbuffer F-01 changes.
- New Intel XeFG warning policy.
- REFramework source changes.
- Special K-specific behavior changes.
- New Destroy retries.
- New asynchronous recreation queue.
- New COM smart-pointer migration for global State.
- General DXGI hook refactor.
- FSRFG/DLSSG architectural cleanup unless required to keep existing behavior unchanged.

Keep this PR reviewable and narrow.

---

## 13. Static verification

After implementation, search the modified XeFG/wrapper paths for all of the following:

```text
currentRealSwapchain->Release
currentWrappedSwapchain->Release
while (refCount > 0)
while (release > 0)
0xffffff00
Release-until-zero
```

Expected result for the XeFG ownership paths changed here: no force-drain behavior.

Also verify that wrapper/real alias clearing appears only in an actual wrapper/real owner cleanup path, not in the XeFG proxy/context cleanup path.

Run:

```text
git diff --check
clang-format check for modified C/C++ files
Release x64 build
```

Use the repository's existing build workflow/tooling. Do not change build configuration as part of this work.

---

## 14. Runtime validation

Runtime validation is required before considering this complete.

### Primary: Monster Hunter Wilds + REFramework + Intel XeFG

Exercise at least:

```text
launch
reach gameplay
XeFG enabled
several Alt+Tab cycles
window/borderless or resolution transition that reaches swapchain lifecycle code
return to gameplay
exit normally
relaunch
```

Verify:

- no repeated `Release()` drain loop;
- no huge/obviously corrupted COM refcount pattern caused by OptiScaler repeatedly releasing the same object;
- no use-after-free around REF overlay/renderer reset;
- final XeFG public-proxy release occurs exactly once through the owner path;
- `xefgSwapChainDestroy` success/failure is logged clearly;
- if Destroy returns `POINTER_STILL_IN_USE`, recreation remains blocked rather than consuming foreign references;
- wrapper/real aliases are cleared when their owner actually releases them, not merely because XeFG proxy teardown ran.

### Regression: known-good RE Engine title

Use at least one of the currently known-good REF + XeFG games such as DD2.

Check launch, gameplay, Alt+Tab, resize/borderless transition if available, and exit.

### OptiScaler without REFramework

Run one XeFG-capable game without REF to verify this remains an OptiScaler ownership fix rather than an REF-only workaround.

---

## 15. Logging

Do not add per-frame ownership logs.

Useful lifecycle logs are sufficient:

```text
[XeFG][Lifecycle] action = destroy_begin
[XeFG][Lifecycle] action = destroy_return
[XeFG][Lifecycle] action = recreate_aborted
[XeFG][Ownership] action = final_proxy_aliases_cleared
```

If needed, add one debug-level wrapper cleanup log showing the wrapper and real identities being retired.

Do not log COM refcount values as a success criterion. A returned count may be diagnostic only.

---

## 16. Acceptance criteria

This work is complete only when all are true:

- [ ] No `Release()`-until-zero behavior is restored.
- [ ] `CreateSwapchain()` and `CreateSwapchain1()` no longer clear wrapper/real aliases as a side effect of XeFG teardown.
- [ ] `hkFGRelease()` no longer clears wrapper/real aliases as a side effect of final XeFG proxy teardown.
- [ ] Final XeFG proxy aliases are cleared by exact identity before its final COM release.
- [ ] `WrappedIDXGISwapChain4::Release()` remains the authority for wrapper/real alias cleanup and releases `_real` exactly once.
- [ ] Failed Intel Destroy remains fail-closed.
- [ ] `oldSwapChain` is not armed for a failed replacement or for an unchanged/live proxy identity.
- [ ] No unrelated XeFG result-status policy changes are included.
- [ ] `git diff --check` passes.
- [ ] Formatting checks pass.
- [ ] Release x64 build passes.
- [ ] MHW + REF + Intel XeFG runtime validation is recorded.
- [ ] At least one known-good RE Engine regression test is recorded.
- [ ] A non-REF XeFG smoke test is recorded.

---

## 17. PR guidance

Suggested branch:

```text
fix/xefg-owner-scoped-swapchain-cleanup
```

Suggested PR title:

```text
Refine XeFG swapchain cleanup ownership boundaries
```

Open as a **Draft PR** after implementation and validation artifacts are ready.

In the PR description, explicitly state:

1. PR #6's force-drain removal is preserved.
2. This PR removes only the cross-owner alias-clearing policy introduced around that cleanup.
3. XeFG final proxy cleanup and wrapper/real cleanup now have separate authorities.
4. No REFramework changes are required.
5. `d87d8ca` warning/result handling is intentionally not part of this PR.

Do not merge automatically.