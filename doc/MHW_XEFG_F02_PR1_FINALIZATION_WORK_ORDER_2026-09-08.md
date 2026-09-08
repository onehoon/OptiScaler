# Work Order: Finalize PR #1 — XeFG Destroy/Recreate Fail-Closed Lifecycle

Date: 2026-09-08  
Repository: `onehoon/OptiScaler`  
Target PR: `#1 Fix XeFG destroy failure lifecycle`  
PR head branch: `fix/mhw-xefg-destroy-recreate-lifecycle`  
Current master after merged F-01 PR #2: `1623de902c1d4160ded39ec7d9d057c9da21ce7e`  
Current PR #1 head at planning time: `a62ebcb930c3608a55e9c20abfb46b668d84999e`

---

# 1. Goal

Finish the **existing PR #1** as the F-02 lifecycle-hardening change after F-01 has already been merged to `master` via PR #2.

Do **not** create a replacement PR unless the existing branch becomes technically unrecoverable.

The desired final topology is:

```text
master
  1623de902...  # includes merged PR #2 / F-01
      ^
      |
PR #1
fix/mhw-xefg-destroy-recreate-lifecycle
```

PR #1 must become a clean `master`-based PR containing only the F-02 destroy/recreate lifecycle changes and any narrowly required XeFG result-semantics correction.

Primary correctness invariant:

> If Intel XeFG reports an actual destroy error, OptiScaler must retain the still-live context/swapchain state and must not create a replacement XeFG context as if teardown succeeded.

This remains a **fail-closed** change. It is not an automatic recovery/retry feature.

---

# 2. Current State

## 2.1 F-01 is already merged

PR #2 has been merged to `master`.

F-01 now owns the backbuffer COM-ownership correction:

- no `Release until refcount <= N` loop in `FGHooks::hkResizeBuffers()`;
- no equivalent loop in `FGHooks::hkResizeBuffers1()`;
- no backbuffer refcount-draining loop in `FGHooks::hkFGRelease()`;
- OptiScaler-owned DX12 menu render-target references are cleaned through their real owner cleanup path;
- obsolete `XEFG_RESOURCE_REF_LIMIT` / `oldBackBuffers` scaffolding was removed.

PR #1 must **not reintroduce or duplicate** those changes.

## 2.2 PR #1 still targets the old F-01 branch

At planning time PR #1 still has:

```text
base: refactor/mhw-disable-resize-backbuffer-release
head: fix/mhw-xefg-destroy-recreate-lifecycle
```

That old base was correct while F-01 was unmerged, but is no longer the desired final structure.

## 2.3 PR #1 contains a confirmed XeFG result-semantics bug

Current `DestroySwapchainContext()` contains logic equivalent to:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _swapChainContext = context;
    LOG_ERROR("[XeFG][Lifecycle] action = destroy_failed, ...");
    return false;
}
```

This is incorrect for Intel XeSS-FG result semantics.

For `xefg_swapchain_result_t`:

```text
result < 0  => error / failure
result == 0 => success
result > 0  => warning / non-failure
```

Therefore PR #1 currently risks treating a positive warning as a destroy failure, retaining/quarantining a context that the vendor call did not report as failed, and aborting valid recreation.

This must be fixed before merge.

---

# 3. Phase 1 — Move Existing PR #1 Onto Current Master

Use the existing branch:

```text
fix/mhw-xefg-destroy-recreate-lifecycle
```

Do not create a third implementation branch for the same F-02 work unless reusing the current branch becomes impossible.

## 3.1 Rebase/update onto master

Update the PR #1 head branch so its effective base is current `master`:

```text
1623de902c1d4160ded39ec7d9d057c9da21ce7e
```

A normal rebase is preferred if practical:

```bash
git fetch origin
git checkout fix/mhw-xefg-destroy-recreate-lifecycle
git rebase origin/master
```

Resolve conflicts by preserving:

- merged F-01 ownership behavior from `master`;
- F-02 destroy/recreate lifecycle changes from PR #1.

Do not resolve conflicts by restoring old F-01 force-release blocks.

If the branch history contains commits that are now already represented by merged PR #2, drop those duplicated commits during the rebase rather than replaying equivalent changes.

## 3.2 Retarget PR #1 to master

After branch history is corrected, PR #1 must target:

```text
base: master
head: fix/mhw-xefg-destroy-recreate-lifecycle
```

Keep PR #1 itself. Do not close it and open an equivalent new PR merely to obtain a cleaner diff.

## 3.3 Verify the post-rebase diff

After retarget/rebase, compare:

```text
master...fix/mhw-xefg-destroy-recreate-lifecycle
```

The diff must no longer show F-01 as a new PR #1 change.

In particular, PR #1 must not appear to newly remove:

```text
XEFG_RESOURCE_REF_LIMIT
oldBackBuffers
ResizeBuffers backbuffer refcount draining
ResizeBuffers1 backbuffer refcount draining
hkFGRelease backbuffer refcount draining
```

Those are already master behavior from PR #2.

Expected F-02 implementation files may include:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
OptiScaler/with_dx12/dx11_with_dx12_sc.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
```

