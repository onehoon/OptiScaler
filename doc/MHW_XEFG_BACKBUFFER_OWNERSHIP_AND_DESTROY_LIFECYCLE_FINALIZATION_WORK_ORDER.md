# Work Order: Finalize XeFG Backbuffer Ownership and Destroy/Recreate Lifecycle Hardening

Date: 2026-09-08  
Repository: `onehoon/OptiScaler`  
Current upstream/fork master at planning time: `da70e61e1542a0b99adcb24168ff941e42109567`  
Existing F-01 branch: `refactor/mhw-disable-resize-backbuffer-release`  
Existing F-01 implementation commit: `8707e640506d799c66c8bfba88cf8239f21380d0`  
Existing F-02 PR: `#1` (`fix/mhw-xefg-destroy-recreate-lifecycle`)  
Existing F-02 PR head at planning time: `a62ebcb930c3608a55e9c20abfb46b668d84999e`

---

# 1. Goal

Finalize the two existing XeFG lifecycle fixes without creating a third independent implementation path.

The intended sequence is:

```text
master
  -> finalize existing F-01 branch
  -> open F-01 PR to master
  -> review + merge F-01
  -> rebase/retarget existing PR #1 onto updated master
  -> fix PR #1 result semantics
  -> review + merge PR #1
```

Do **not** create a separate replacement implementation branch for the same ownership/lifecycle problems unless the existing branches become technically unusable.

The two problems are:

1. **F-01 — unsafe aggressive backbuffer COM release**
2. **F-02 — failed XeFG destroy treated as completed teardown, allowing recreation over a still-live context**

The fixes should remain separated into two PRs so runtime failures remain diagnosable and review scope stays narrow.

---

# 2. Background and Correctness Model

## 2.1 F-01: OptiScaler must release only references it owns

Current upstream-style XeFG resize code contains logic equivalent to:

```cpp
ID3D12Resource* backBuffer = nullptr;
if (SUCCEEDED(This->GetBuffer(i, IID_PPV_ARGS(&backBuffer))))
{
    auto refCount = backBuffer->Release();
    while (refCount > XEFG_RESOURCE_REF_LIMIT)
    {
        refCount = backBuffer->Release();
    }
}
```

This is not ownership-safe.

`GetBuffer()` gives OptiScaler one COM reference. OptiScaler may release that one reference. It may **not** infer ownership of the remaining references from the returned refcount and repeatedly call `Release()` until the total count reaches a chosen threshold.

Other live references may belong to:

- the game;
- REFramework;
- another overlay;
- another DXGI observer/wrapper;
- XeFG/runtime-owned structures;
- OptiScaler subsystems other than the local code performing the probe.

A repeated `Release()` loop can therefore consume references that OptiScaler never acquired. Any surviving owner will then retain a pointer whose COM ownership has already been consumed, creating a later double-release/UAF/refcount-corruption risk.

The existing F-01 branch correctly removed this aggressive block from:

- `FGHooks::hkResizeBuffers()`
- `FGHooks::hkResizeBuffers1()`

That direction is correct and must be preserved.

## 2.2 ResizeBuffers still requires the caller to release its own references

Removing the force-release loop does **not** mean ignoring legitimate OptiScaler-owned resources.

DXGI and Intel XeSS-FG require backbuffer references owned by the application/integration to be released before swapchain buffer recreation.

The correct rule is:

> Each component releases the references it actually owns. No component forcibly decrements another component's COM ownership.

OptiScaler now has a concrete ownership example in the DX12 ImGui menu.

At current master `da70e61...`, `CreateRenderTargetDx12()` keeps the `GetBuffer()` references in:

```cpp
g_mainRenderTargetResource[]
```

and `CleanupRenderTargetDx12()` correctly releases them via `SAFE_RELEASE()`.

Therefore the resize path must explicitly execute the existing OptiScaler-owned cleanup where required rather than relying on a global refcount-draining loop.

## 2.3 F-02: a failed XeFG destroy is not a completed teardown

Existing PR #1 addresses the second bug:

```text
xefgSwapChainDestroy(oldContext)
  -> failure
  -> old context is still alive
```

OptiScaler must not then do this:

```text
_swapChainContext = nullptr
currentFGSwapchain = nullptr
create another XeFG context
```

PR #1 correctly moves toward fail-closed lifecycle behavior:

