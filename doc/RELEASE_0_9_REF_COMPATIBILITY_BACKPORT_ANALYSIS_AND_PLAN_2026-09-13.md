# OptiScaler `release/0.9` — REFramework Compatibility Backport Analysis and Plan

**Repository:** `onehoon/OptiScaler`  
**Document branch:** `master`  
**Master baseline reviewed:** `7ee16d9b8b5405451322a840e1adbf1efe24dcf1`  
**Target runtime branch:** `release/0.9`  
**`release/0.9` baseline reviewed:** `132bc110f371d273834681ae05c73db4212bd337`  
**Date:** 2026-09-13  

## 1. Purpose

This document defines a narrow backport plan for bringing the **REFramework coexistence and XeFG swapchain-lifecycle hardening work from the fork `master` line into `release/0.9`**.

The governing project objective remains the REFramework + OptiScaler + Intel XeFG configuration described in:

- `onehoon/REFramework/doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md`

The desired topology is:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (fork)
Intel XeFG   = enabled through OptiScaler
Special K    = absent
```

The target is **not** a general `release/0.9` modernization and **not** a request to port the current `master` XeLL/fakenvapi architecture.

The objective is only to preserve the compatibility rules learned while making OptiScaler coexist more safely with REFramework and the XeFG proxy swapchain.

The central conclusion of the analysis is:

> Do not cherry-pick the full master history. Re-implement the final ownership and lifecycle invariants in `release/0.9` using the `release/0.9` architecture.

---

## 2. Explicit Scope

### In scope

Only the following classes of changes are candidates for backport:

1. XeFG backbuffer COM ownership cleanup.
2. Swapchain wrapper/real/proxy ownership-boundary cleanup.
3. Fail-closed XeFG Destroy and recreation lifecycle behavior.
4. Minimal lifecycle diagnostics needed to confirm the above behavior.

### Out of scope

The following must **not** be mixed into this backport unless a later independent investigation explicitly requests them:

- `master` XeLL/fakenvapi integration redesign.
- `LOW_LATENCY_INPUTS` / InputXeLL architecture.
- FSRFG texture corruption fixes.
- DLSSG/FSRFG frame-ID or slot-generation redesign.
- Subnautica 2 Streamline/XeFG geometry fixes or PR #21 diagnostics.
- RE9/PRAGMATA Reflex availability work.
- general DXGI refactoring.
- general swapchain modernization unrelated to the verified ownership hazards.
- PR #17 thread-local reentrancy-guard experiment.
- PR #18 thread-aware `OwnedMutex` experiment.
- PR #19 automatic XeFG crash trace.
- CI/release workflow changes.

This separation is important because `release/0.9` and `master` have materially different low-latency and fake-NVAPI architecture. The purpose of this work is to evaluate the proven ownership/lifecycle invariants without importing those unrelated architectural differences.

---

## 3. Why `release/0.9` Is Worth Hardening Separately

`release/0.9` is not treated as a known-good rendering baseline. In current testing, it can progress differently from `master`, but some FSRFG-to-XeFG cases may produce invalid/corrupted scene output.

That rendering problem is intentionally excluded from this document.

For the REFramework compatibility project, `release/0.9` is still useful because:

- its XeLL ownership model is older and separate from current `master` fakenvapi integration;
- its XeFG swapchain code still contains the historical COM force-drain behavior that was later removed on the fork `master` line;
- therefore it can be used to test whether the ownership/lifecycle fixes themselves improve REF coexistence independently of the newer master low-latency architecture.

The correct question for this backport is not:

> Does `release/0.9` render every FG input correctly?

The correct question is:

> With the same older `release/0.9` architecture, does removing invalid COM ownership behavior and enforcing fail-closed XeFG lifecycle rules improve OptiScaler + REFramework coexistence?

---

## 4. Master Changes Reviewed

The fork master history contains several different investigations. Only a subset is relevant to this backport.

### 4.1 PR #2 — `Fix XeFG backbuffer COM ownership during resize and release`

PR:

- https://github.com/onehoon/OptiScaler/pull/2

Merged master intent:

- remove aggressive backbuffer `Release()` loops that drain references until a returned refcount reaches a threshold;
- remove obsolete `XEFG_RESOURCE_REF_LIMIT` / `oldBackBuffers` ownership heuristics;
- release only references that OptiScaler actually owns, especially menu-render-target references around resize.

**Backport decision: REQUIRED.**

### 4.2 PR #6 — `Remove remaining XeFG swapchain COM ownership drains`

PR:

- https://github.com/onehoon/OptiScaler/pull/6

Merged master intent:

- treat `State::currentSwapchain`, `currentWrappedSwapchain`, `currentRealSwapchain`, and `currentFGSwapchain` as tracking aliases rather than implicit COM owners;
- remove `currentRealSwapchain` release-until-zero behavior;
- remove `currentWrappedSwapchain` release-until-zero behavior;
- make the wrapper release only the real-swapchain reference that the wrapper actually owns;
- clear aliases by pointer identity instead of using global aliases as release authority.

**Backport decision: REQUIRED, but not as a raw PR #6 cherry-pick.**

### 4.3 PR #13 — `Refine XeFG swapchain cleanup ownership boundaries`

PR:

- https://github.com/onehoon/OptiScaler/pull/13

PR #13 refined PR #6 after review. The final rule is stricter:

- XeFG public-proxy cleanup must remain separate from wrapper/real-swapchain cleanup;
- the XeFG teardown path must not clear wrapper/real aliases that belong to a different owner domain;
- wrapper-owned `_real` cleanup remains a wrapper responsibility;
- State pointers remain observational/tracking aliases.

**Backport decision: REQUIRED together with PR #6 as one final ownership model.**

Do not reproduce the intermediate PR #6 state and then reproduce PR #13 as a second correction. Implement the PR #13-corrected final model directly.

### 4.4 PR #1 — `Fix XeFG destroy failure lifecycle`

PR:

- https://github.com/onehoon/OptiScaler/pull/1

Relevant master intent:

- if `xefgSwapChainDestroy()` does not complete successfully, retain the old context identity;
- propagate teardown failure to the caller;
- do not clear state as if teardown succeeded;
- do not create a replacement XeFG context on top of a still-live old context;
- serialize/guard create-release lifecycle transitions;
- clean up partially initialized XeFG contexts when initialization aborts.

**Backport decision: REQUIRED in concept, manually adapted to `release/0.9`.**

PR #1 accumulated multiple revisions on master. Its final lifecycle invariant is valuable, but the code must not be mechanically transplanted because the target branch architecture differs.

### 4.5 `d87d8ca` and PR #14 result-status policy

Historical commit:

- `d87d8ca8c88c3c213163c829eb8aeac3e86d79b5` — `fix: honor non-negative XeFG result statuses`

Follow-up PR:

- https://github.com/onehoon/OptiScaler/pull/14

The temporary broad rule in `d87d8ca` treated all non-negative XeFG result values as successful enough for many call sites. PR #14 later narrowed this and restored **exact `XEFG_SWAPCHAIN_RESULT_SUCCESS` requirements for lifecycle, state-transition, and output-validity gates**.

`release/0.9` already uses exact-success checks at many key XeFG call sites.

**Backport decision:**

- do **not** backport `d87d8ca` wholesale;
- do **not** convert `release/0.9` to a blanket `result >= 0` success policy;
- preserve `release/0.9` exact-success semantics for context creation, latency reduction, Destroy, init, swapchain retrieval, activation/deactivation, and resource-tagging recovery gates;
- optional warning classification/logging from PR #14 may be considered later, but it is not required for the REF compatibility backport.

---

## 5. Confirmed Problems Still Present in `release/0.9`

The `release/0.9` baseline `132bc110...` still contains the same ownership hazards that motivated the master fixes.

### 5.1 Backbuffer refcount draining still exists

`OptiScaler/hooks/FG_Hooks.cpp` contains repeated patterns equivalent to:

```cpp
auto refCount = backBuffer->Release();
while (refCount > XEFG_RESOURCE_REF_LIMIT)
{
    refCount = backBuffer->Release();
}
```

The problem is conceptual, not only numeric:

> The refcount returned by `Release()` is not an ownership map.

A high refcount can represent references held by:

- the game;
- Intel XeFG;
- REFramework;
- the OptiScaler wrapper;
- another overlay or observer;
- another DXGI/D3D component.

OptiScaler may release the reference it acquired through its own `GetBuffer()` call. It must not repeatedly consume references simply because the object remains alive afterward.

### 5.2 Wrapped swapchain force-drain still exists

The `release/0.9` `hkFGRelease()` path still contains logic equivalent to:

```cpp
auto refCount = State::Instance().currentWrappedSwapchain->Release();
while (refCount > 0 && refCount < 0xffffff00)
{
    refCount = State::Instance().currentWrappedSwapchain->Release();
}
```

This is incompatible with a multi-owner DXGI environment.

The presence of `currentWrappedSwapchain` in global state does not prove that global state owns the currently outstanding COM references.

### 5.3 Real swapchain release-until-zero still exists

`release/0.9` `XeFG_Dx12::CreateSwapchain()` / `CreateSwapchain1()` still contain a recreation path equivalent to:

```cpp
if (State::Instance().currentRealSwapchain != nullptr)
{
    UINT release = 0;
    do
    {
        release = State::Instance().currentRealSwapchain->Release();
    } while (release > 0);
}
```

This is the same ownership class of bug.

`currentRealSwapchain` is a tracked pointer. It is not evidence that OptiScaler owns every reference represented by the object's current refcount.

### 5.4 Wrapper final release still attempts to drive `_real` to zero

The older wrapper code contains a loop equivalent to:

```cpp
while (refCount > 0)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    refCount = _real->Release();
}
```

The wrapper should release the reference it owns, not wait for and actively consume references owned by other participants.

### 5.5 XeFG Destroy failure is not propagated correctly

The `release/0.9` `DestroySwapchainContext()` behavior is effectively:

```cpp
auto context = _swapChainContext;
_swapChainContext = nullptr;