The existing F-02 work-order document may remain if useful, but avoid bringing obsolete duplicated F-01 history into the code diff.

---

# 4. Phase 2 — Fix Intel XeFG Result Semantics

This is mandatory before PR #1 is ready for final review.

## 4.1 Correct failure predicate

For `xefg_swapchain_result_t`, do not use exact-zero inequality as a generic failure test.

Minimum required correction in `DestroySwapchainContext()`:

```cpp
auto result = XeFGProxy::Destroy()(context);

LOG_INFO("[XeFG][Lifecycle] action = destroy_return, context = {:X}, result = {} ({})",
         (size_t) context, magic_enum::enum_name(result), (INT) result);

if (static_cast<int32_t>(result) < 0)
{
    _swapChainContext = context;
    LOG_ERROR("[XeFG][Lifecycle] action = destroy_failed, context = {:X}, retained = true",
              (size_t) context);
    return false;
}

State::Instance().currentFGSwapchain = nullptr;
return true;
```

A shared helper is preferred if it improves consistency without broad refactoring:

```cpp
namespace xefg_result
{
constexpr bool succeeded(xefg_swapchain_result_t result) noexcept
{
    return static_cast<int32_t>(result) >= 0;
}

constexpr bool failed(xefg_swapchain_result_t result) noexcept
{
    return static_cast<int32_t>(result) < 0;
}
}
```

Then:

```cpp
if (xefg_result::failed(result))
{
    ...
}
```

## 4.2 Positive warnings are non-failure

Required lifecycle behavior:

```text
Destroy returns < 0
    -> actual failure
    -> restore/retain exact old context
    -> retain currentFGSwapchain
    -> ReleaseSwapchain returns false
    -> replacement creation aborts

Destroy returns 0
    -> success
    -> normal release completion

Destroy returns > 0
    -> warning / non-failure
    -> do NOT enter destroy_failed path
    -> normal release completion
```

Do not rename a positive warning to `destroy_failed` in logs.

## 4.3 Audit other XeFG exact-success checks on the PR branch

Search the XeFG integration for decisions involving `xefg_swapchain_result_t`, including patterns such as:

```text
result != XEFG_SWAPCHAIN_RESULT_SUCCESS
result == XEFG_SWAPCHAIN_RESULT_SUCCESS
XEFG_SWAPCHAIN_RESULT_SUCCESS
```