```text
Destroy failure
  -> retain exact old context handle
  -> retain currentFGSwapchain identity
  -> ReleaseSwapchain returns false
  -> CreateSwapchain/CreateSwapchain1 abort replacement
  -> no second XeFG context is created over the old one
```

Keep that behavior.

---

# 3. Required Phase A — Finalize the Existing F-01 Branch

Target branch:

```text
refactor/mhw-disable-resize-backbuffer-release
```

Do not replace this branch with a third implementation branch.

## 3.1 Preserve the existing ResizeBuffers / ResizeBuffers1 removal

Keep the current F-01 removal in both:

```text
FGHooks::hkResizeBuffers()
FGHooks::hkResizeBuffers1()
```

The following pattern must remain absent from those two paths:

```cpp
while (refCount > XEFG_RESOURCE_REF_LIMIT)
{
    refCount = backBuffer->Release();
}
```

Do not reintroduce the loop with a different limit.

Do not replace it with:

- `while (refCount > 0)`;
- a retry loop;
- AddRef/Release balancing intended to discover external ownership;
- manual destruction of references owned by REFramework or the game.

## 3.2 Remove the same unsafe backbuffer force-release from `hkFGRelease()`

The current F-01 branch still contains another aggressive backbuffer release sequence inside:

```text
FGHooks::hkFGRelease()
```

It currently performs roughly:

```cpp
for (...)
{
    ID3D12Resource* backBuffer = nullptr;
    if (SUCCEEDED(((IDXGISwapChain*)This)->GetBuffer(i, IID_PPV_ARGS(&backBuffer))))
    {
        auto refCount = backBuffer->Release();
        while (refCount > XEFG_RESOURCE_REF_LIMIT)
        {
            refCount = backBuffer->Release();
        }
    }
}
```

Delete this force-release behavior as part of F-01 finalization.

Reason:

- it has the same ownership violation as the resize paths;
- it executes before the later XeFG release/destroy transaction;
- it can consume references held by REFramework or another component before that component receives its normal lifecycle callback and releases its own references.

The release path must not use global COM refcount draining as a substitute for lifecycle correctness.

## 3.3 Ensure OptiScaler-owned DX12 menu backbuffers are released before both resize APIs

Current master owns DX12 overlay backbuffer references through:

```text
OptiScaler/menu/menu_overlay_dx.cpp
  g_mainRenderTargetResource[]
```

and releases them through:

```cpp
MenuOverlayDx::CleanupRenderTarget(false, NULL);
```

`hkResizeBuffers1()` already performs this cleanup before the actual resize call.

Audit `hkResizeBuffers()` and ensure it performs equivalent OptiScaler-owned cleanup before calling the real resize function.

Recommended shape:

```cpp
if (fg != nullptr && fg->IsActive())
{
    State::Instance().fgChanged = true;
    fg->UpdateTarget();
    fg->Deactivate();
}

if (Config::Instance()->OverlayMenu.value_or_default())
{
    MenuOverlayDx::CleanupRenderTarget(false, NULL);
}

HRESULT result;
{
    ScopedSkipSpoofingGlobal skipSpoofingGlobal{};
    result = o_FGSCResizeBuffers(...);
}
```

Match repository style and existing call ordering where practical.

The important invariant is:

```text
OptiScaler-owned overlay backbuffer refs released
  BEFORE real ResizeBuffers / ResizeBuffers1
```

Do not use force-release loops to compensate for missing local cleanup.

## 3.4 Audit other OptiScaler-owned backbuffer references relevant to these transitions

Before finalizing F-01, search the current branch for backbuffer ownership patterns, especially:

```text
GetBuffer(
ID3D12Resource*
ComPtr<ID3D12Resource>
g_mainRenderTargetResource
currentFGSwapchain
wrapped swapchain resize cleanup
```

Classify each relevant reference into one of these groups:

### A. Clearly OptiScaler-owned

Examples:

- a pointer stored after OptiScaler directly calls `GetBuffer()`;
- a `ComPtr` populated by OptiScaler;
- an explicit AddRef performed by OptiScaler.

These must be released through their actual owner cleanup path.

### B. Borrowed / externally owned

Do not `Release()` these unless the local code acquired a corresponding reference.

### C. Temporary GetBuffer reference

One successful `GetBuffer()` call creates one reference. Release exactly that reference once.

### D. Diagnostic/refcount-probing code

Do not retain logic that drains total refcount in production lifecycle code.