auto result = XeFGProxy::Destroy()(context);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _swapChainContext = context;
}

return true;
```

This restores the context on failure but still reports success to the caller.

That creates a dangerous higher-level state transition:

```text
XeFG Destroy did not complete
    -> local context handle restored
    -> caller still believes teardown completed
    -> surrounding state may be cleared
    -> replacement path may proceed
    -> second context/proxy may be created over a still-live old lifecycle
```

This must be corrected independently of rendering correctness.

---

## 6. Ownership Model Required on `release/0.9`

The target model should match the final master ownership invariant without requiring the newer master architecture.

```text
Game
    owns references it acquired

Intel XeFG runtime/proxy
    owns references required by the XeFG implementation

REFramework
    owns and releases its own renderer/backbuffer/keepalive references

OptiScaler wrapper
    owns only references it explicitly acquired for the wrapper

OptiScaler menu renderer
    owns and releases its own render-target references

State::current*Swapchain
    tracking aliases only
    no implicit AddRef
    no Release authority
```

Recommended documentation in `State.h`:

```cpp
// Swapchain tracking aliases.
// Storing a pointer here does not acquire a COM reference and does not grant
// permission to Release() it. Lifetime and cleanup authority remain with the
// concrete owner that acquired or accepted the corresponding reference.
IDXGISwapChain* currentSwapchain = nullptr;
IDXGISwapChain* currentWrappedSwapchain = nullptr;
IDXGISwapChain* currentRealSwapchain = nullptr;
IDXGISwapChain* currentFGSwapchain = nullptr;
```

Do not convert these fields to owning `ComPtr`s merely to make lifetime management simpler. In particular, adding an unconditional strong reference to the XeFG public proxy can itself prevent XeFG Destroy from succeeding.

---

## 7. Recommended Backport Structure

Use a dedicated branch from the reviewed `release/0.9` baseline, for example:

```text
release/0.9-ref-compat
```

Do not cherry-pick the entire master compatibility history.

Implement three reviewable PRs in order.

---

# P1 — Backbuffer Ownership Hygiene

## 8. Goal

Backport the final intent of master PR #2 only.

### Primary files

```text
OptiScaler/hooks/FG_Hooks.cpp
```

Possibly related menu-render-target code only if required by the existing `release/0.9` interfaces.

### Required changes

1. Remove `XEFG_RESOURCE_REF_LIMIT`-based ownership behavior.
2. Remove `oldBackBuffers` behavior if it exists only to support force-draining.
3. Remove every targeted backbuffer loop that repeatedly calls `Release()` until a threshold is reached.
4. Preserve normal one-for-one COM ownership:
   - if this function called `GetBuffer()` and received one reference, release that one acquired reference exactly once;
   - do not infer external ownership from the returned refcount.
5. Before `ResizeBuffers` / `ResizeBuffers1`, release only OptiScaler-owned render-target references, including the menu renderer's references as appropriate for the `release/0.9` implementation.

### Must not change

- XeLL setup.
- fakenvapi.
- FG input frame accounting.
- FSRFG/DLSSG texture handling.
- swapchain status-result policy.
- REFramework code.

### P1 acceptance condition

A static search must show no targeted backbuffer pattern equivalent to:

```cpp
while (refCount > XEFG_RESOURCE_REF_LIMIT)
```

or another repeated backbuffer `Release()` loop intended to force the count downward.

---

# P2 — Owner-Scoped Swapchain Cleanup

## 9. Goal

Backport the **final combined state of master PR #6 plus PR #13**, not their intermediate history.

### Primary files

```text
OptiScaler/State.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
```

Reference-only unless required:

```text
OptiScaler/with_dx12/dx11_with_dx12_sc.cpp
OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp
```

### Required changes

#### 9.1 State aliases

Document the four global swapchain pointers as tracking aliases.

Do not add ownership merely because they are globally visible.

#### 9.2 Remove `currentRealSwapchain` force-drain

Delete `Release()`-until-zero loops from both XeFG swapchain creation variants.

After a successfully completed previous lifecycle transaction, clear only aliases that this path is authorized to clear and only when identity matches the object being retired.

#### 9.3 Remove `currentWrappedSwapchain` force-drain

`hkFGRelease()` must not repeatedly release the wrapper through a State alias.

#### 9.4 Wrapper owns wrapper cleanup

When `WrappedIDXGISwapChain4` reaches its actual final wrapper release:

- clear wrapper-related aliases by identity where appropriate;
- release the wrapper-owned `_real` reference exactly once;
- do not wait for `_real` to reach zero;
- do not repeatedly call `_real->Release()` to consume references held elsewhere.

Conceptual target:

```cpp
if (state.currentWrappedSwapchain == this)
    state.currentWrappedSwapchain = nullptr;