At minimum inspect:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/proxies/XeFG_Proxy.*
```

Current `XeFG_Dx12.cpp` contains several exact-success checks around calls such as:

```text
D3D12CreateContext
SetLoggingCallback
SetLatencyReduction
GetProperties
Destroy
other XeFG swapchain APIs
```

Classify each occurrence before modifying it:

### Convert

Convert generic success/failure decisions where the return type is `xefg_swapchain_result_t` and Intel's non-negative success semantics apply.

### Do not mechanically convert

Do not change:

- HRESULT comparisons;
- XeLL result enums unless their documented semantics independently require it;
- comparisons that intentionally test for one specific warning/result value;
- unrelated integer/status enums.

Keep this audit focused on correctness. Do not turn PR #1 into a broad XeFG refactor.

---

# 5. Phase 3 — Preserve F-02 Fail-Closed Lifecycle Invariants

The existing PR contains the core F-02 design. Preserve it unless review finds a concrete defect.

## 5.1 `DestroySwapchainContext()`

Required behavior:

```text
no context
    -> return true

process shutdown path
    -> preserve existing shutdown policy unless a concrete regression is found

vendor Destroy actual error (< 0)
    -> restore exact context handle
    -> return false

vendor Destroy non-failure (>= 0)
    -> complete successful destroy state transition
    -> return true
```

Never clear `_swapChainContext` permanently after a negative Destroy result.

Never clear `currentFGSwapchain` as if teardown completed after a negative Destroy result.

## 5.2 `ReleaseSwapchain()` is authoritative

The result of XeFG context destruction must control the transaction.

Required failure path:

```text
ReleaseSwapchain
  -> DestroySwapchainContext
  -> vendor error
  -> preserve _swapChainContext
  -> preserve currentFGSwapchain
  -> do not execute normal successful-release tail
  -> release any mutex owned by this transaction
  -> return false
```

Required success/non-failure path:

```text
ReleaseSwapchain
  -> DestroySwapchainContext >= 0
  -> complete cleanup
  -> clear state appropriate to successful teardown
  -> return true
```

## 5.3 Same-window recreation must stop on failed teardown

Both:

```text
XeFG_Dx12::CreateSwapchain()
XeFG_Dx12::CreateSwapchain1()
```

must obey:

```cpp
if (!ReleaseSwapchainLocked(_hwnd))
{
    LOG_ERROR("[XeFG][Lifecycle] action = recreate_aborted, ...");
    return false;
}
```

No new XeFG swapchain context may be initialized after the release transaction reports failure.

## 5.4 Caller state clearing must respect failure

Recheck every caller changed by PR #1.

Important callers currently include:

```text
FGHooks::hkFGRelease
ffxDestroyContext_Dx12FG
Dx11wDx12SC::Release
WrappedIDXGISwapChain4 release/destruction paths
```

For each caller verify this invariant:

```text
ReleaseSwapchain() == false
    -> caller does not immediately clear the same global/context identity
    -> caller does not immediately continue replacement creation
```

Do not assume `ReleaseSwapchain()` always succeeds now that it returns a meaningful bool.

---

# 6. Phase 4 — Review Lifecycle Locking for Concrete Regressions

PR #1 introduces:

```cpp
std::mutex _swapchainLifecycleMutex;
std::atomic_bool _swapchainReleaseInProgress { false };
```

and uses `std::try_to_lock` in create/release entry points.

Do not remove this merely because it is conservative.

But before finalization, inspect actual call paths for these concrete failure classes.

## 6.1 No self-deadlock

`CreateSwapchain()` / `CreateSwapchain1()` acquire the lifecycle mutex and then call the internal locked helper rather than recursively acquiring the same mutex.

Preserve that structure.

## 6.2 Configured FG mutex must always be released on early error paths

Where `FGUseMutexForSwapchain` is enabled, every path after successful `Mutex.lock(1)` must either:

- complete normal cleanup and unlock; or
- explicitly unlock before returning failure.

In particular verify the negative Destroy early-return path.

## 6.3 `release_already_in_progress` must not report success

A second/re-entrant release attempt must not return `true` if the owning release transaction has not completed.

Returning false/deferred is appropriate for fail-closed semantics.

## 6.4 Do not over-engineer theoretical races

Only change locking further if inspection finds a realistically reachable problem in the current call graph.

Do not add condition variables, background retries, or complex state machines merely to make concurrent lifecycle calls wait instead of fail closed.

---

# 7. Logging Requirements

Retain compact lifecycle logs useful for field diagnosis.

Useful events include:

```text
[XeFG][Lifecycle] action = destroy_begin
[XeFG][Lifecycle] action = destroy_return
[XeFG][Lifecycle] action = destroy_failed
[XeFG][Lifecycle] action = release_swapchain_aborted
[XeFG][Lifecycle] action = recreate_aborted
```

Requirements:

- no per-frame lifecycle spam;
- log the actual signed XeFG result meaningfully;
- positive warning results must not be logged as destroy failures;
- retain context/swapchain pointer identity where useful for correlating transitions;
- do not add large diagnostic dumps to normal release paths.

Consider logging the numeric XeFG result as signed (`int32_t`) rather than unsigned so negative vendor errors are obvious in support logs.

Example:

```cpp
LOG_INFO("[XeFG][Lifecycle] action = destroy_return, context = {:X}, result = {} ({})",
         (size_t) context,
         magic_enum::enum_name(result),
         static_cast<int32_t>(result));