Do not broaden this phase into a general resource tracking refactor. Only fix concrete ownership relevant to XeFG swapchain resize/release transitions.

## 3.5 Clean up obsolete F-01 scaffolding if it is no longer used

After removing all production backbuffer force-release paths, audit:

```cpp
#define XEFG_RESOURCE_REF_LIMIT 1
oldBackBuffers
#if (XEFG_RESOURCE_REF_LIMIT == 0)
```

If these exist only to support the removed force-release/refcount-draining mechanism, delete them and their associated stale branches/comments.

Do not remove `oldSwapChain` or unrelated release guards unless they are independently proven obsolete.

## 3.6 Do not modify F-02 behavior in Phase A

Phase A must not implement the destroy/recreate hardening from PR #1.

Specifically do not yet change:

- `DestroySwapchainContext()` return semantics;
- `ReleaseSwapchain()` destroy-failure propagation;
- CreateSwapchain/CreateSwapchain1 recreate-abort behavior;
- lifecycle mutex additions from PR #1.

Keep F-01 reviewable as an ownership-safe backbuffer lifecycle correction.

---

# 4. Phase A PR Requirements

After F-01 branch finalization, open a PR:

```text
base: master
head: refactor/mhw-disable-resize-backbuffer-release
```

Suggested title:

```text
Fix XeFG backbuffer COM ownership during resize and release
```

Suggested summary:

```text
- remove aggressive Release-until-refcount-limit behavior from XeFG resize/release paths
- release only backbuffer references actually owned by OptiScaler
- ensure DX12 ImGui render-target references are cleaned before both ResizeBuffers variants
- remove obsolete refcount-draining scaffolding where no longer used
```

The PR description must explicitly state:

> This change does not assume that a high COM refcount is itself an error. It stops OptiScaler from consuming references it does not own. If external references prevent a later XeFG destroy, that failure is handled separately by the existing F-02 lifecycle hardening PR.

---

# 5. Required Phase B — Update Existing PR #1, Do Not Replace It

Existing PR:

```text
#1 Fix XeFG destroy failure lifecycle
head: fix/mhw-xefg-destroy-recreate-lifecycle
```

After Phase A is merged:

1. rebase or otherwise update the existing PR #1 branch onto the new `master`;
2. retarget PR #1 to `master` if GitHub still shows the old F-01 branch as its base;
3. keep the existing fail-closed destroy/recreate behavior;
4. fix the XeFG result semantics described below.

Do not close PR #1 just to create an equivalent replacement PR unless branch history becomes unrecoverable.

---

# 6. Mandatory PR #1 Fix — XeFG Result Semantics

Current PR #1 contains logic equivalent to:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _swapChainContext = context;
    return false;
}
```

This is incorrect for Intel XeSS-FG result semantics.

Intel XeFG semantics are:

```text
0   = success
> 0 = warning / non-failure
< 0 = error
```

Therefore lifecycle failure must be based on a negative result, not exact-zero inequality.

Preferred helper:

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

A local helper or equivalent strongly typed implementation is acceptable if consistent with OptiScaler style.

Then use failure semantics such as:

```cpp
if (xefg_result::failed(result))
{
    _swapChainContext = context;
    LOG_ERROR(...);
    return false;
}
```

Positive warning results must continue through the non-failure lifecycle path.

## 6.1 Audit XeFG result checks in the PR branch

Before finalizing PR #1, search relevant XeFG code for:

```text
XEFG_SWAPCHAIN_RESULT_SUCCESS
result == 0
result != 0
result == XEFG_SWAPCHAIN_RESULT_SUCCESS
result != XEFG_SWAPCHAIN_RESULT_SUCCESS
```

Classify each occurrence:

- lifecycle success/failure decision;
- exact enum diagnostic branch;
- unrelated integer/HRESULT comparison.

Only convert checks where Intel XeFG success/non-failure semantics apply.

Do not mechanically replace unrelated HRESULT logic.

---

# 7. Required F-02 Invariants to Preserve in PR #1

After the result-semantics fix, PR #1 must still guarantee:

## Destroy failure

```text
XeFG Destroy returns negative error
  -> exact old _swapChainContext restored/retained
  -> currentFGSwapchain retained
  -> ReleaseSwapchain returns false
  -> successful-release tail does not run
  -> recreation caller aborts
  -> no replacement XeFG context is created