if (state.currentRealSwapchain == _real)
    state.currentRealSwapchain = nullptr;

_real->Release(); // exactly the reference owned by this wrapper

delete this;
```

The exact code must respect the existing `release/0.9` wrapper refcount structure.

#### 9.5 Keep owner domains separate

This is the PR #13 correction that must be present from the beginning:

```text
XeFG public-proxy teardown
    must not opportunistically clear/release wrapper/real ownership

Wrapper teardown
    owns wrapper/_real cleanup

State alias cleanup
    only by identity and only inside the relevant owner domain
```

### P2 static acceptance conditions

No relevant matches should remain for patterns equivalent to:

```text
currentRealSwapchain->Release() inside a drain loop
currentWrappedSwapchain->Release() inside a drain loop
while (refCount > 0) around swapchain COM cleanup
Release-until-0 ownership inference
```

A returned COM refcount may be logged for diagnostics, but it must not be used as authority to release additional references that this code did not acquire.

---

# P3 — Fail-Closed XeFG Lifecycle

## 10. Goal

Adapt the lifecycle invariant from master PR #1 to `release/0.9` while preserving `release/0.9` architecture and exact-success semantics.

### Primary files

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
```

Caller audit may require:

```text
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
OptiScaler/with_dx12/dx11_with_dx12_sc.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
```