```

---

# 8. Do Not Regress Merged F-01 Behavior

After rebase/conflict resolution, explicitly verify that current master F-01 behavior remains intact.

The following must remain absent from the backbuffer resize/release paths:

```cpp
while (refCount > XEFG_RESOURCE_REF_LIMIT)
{
    refCount = backBuffer->Release();
}
```

Do not restore `XEFG_RESOURCE_REF_LIMIT` or `oldBackBuffers` as part of conflict resolution.

`hkResizeBuffers()` and `hkResizeBuffers1()` must continue to clean OptiScaler's own DX12 menu render-target references through `MenuOverlayDx::CleanupRenderTarget(...)` rather than draining global COM refcounts.

Do not broaden PR #1 into further backbuffer ownership work unless a new concrete defect is found.

---

# 9. Out of Scope

Do not add the following to PR #1:

- REFramework changes;
- forced release of references owned by REFramework, the game, or third-party overlays;
- repeated automatic `xefgSwapChainDestroy()` retry loops;
- background recovery/recreation workers;
- Intel driver-specific reset hacks without evidence;
- frame-tagging redesign;
- interpolation logic changes;
- queue-selection redesign;
- Streamline changes;
- DLSSG/FSRFG behavior changes except where required to preserve an existing shared caller contract;
- `FGPreserveSwapChain` default changes;
- broad COM ownership refactoring outside the lifecycle being fixed;
- speculative cleanup of unrelated existing force-release logic.

Keep PR #1 centered on:

```text
actual XeFG Destroy error
  -> retain state
  -> propagate failure
  -> prevent invalid recreation