```

## Destroy success or positive warning/non-failure

```text
XeFG Destroy returns >= 0
  -> treat vendor call as non-failure
  -> complete normal successful-release state transition
```

## Mutex/lifecycle transaction safety

Keep the PR's existing safeguards that prevent:

- returning from a failed destroy while the configured swapchain mutex remains owned;
- overlapping create/release transactions;
- immediately creating a second context after release failure.

Any lifecycle mutex introduced by PR #1 must be reviewed for real deadlock/reentrancy risk, but do not remove it merely for stylistic simplification.

---

# 8. Out of Scope

Do not use these two PRs to implement unrelated XeFG changes.

Out of scope:

- REFramework code changes;
- REFramework-specific pointer manipulation;
- forced release of REFramework-owned resources;
- Intel driver workarounds unrelated to concrete lifecycle failures;
- XeFG frame tagging changes;
- interpolation logic changes;
- queue selection redesign;
- Streamline changes;
- DLSSG/FSRFG behavior changes unless required to preserve a shared caller contract;
- broad wrapped-swapchain refactor;
- repeated automatic Destroy retry loops;
- automatic recovery by forgetting a failed context;
- changing `FGPreserveSwapChain` defaults;
- speculative performance tuning.

---

# 9. Validation

## Phase A static validation

At minimum:

```text
- build Release x64
- git diff --check
- search confirms no Release-until-refcount-limit loop remains in the targeted FGHooks backbuffer paths
- both ResizeBuffers variants release OptiScaler-owned DX12 menu render-target references before the real resize call
- no new unconditional Release is applied to borrowed backbuffer pointers
```

Recommended source audit searches:

```text
XEFG_RESOURCE_REF_LIMIT
oldBackBuffers
while (refCount
GetBuffer(
CleanupRenderTarget(false
hkResizeBuffers
hkResizeBuffers1
hkFGRelease
```

## Phase B static validation

At minimum:

```text
- build Release x64
- git diff --check
- negative XeFG Destroy result retains context and aborts recreation
- zero result completes successful lifecycle
- positive XeFG result follows non-failure lifecycle
- no caller clears currentFGSwapchain after ReleaseSwapchain() reports failure
```

## Runtime matrix

Primary target:

```text
Monster Hunter Wilds
+ fork REFramework latest master
+ fork OptiScaler
+ Intel XeFG output
```

Exercise:

```text
- startup
- normal gameplay
- repeated Alt+Tab
- window resize if supported
- fullscreen/borderless transitions where applicable
- long-duration gameplay
- shutdown/restart
```

Also perform at least one non-REFramework XeFG smoke test if practical. The ownership correction should not depend on REFramework being present.

Useful failure evidence:

```text
ResizeBuffers HRESULT
ResizeBuffers1 HRESULT
XeFG Destroy numeric result
context pointer
currentFGSwapchain pointer
release_swapchain_aborted
recreate_aborted
```

---

# 10. Review / Merge Order

Do not merge PR #1 before the finalized F-01 PR if PR #1 still depends on the F-01 branch.

Required order:

```text
1. finalize refactor/mhw-disable-resize-backbuffer-release
2. open F-01 PR -> master
3. review F-01 carefully
4. merge F-01
5. update/rebase/retarget existing PR #1 -> master
6. apply XeFG >=0 / <0 result semantics fix in PR #1
7. review PR #1 carefully
8. merge PR #1
```

If Phase A reveals a concrete blocker that invalidates PR #1 assumptions, stop before rebasing PR #1 and document the actual blocker. Do not create speculative replacement fixes.

---

# 11. Final Intended Architecture

The finished fork behavior should follow this ownership/lifecycle model:

```text
Resize/release begins
    |
    +-- OptiScaler releases references OptiScaler actually owns
    |     - DX12 menu render-target backbuffers
    |     - other proven local owners
    |
    +-- OptiScaler does NOT drain global COM refcounts
    |
    +-- REFramework/game/other components release their own refs through their lifecycle
    |
    +-- Resize/Destroy proceeds
            |
            +-- Destroy >= 0
            |     -> normal cleanup/recreation may continue
            |
            +-- Destroy < 0
                  -> old context remains authoritative
                  -> release transaction fails closed
                  -> replacement creation is blocked
```

This is the key design rule for both PRs:

> **Ownership is local; lifecycle failure is explicit. Do not steal external COM references, and do not pretend a failed XeFG destroy succeeded.**