Only modify callers that can incorrectly clear or recreate after a failed XeFG teardown.

## 10.1 Destroy must return meaningful success/failure

Target semantic:

```cpp
bool XeFG_Dx12::DestroySwapchainContext()
{
    if (_swapChainContext == nullptr || State::Instance().isShuttingDown)
        return true;

    auto context = _swapChainContext;
    _swapChainContext = nullptr;

    auto result = XeFGProxy::Destroy()(context);

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        _swapChainContext = context;
        return false;
    }

    State::Instance().currentFGSwapchain = nullptr;
    return true;
}
```

The exact implementation must preserve the `release/0.9` XeLL lifetime rule: the XeLL context must remain valid through XeFG Destroy and should only be destroyed after successful XeFG teardown where the existing branch already expects that behavior.

## 10.2 `ReleaseSwapchain()` must propagate failure

If vendor Destroy does not complete successfully:

```text
old XeFG context identity retained
current FG proxy identity retained as needed
normal successful-release tail not executed
caller receives failure
```

Do not replace this with:

- retry loops;
- timeouts followed by pretending success;
- asynchronous destroy;
- forced leak as the default success path;
- clearing the context handle to recover superficially.

## 10.3 Recreation must abort after failed release

Both:

```text
CreateSwapchain(...)
CreateSwapchain1(...)
```