```

---

# 10. Validation

## 10.1 Repository state

Before final review verify:

```text
PR base = master
PR head = fix/mhw-xefg-destroy-recreate-lifecycle
master includes PR #2 / F-01
PR diff does not duplicate F-01
```

## 10.2 Static search

Search for stale exact-success logic in relevant XeFG result paths:

```text
XEFG_SWAPCHAIN_RESULT_SUCCESS
result != XEFG_SWAPCHAIN_RESULT_SUCCESS
result == XEFG_SWAPCHAIN_RESULT_SUCCESS
```

Manually classify every changed occurrence.

Search lifecycle caller state clearing:

```text
ReleaseSwapchain(
currentFGSwapchain = nullptr
_swapChainContext = nullptr
CreateSwapchain(
CreateSwapchain1(
```

Confirm failure paths do not clear/replace retained state.

## 10.3 Build

Run the repository's normal Release x64 build, e.g.:

```text
MSBuild.exe OptiScaler.sln /m:1 /p:Configuration=Release /p:Platform=x64 /t:Build /nologo /verbosity:minimal
```

Use the exact repository-supported build invocation if it differs.

Required:

```text
Release x64 build: PASS
```

## 10.4 Diff hygiene

Run:

```bash
git diff --check
```

Required:

```text
PASS
```

Inspect final diff for accidental F-01 reintroduction or unrelated refactors.

## 10.5 Runtime validation

Hardware runtime validation is desirable but not required to prove the static lifecycle correction before code review.

Primary target:

```text
Monster Hunter Wilds
Intel XeFG output
REFramework + OptiScaler combination
```

Useful normal-path checks:

- startup;
- gameplay;
- repeated Alt+Tab;
- window/fullscreen transitions where supported;
- ordinary ResizeBuffers activity;
- clean game exit;
- no immediate overlay/regression introduced by the lifecycle guard.

If an actual negative Destroy result is reproduced, expected log sequence is conceptually:

```text
destroy_begin
destroy_return with negative result
destroy_failed
release_swapchain_aborted
recreate_aborted
```

There must **not** be a successful new XeFG context/swapchain initialization immediately following that failed destroy transaction.

Positive-warning test, if practically injectable/testable:

```text
Destroy result > 0
  -> must NOT emit destroy_failed
  -> must follow non-failure lifecycle path
```

Do not fabricate vendor warning results in production code solely for testing unless an existing test seam allows it cleanly.

---

# 11. PR #1 Description Cleanup

After rebasing onto master, update the existing PR #1 description so it no longer says the base is the F-01 experimental branch.

Replace obsolete wording such as:

```text
Base: refactor/mhw-disable-resize-backbuffer-release
```

with current reality:

```text
Base: master
F-01 backbuffer COM ownership hardening is already merged via PR #2.
This PR contains F-02 only: failed XeFG destroy propagation and fail-closed recreation prevention.
```

Also update result semantics in the description:

Do not describe `result != SUCCESS` as failure.

Use:

```text
negative XeFG result => failure
non-negative XeFG result => non-failure
```

Keep runtime-validation status accurate. Do not claim the MHW long-runtime issue is proven fixed until field testing demonstrates it.

---

# 12. Final Acceptance Criteria

PR #1 is ready for final review only when all of the following are true:

- [ ] Existing PR #1 is retained; no redundant replacement PR was created.
- [ ] Head branch is updated onto current `master` containing merged PR #2.
- [ ] PR #1 base is `master`.
- [ ] Final PR diff does not duplicate merged F-01 backbuffer ownership changes.
- [ ] `DestroySwapchainContext()` treats **only negative** `xefg_swapchain_result_t` values as failure.
- [ ] Positive XeFG warnings follow the non-failure path.
- [ ] Relevant XeFG exact-success checks were audited and corrected where the same documented semantics apply.
- [ ] Negative Destroy restores/retains the exact old `_swapChainContext`.
- [ ] Negative Destroy preserves `currentFGSwapchain` identity.
- [ ] `ReleaseSwapchain()` returns false on incomplete teardown.
- [ ] Same-window `CreateSwapchain()` and `CreateSwapchain1()` abort if release did not complete.
- [ ] Lifecycle-sensitive callers do not clear retained state after `ReleaseSwapchain() == false`.
- [ ] Configured FG mutex is released correctly on the negative-Destroy early-return path.
- [ ] Lifecycle locking does not recursively acquire the same mutex through the normal create/release flow.
- [ ] F-01 `Release until refcount <= N` backbuffer logic remains absent.
- [ ] Release x64 build passes.
- [ ] `git diff --check` passes.
- [ ] PR description is updated for `master` base and signed/non-negative XeFG result semantics.
- [ ] Hardware runtime validation status is reported accurately.

---

# 13. Completion Handoff

When implementation is complete, leave PR #1 in a reviewable state and report:

```text
1. final PR base/head
2. final head SHA
3. files changed vs master
4. exact XeFG result-semantics changes made
5. confirmation that negative Destroy preserves context/swapchain state
6. confirmation that non-negative Destroy completes normal teardown
7. caller paths audited for ReleaseSwapchain(false)
8. build result
9. git diff --check result
10. runtime validation performed, if any
11. any remaining evidence-gated concern that should NOT block merge
```

Do not merge PR #1 automatically unless explicitly instructed after review.