must refuse to create a replacement if the previous XeFG lifecycle did not finish.

Target invariant:

```text
Destroy FAILED / non-success
    -> exact old context retained
    -> release transaction reports failure
    -> recreation aborts
    -> no second XeFG context is constructed over the first
```

## 10.4 Partial-initialization cleanup

If a XeFG context has been created but later initialization fails, for example at:

```text
GetProperties
D3D12InitFromSwapChainDesc
D3D12GetSwapChainPtr
```

the error path should not silently leave a half-initialized context that a later create path mistakes for a valid lifecycle.

A local helper similar in intent to the master `AbortSwapchainInitialization(stage)` is recommended, adapted to `release/0.9`.

Important:

- cleanup must still obey the same fail-closed Destroy rule;
- if cleanup Destroy itself fails, retain the context identity and block replacement rather than pretending cleanup succeeded.

## 10.5 Lifecycle serialization

Port only the synchronization needed to prevent overlapping create/release transactions for the same XeFG object.

Do not automatically port PR #18's broader Present mutex thread-ownership changes as part of P3.

The lifecycle guard should answer only:

```text
Is another XeFG create/release transaction already mutating this context/proxy lifecycle?
```

and refuse an unsafe overlapping transition.

---

## 11. XeFG Result Semantics for This Backport

This section is a hard guardrail because the master history contains an intermediate policy that should not be reproduced.

### Keep exact success for lifecycle/state/output commits

Use:

```cpp
result == XEFG_SWAPCHAIN_RESULT_SUCCESS
```

for operations where code is about to commit lifecycle or state transitions, including at least:

```text
D3D12CreateContext
SetLatencyReduction
Destroy
GetProperties output commit
D3D12InitFromSwapChainDesc
D3D12GetSwapChainPtr
SetEnabled state transition
resource-tagging paths whose existing recovery behavior depends on exact success
```

### Do not use blanket non-negative success

Do not broadly replace the above with:

```cpp
static_cast<int32_t>(result) >= 0
```

The later master PR #14 intentionally narrowed the earlier `d87d8ca` behavior.

Warnings may be logged distinctly from negative errors if desired, but a positive warning/status must not automatically become proof that a lifecycle transition completed and its outputs are valid.

---

## 12. Changes Explicitly Not to Backport

### PR #17 — thread-local FG hook reentrancy guards

- draft/unmerged;
- targeted a logging-sensitive MHW race hypothesis;
- not established as the root-cause fix;
- should remain an independent experiment.

### PR #18 — thread-aware FG Present mutex ownership

- draft/unmerged;
- identifies a valid owner-tag/thread-identity issue;
- runtime crash remained after the patch;
- not required to enforce the ownership/lifecycle model in this document.

### PR #19 — automatic XeFG crash trace

- diagnostic-only;
- stacked on PR #18;
- large instrumentation surface;
- should not be imported into a clean `release/0.9` compatibility baseline.

If later diagnostics are needed on `release/0.9`, create a branch-local minimal diagnostic rather than dragging PR #19's master-specific tracing architecture into the baseline.

### Reflex work

PR #3 and PR #8-#12 address RE9/PRAGMATA Reflex availability/spoofing gates and are unrelated to swapchain ownership.

### XeLL/fakenvapi integration

Do not change the `release/0.9` pattern merely to match master.

For this backport, preserve the existing `release/0.9` model approximately represented by:

```text
XeLLProxy::CreateContext
    -> XeLL SetSleepMode
    -> fakenvapi::setModeAndContext
    -> XeFG SetLatencyReduction
```

The master-vs-0.9 XeLL/fakenvapi ownership difference is a separate investigation.

---

## 13. Recommended PR Dependency Graph

```text
release/0.9 @ 132bc110
        |
        v
P1 Backbuffer ownership hygiene
        |
        v
P2 Owner-scoped swapchain cleanup
        |
        v
P3 Fail-closed XeFG lifecycle
        |
        v
REF compatibility validation baseline
```

Keep each PR buildable and reviewable independently.

Do not squash all three conceptual stages into a single large first implementation because a runtime regression would become difficult to attribute.

---

## 14. Validation Strategy

The validation goal is lifecycle/coexistence correctness, not solving every `release/0.9` rendering defect.

### 14.1 Build/static validation for every PR

Required:

```text
git diff --check
Release | x64 build
clang-format checks for touched C/C++ files
```

Static searches should confirm the intended ownership drains are gone.

### 14.2 Runtime matrix

At minimum run the following configurations after each stage when practical:

| Test | OptiScaler | REF | XeFG | Purpose |
|---|---|---|---|---|
| A | stock `release/0.9` | fork REF | On | baseline |
| B | P1 | fork REF | On | isolate backbuffer ownership effect |
| C | P1+P2 | fork REF | On | isolate full swapchain ownership effect |
| D | P1+P2+P3 | fork REF | On | final lifecycle baseline |
| E | P1+P2+P3 | absent | On | OptiScaler-only regression smoke test |

Recommended RE Engine coverage:

```text
Monster Hunter Wilds
Dragon's Dogma 2
```

Additional known working games can be used as controls.

### 14.3 Lifecycle actions

For each applicable runtime test exercise:

1. cold launch;
2. reach stable gameplay/menu presentation;
3. Alt+Tab out and back;
4. fullscreen/window/borderless transition if available;
5. resolution change / ResizeBuffers path if available;
6. overlay open/close;
7. return to title/menu if it recreates presentation objects;
8. normal game exit.

### 14.4 What counts as success

For this backport, success means:

- no force-drain of external COM references;
- no REF-visible swapchain object is destroyed simply because OptiScaler sees a refcount above a threshold;
- no wrapper/real/proxy owner crosses ownership boundaries during cleanup;
- failed XeFG Destroy does not get converted into a successful teardown;
- no replacement XeFG context is created on top of a failed old Destroy lifecycle;
- REF and OptiScaler survive normal resize/recreation transitions at least as well as the baseline, preferably better;
- OptiScaler-only behavior does not regress due to the ownership cleanup.

### 14.5 What is not a failure of this backport by itself

Known/independent issues must be logged separately instead of causing this backport to absorb unrelated fixes:

- FSRFG-to-XeFG corrupted 3D scene output on `release/0.9`;
- master startup/Destroy instability around the newer XeLL/fakenvapi integration;
- Subnautica 2 resource-geometry mismatch;
- DLSSG frame-ID/slot-generation issues;
- Reflex availability quirks.

These can make a specific rendering test unusable as a visual-quality control, but they do not invalidate the static ownership objective of P1/P2/P3.

---

## 15. Suggested Diagnostic Logging for the Clean Backport

Keep diagnostics low-volume and lifecycle-oriented.

Useful events:

```text
[XeFG][Lifecycle] action=destroy_begin
[XeFG][Lifecycle] action=destroy_return result=...
[XeFG][Lifecycle] action=destroy_failed retained=true
[XeFG][Lifecycle] action=release_swapchain_aborted
[XeFG][Lifecycle] action=recreate_aborted reason=previous_destroy_failed
[XeFG][Lifecycle] action=init_aborted stage=...
[XeFG][Lifecycle] action=init_cleanup_failed
```

Do not add per-frame Present/resource tracing to the production backport.

The purpose is to answer from a normal log:

```text
Did XeFG Destroy return?
Did it succeed exactly?
Was the old context retained on failure?
Did ReleaseSwapchain stop?
Was a replacement context prevented?
```

---

## 16. Review Checklist

### P1 review

- [ ] No backbuffer release-until-threshold loop remains in XeFG Resize/Resize1/final release paths.
- [ ] OptiScaler-owned render targets are released before resize.
- [ ] No XeLL/fakenvapi behavior changed.
- [ ] No external COM ownership inferred from returned refcount.

### P2 review

- [ ] State swapchain fields are documented as non-owning tracking aliases.
- [ ] `currentRealSwapchain` is not release-drained.
- [ ] `currentWrappedSwapchain` is not release-drained.
- [ ] wrapper releases only its own `_real` reference.
- [ ] XeFG teardown does not clear/release wrapper/real ownership as a side effect.
- [ ] alias clear operations are identity-aware.
- [ ] no new unconditional `AddRef`/`ComPtr` ownership was added to `currentFGSwapchain`.

### P3 review

- [ ] `DestroySwapchainContext()` returns failure when exact XeFG Destroy success is not obtained.
- [ ] old context identity is retained on failed Destroy.
- [ ] `ReleaseSwapchain()` propagates failure.
- [ ] `CreateSwapchain()` aborts replacement after failed release.
- [ ] `CreateSwapchain1()` aborts replacement after failed release.
- [ ] partial initialization is cleaned up fail-closed.
- [ ] XeLL lifetime remains valid through XeFG Destroy.
- [ ] lifecycle overlap is guarded without importing unrelated Present-mutex experiments.
- [ ] exact-success XeFG status semantics are preserved.

---

## 17. Recommended Implementation Policy

The safest implementation policy is:

> Backport invariants, not commits.

Specifically:

```text
PR #2
    -> reproduce final backbuffer ownership rule

PR #6 + PR #13
    -> reproduce only the final owner-scoped swapchain model

PR #1
    -> reproduce fail-closed lifecycle behavior in release/0.9 structure

PR #14
    -> use as a semantic guardrail, not as a mandatory code port
```

Do not mechanically cherry-pick the full PR histories because:

- master and `release/0.9` have diverged heavily;
- master changed low-latency/fakenvapi ownership architecture;
- PR #1 history included intermediate status semantics later refined by PR #14;
- PR #6 was itself ownership-boundary-corrected by PR #13;
- a raw cherry-pick would increase the risk of importing unrelated master behavior or intermediate states.

---

## 18. Final Recommendation

Create a dedicated `release/0.9` compatibility line and implement only these three stages:

```text
P1: PR #2 final intent
    Backbuffer COM ownership hygiene

P2: PR #6 + PR #13 final intent
    Owner-scoped swapchain cleanup

P3: PR #1 final lifecycle intent
    Fail-closed XeFG Destroy/recreation
```

Everything else should remain outside this baseline until runtime evidence specifically requires it.

This gives the project a clean A/B platform for answering an important architectural question:

> How much of the REFramework + XeFG instability can be removed on the older `release/0.9` architecture solely by enforcing correct COM ownership and XeFG lifecycle rules?

If P1/P2/P3 improve REF coexistence while keeping the older XeLL/fakenvapi architecture untouched, that is strong evidence that the ownership/lifecycle work is branch-independent and should remain part of the long-term OptiScaler compatibility model.

If problems remain afterward, the next investigation can proceed from a much cleaner baseline without conflating COM ownership violations with XeLL integration, texture/resource geometry, frame-ID handling, or diagnostic experiments.
