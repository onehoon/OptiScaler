# Work Order: `release/0.9` REFramework Compatibility P1 — Backbuffer COM Ownership Hygiene

**Repository:** `onehoon/OptiScaler`  
**Work-order branch:** `master`  
**Fork `master` reviewed:** `1ad1623c83e51973b6c8ce867f61852589e9ad24`  
**Fork code baseline immediately below the documentation commit:** `7ee16d9b8b5405451322a840e1adbf1efe24dcf1`  
**Upstream repository:** `optiscaler/OptiScaler`  
**Upstream target branch:** `release/0.9`  
**Upstream `release/0.9` HEAD reviewed:** `132bc110f371d273834681ae05c73db4212bd337`  
**Long-lived fork branch to create:** `reframework-0.9`  
**P1 implementation branch:** `feature/reframework-0.9-p1-backbuffer-ownership`  
**Date:** 2026-09-13

Related planning document:

```text
doc/RELEASE_0_9_REF_COMPATIBILITY_BACKPORT_ANALYSIS_AND_PLAN_2026-09-13.md
```

Relevant fork history used only as implementation reference:

- PR #2 — `Fix XeFG backbuffer COM ownership during resize and release`
- Current fork `master` final behavior in `OptiScaler/hooks/FG_Hooks.cpp`

---

## 1. Objective

Create a clean `reframework-0.9` compatibility line from the **latest upstream `release/0.9`**, then implement P1 as a separate reviewable PR.

P1 has exactly one technical objective:

> Remove XeFG backbuffer COM reference draining that repeatedly calls `Release()` based on the returned refcount, while preserving only references actually owned by OptiScaler.

The branch topology must be:

```text
optiscaler/OptiScaler release/0.9
        |
        | exact upstream baseline
        v
onehoon/OptiScaler reframework-0.9
        |
        | P1 PR
        v
feature/reframework-0.9-p1-backbuffer-ownership
```

Do **not** start this work from fork `master`.

Do **not** cherry-pick PR #2 directly.

Do **not** merge current fork `master` into `reframework-0.9`.

The correct policy is:

> Backport the final ownership invariant into the current `release/0.9` architecture.

---

## 2. Why a clean upstream-based branch is required

Current fork `master` contains many changes beyond P1, including later swapchain-owner cleanup, fail-closed XeFG lifecycle work, Reflex investigations, low-latency/fakenvapi changes, and diagnostics.

Those changes must not contaminate the `release/0.9` compatibility baseline.

At the time of this work order, upstream `release/0.9` resolves to:

```text
132bc110f371d273834681ae05c73db4212bd337
```

Commit message:

```text
Remove forced 4x limit at Config.cpp and use value reported from libxess_fg.dll
```

The existing fork `release/0.9` currently resolves to the same commit, but **branch provenance for this compatibility line must still be the fetched upstream branch**, not an assumption that the fork branch remains synchronized.

---

## 3. Mandatory branch creation procedure

### 3.1 Verify remotes

From the local clone of `onehoon/OptiScaler`:

```bash
git remote -v
```

Expected conceptual mapping:

```text
origin    -> https://github.com/onehoon/OptiScaler.git
upstream  -> https://github.com/optiscaler/OptiScaler.git
```

If `upstream` does not exist:

```bash
git remote add upstream https://github.com/optiscaler/OptiScaler.git
```

### 3.2 Fetch the actual latest upstream `release/0.9`

```bash
git fetch --prune upstream release/0.9
git rev-parse upstream/release/0.9
```

For the reviewed baseline, the result must be:

```text
132bc110f371d273834681ae05c73db4212bd337
```

### 3.3 Freshness guard

If upstream `release/0.9` has advanced beyond `132bc110...` when this work is executed:

1. **Do not silently pin to the old SHA.**
2. Inspect the new upstream commits first.
3. Specifically check whether they changed:
   - `OptiScaler/hooks/FG_Hooks.cpp`;
   - XeFG resize handling;
   - backbuffer `GetBuffer()` / `Release()` behavior;
   - `hkFGRelease()`.
4. Rebase the P1 implementation on the new upstream HEAD only after confirming the ownership problem still exists and updating the patch accordingly.

The user requested the latest upstream `release/0.9`, so the upstream branch is authoritative.

### 3.4 Create the long-lived compatibility branch

First make sure a remote `reframework-0.9` branch does not already contain independent work.

```bash
git fetch origin --prune
```

If `origin/reframework-0.9` does not exist:

```bash
git switch --detach upstream/release/0.9
git switch -c reframework-0.9
git push -u origin reframework-0.9
```

Immediately verify that the new branch is an exact upstream baseline:

```bash
git rev-parse reframework-0.9
git rev-parse upstream/release/0.9
git diff --stat upstream/release/0.9..reframework-0.9
```

Before P1 begins, the two SHAs must match and the diff must be empty.

### 3.5 Create the P1 feature branch

Do not implement P1 directly on the long-lived branch.

```bash
git switch reframework-0.9
git switch -c feature/reframework-0.9-p1-backbuffer-ownership
```

The eventual PR must target:

```text
base: reframework-0.9
head: feature/reframework-0.9-p1-backbuffer-ownership
```

Suggested PR title:

```text
P1: remove XeFG backbuffer COM force-drain on release/0.9
```

---

## 4. Code comparison summary: upstream `release/0.9` vs fork `master`

The code was compared directly, not only by reading historical PR descriptions.

### 4.1 File-level conclusion

P1 should normally modify only:

```text
OptiScaler/hooks/FG_Hooks.cpp
```

Do not broaden the change unless compilation proves that a directly related declaration exists elsewhere.

### 4.2 Top-level ownership heuristic

Upstream `release/0.9` still contains:

```cpp
#define XEFG_RESOURCE_REF_LIMIT 1
```

and:

```cpp
#if (XEFG_RESOURCE_REF_LIMIT == 0)
inline static std::vector<void*> oldBackBuffers;
#endif
```

Current fork `master` contains neither construct.

These are historical ownership heuristics and must be removed in P1.

### 4.3 `hkResizeBuffers()`

Current upstream `release/0.9` already performs the correct OptiScaler-owned menu render-target cleanup:

```cpp
if (Config::Instance()->OverlayMenu.value_or_default())
    MenuOverlayDx::CleanupRenderTarget(false, NULL);
```

But immediately afterward it still probes up to eight swapchain backbuffers and repeatedly calls `Release()` until the returned count reaches `XEFG_RESOURCE_REF_LIMIT`.

The upstream block is conceptually:

```cpp
if (State::Instance().activeFgOutput == FGOutput::XeFG)
{
    for (UINT i = 0; i < 8; i++)
    {
        ID3D12Resource* backBuffer = nullptr;
        auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

        if (bbResult == S_OK)
        {
            auto refCount = backBuffer->Release();
            while (refCount > XEFG_RESOURCE_REF_LIMIT)
            {
                refCount = backBuffer->Release();
            }
        }
        else
        {
            break;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

Current fork `master` no longer performs this backbuffer probing/draining.

Important difference from the old PR #2 patch: **the latest upstream `release/0.9` already has menu render-target cleanup before the force-drain block.** Do not add a second cleanup call.

Also, upstream `release/0.9` still has a `100 ms` sleep tied to this workaround. Current fork `master` no longer has that sleep in the corresponding ownership-clean path.

Therefore P1 must remove the whole force-drain workaround, including its attached sleep.

### 4.4 `hkResizeBuffers1()`

The same ownership violation remains in the `ResizeBuffers1` path:

```text
menu RT cleanup
    -> XeFG GetBuffer loop
    -> first Release()
    -> repeated Release() until threshold
    -> 100 ms sleep
    -> actual ResizeBuffers1
```

Current fork `master` keeps the menu cleanup but removes the XeFG backbuffer drain.

P1 must do the same using the `release/0.9` function structure, not by copying the entire current master function.

### 4.5 `hkFGRelease()`

Upstream `release/0.9` still contains two P1-specific backbuffer mechanisms that current fork `master` removed:

1. `oldBackBuffers` release suppression;
2. a final FG-swapchain backbuffer enumeration that drains each backbuffer based on returned refcount.

The upstream final-release block is conceptually:

```cpp
DXGI_SWAP_CHAIN_DESC scDesc {};
((IDXGISwapChain*) This)->GetDesc(&scDesc);

for (UINT i = 0; i < scDesc.BufferCount; i++)
{
    ID3D12Resource* backBuffer = nullptr;
    auto bbResult = ((IDXGISwapChain*) This)->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

    if (bbResult == S_OK)
    {
        auto refCount = backBuffer->Release();
        while (refCount > XEFG_RESOURCE_REF_LIMIT)
            refCount = backBuffer->Release();
    }
    else
    {
        break;
    }
}
```

That entire ownership-probing block is P1 scope and must be removed.

### 4.6 Important adjacent code that is **not P1**

Upstream `hkFGRelease()` also contains a later block that repeatedly releases:

```cpp
State::Instance().currentWrappedSwapchain
```

until its refcount falls to zero.

That is also an ownership bug, but it belongs to **P2 Owner-Scoped Swapchain Cleanup**.

Do **not** modify it in P1.

This is intentional. P1 must remain attributable specifically to backbuffer ownership cleanup.

---

## 5. Core COM ownership rule

The implementation must follow this rule:

> The integer returned by `IUnknown::Release()` is not an ownership map.

For a swapchain backbuffer:

```text
Game           may own references
Intel XeFG     may own references
REFramework    may own references
DXGI/D3D       may own references
OptiScaler     may own references
```

If OptiScaler calls:

```cpp
GetBuffer(..., &backBuffer)
```

then that successful call gives OptiScaler one COM reference.

OptiScaler may later release **that one reference exactly once**:

```cpp
backBuffer->Release();
```

It may not continue releasing merely because the returned count is greater than `1`, `0`, or any other threshold.

Therefore this pattern is forbidden:

```cpp
auto refCount = backBuffer->Release();
while (refCount > someThreshold)
    refCount = backBuffer->Release();
```

The additional calls can consume references owned by REFramework, XeFG, the game, or another observer.

---

## 6. Task A — Remove the obsolete backbuffer ownership heuristic

In:

```text
OptiScaler/hooks/FG_Hooks.cpp
```

remove:

```cpp
#define XEFG_RESOURCE_REF_LIMIT 1
```

and remove:

```cpp
#if (XEFG_RESOURCE_REF_LIMIT == 0)
inline static std::vector<void*> oldBackBuffers;
#endif
```

Do not replace them with another threshold.

Do not introduce a different refcount constant.

Do not add an `AddRef()`/`Release()` probe merely to inspect the count.

---

## 7. Task B — Clean `hkResizeBuffers()`

### 7.1 Preserve the branch-native flow

Keep all existing `release/0.9` logic that is unrelated to the force-drain, including as applicable:

- resize recursion/internal-call guards;
- FG pause/deactivation;
- GPU-idle synchronization;
- swapchain flag handling;
- skip-resize logic;
- `FGResizing` state;
- tearing state;
- HDR handling;
- borderless workaround;
- actual `ResizeBuffers()` call;
- post-resize state restoration.

### 7.2 Keep OptiScaler-owned menu cleanup

This must remain exactly once before the real resize:

```cpp
if (Config::Instance()->OverlayMenu.value_or_default())
    MenuOverlayDx::CleanupRenderTarget(false, NULL);
```

This is legitimate owner-scoped cleanup: the menu renderer is releasing render-target references it owns.

### 7.3 Remove the entire XeFG backbuffer drain

Delete the block that:

- iterates `i < 8`;
- calls `This->GetBuffer()` for ownership probing;
- calls the first `backBuffer->Release()`;
- loops while the returned count exceeds `XEFG_RESOURCE_REF_LIMIT`;
- stores pointers in `oldBackBuffers`;
- sleeps for 100 ms after draining.

### 7.4 Preferred resulting shape

Do **not** copy unrelated current-master code. The local `release/0.9` function should simply transition from owner-scoped cleanup to the existing next stage:

```cpp
// Release menu render targets owned by OptiScaler.
if (Config::Instance()->OverlayMenu.value_or_default())
    MenuOverlayDx::CleanupRenderTarget(false, NULL);

// Do not enumerate/drain swapchain backbuffers here.
// Their remaining COM references may belong to the game, XeFG,
// REFramework, DXGI, or another participant.

// Continue with the existing release/0.9 HDR/resize flow.
```

A comment is optional. If added, use ownership language like the above.

Do **not** copy the current-master comment:

```text
Backbuffer release probing is disabled for the MHW resize investigation.
```

That comment describes a historical investigation. `reframework-0.9` is intended to establish a durable ownership rule, not carry experiment-specific wording.

---

## 8. Task C — Clean `hkResizeBuffers1()`

Apply the same rule independently to `hkResizeBuffers1()`.

### Before

Conceptually:

```cpp
if (Config::Instance()->OverlayMenu.value_or_default())
    MenuOverlayDx::CleanupRenderTarget(false, NULL);

if (State::Instance().activeFgOutput == FGOutput::XeFG)
{
    for (UINT i = 0; i < 8; i++)
    {
        ID3D12Resource* backBuffer = nullptr;
        auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

        if (bbResult == S_OK)
        {
            auto refCount = backBuffer->Release();
            while (refCount > XEFG_RESOURCE_REF_LIMIT)
                refCount = backBuffer->Release();
        }
        else
        {
            break;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

### After

```cpp
if (Config::Instance()->OverlayMenu.value_or_default())
    MenuOverlayDx::CleanupRenderTarget(false, NULL);

// No backbuffer refcount drain.

// Continue directly into the existing ResizeBuffers1 path.
```

Again, preserve all unrelated `release/0.9` behavior.

---

## 9. Task D — Clean only the backbuffer portion of `hkFGRelease()`

This function is especially important because P1 and P2 concerns are adjacent in the same function.

### 9.1 Remove `oldBackBuffers` suppression

Delete the conditional block equivalent to:

```cpp
#if (XEFG_RESOURCE_REF_LIMIT == 0)
if (oldBackBuffers.size() > 0)
{
    for (auto it = oldBackBuffers.begin(); it != oldBackBuffers.end(); ++it)
    {
        if (*it == This)
            return 0;
    }
}
#endif
```

Once P1 no longer force-releases backbuffers, there must be no synthetic list of "already force-released" backbuffers that suppresses future legitimate release behavior.

### 9.2 Remove final-swapchain backbuffer draining

Inside the path where:

```cpp
o_FGRelease(This) == 1
```

keep:

```cpp
WaitForGPUIdle();
```

if that is part of the existing branch flow.

Then remove:

```cpp
DXGI_SWAP_CHAIN_DESC scDesc {};
((IDXGISwapChain*) This)->GetDesc(&scDesc);
```

**if those variables are only used by the removed backbuffer drain**, and remove the entire `GetBuffer()` / repeated-`Release()` loop.

The conceptual P1 result should be:

```cpp
if (o_FGRelease(This) == 1)
{
    LOG_DEBUG("");

    WaitForGPUIdle();

    // No swapchain-backbuffer enumeration or refcount draining here.

    // Existing release/0.9 lifecycle flow continues here.
    skipReleaseChecks = true;

    if (State::Instance().currentFG != nullptr)
    {
        LOG_DEBUG("FG Swapchain released, release FG & swapchain context");
        State::Instance().currentFG->ReleaseSwapchain(_hwnd);
    }

    // Continue with the existing release/0.9 behavior.
}
```

### 9.3 Do not accidentally implement P2 here

After the P1 deletion, upstream `release/0.9` will still contain code broadly equivalent to:

```cpp
if (State::Instance().currentWrappedSwapchain != nullptr &&
    State::Instance().currentSwapchainDesc.OutputWindow == _hwnd)
{
    auto refCount = State::Instance().currentWrappedSwapchain->Release();

    while (refCount > 0 && refCount < 0xffffff00)
        refCount = State::Instance().currentWrappedSwapchain->Release();

    State::Instance().currentWrappedSwapchain = nullptr;
}
```

**Leave this unchanged in P1.**

It is explicitly scheduled for P2.

Keeping it for one PR is not an endorsement of the code; it is required for clean fault attribution between P1 and P2.

---

## 10. Legitimate one-for-one backbuffer releases that must remain

Do not search-and-delete every occurrence of:

```cpp
backBuffer->Release();
```

There are legitimate paths where OptiScaler obtains one reference with `GetBuffer()` for its own temporary operation and releases that exact reference once.

For example, the existing buffer-state transition logic has the ownership shape:

```cpp
ID3D12Resource* backBuffer = nullptr;
if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
{
    // Use the temporary reference.
    ...

    // Release exactly the reference acquired by GetBuffer().
    backBuffer->Release();
}
```

This is correct and must remain.

The defect is **repeated release based on the returned refcount**, not `Release()` itself.

---

## 11. Do not copy unrelated current-master behavior

Current fork `master` is useful as evidence for the final P1 ownership rule, but it is **not** a drop-in source file for this branch.

Do not copy current-master changes involving:

- `thread_local` release/reentrancy behavior;
- XeFG lifecycle ownership helpers;
- `ReleaseSwapchainFromFinalProxyRelease()`;
- fail-closed Destroy/recreate handling;
- `currentWrappedSwapchain` / `currentRealSwapchain` owner-domain cleanup;
- State alias ownership comments;
- Reflex logic;
- fakenvapi/XeLL redesign;
- DLSSG additions;
- new diagnostics;
- master-only rename/refactor changes such as `fgChanged` or other state naming;
- unrelated swapchain flag behavior.

Those belong to other stages or other architecture lines.

P1 should be a small, mechanically understandable ownership correction against upstream `release/0.9`.

---

## 12. Expected diff profile

Normal expected production-code diff:

```text
OptiScaler/hooks/FG_Hooks.cpp
```

Expected categories:

```text
REMOVE  XEFG_RESOURCE_REF_LIMIT
REMOVE  oldBackBuffers
REMOVE  hkResizeBuffers XeFG GetBuffer/refcount-drain block
REMOVE  hkResizeBuffers drain-related 100 ms sleep
REMOVE  hkResizeBuffers1 XeFG GetBuffer/refcount-drain block
REMOVE  hkResizeBuffers1 drain-related 100 ms sleep
REMOVE  hkFGRelease oldBackBuffers suppression
REMOVE  hkFGRelease final swapchain-backbuffer drain
KEEP    menu render-target cleanup
KEEP    legitimate temporary GetBuffer -> one Release paths
KEEP    currentWrappedSwapchain drain for P2
```

If the implementation starts modifying multiple unrelated files, stop and re-evaluate scope.

---

## 13. Static validation

### 13.1 Basic diff checks

```bash
git diff --check reframework-0.9...HEAD
git diff --stat reframework-0.9...HEAD
git diff reframework-0.9...HEAD -- OptiScaler/hooks/FG_Hooks.cpp
```

Review the full diff manually.

### 13.2 Required searches

The P1 implementation should leave no relevant occurrence of:

```bash
git grep -n "XEFG_RESOURCE_REF_LIMIT" -- OptiScaler/hooks/FG_Hooks.cpp
git grep -n "oldBackBuffers" -- OptiScaler/hooks/FG_Hooks.cpp
git grep -n "Releasing backbuffer" -- OptiScaler/hooks/FG_Hooks.cpp
```

Expected result for the targeted P1 constructs: no matches.

Also inspect all remaining backbuffer release calls:

```bash
git grep -n "backBuffer->Release" -- OptiScaler/hooks/FG_Hooks.cpp
```

Every remaining match must be explainable as a **single release paired with a reference acquired by that local path**.

Do not require this search to return zero.

### 13.3 Check drain loops explicitly

Search for refcount-driven release loops:

```bash
git grep -n "while (refCount" -- OptiScaler/hooks/FG_Hooks.cpp
```

A remaining `currentWrappedSwapchain` loop is expected until P2.

There must be no remaining loop where the repeatedly released object is a swapchain backbuffer obtained via `GetBuffer()`.

### 13.4 Check the workaround sleeps

Search:

```bash
git grep -n "milliseconds(100)" -- OptiScaler/hooks/FG_Hooks.cpp
```

Do not globally delete unrelated sleeps. Confirm specifically that the two sleeps attached to the removed resize backbuffer-drain workarounds are gone.

---

## 14. Formatting and build validation

Run:

```bash
git diff --check
```

Format only touched C/C++ files according to repository `.clang-format`.

Suggested dry-run validation where supported:

```bash
clang-format --dry-run --Werror OptiScaler/hooks/FG_Hooks.cpp
```

Build the x64 Release configuration using the repository's normal Visual Studio/MSBuild environment.

Typical command from a configured VS Developer Command Prompt:

```bat
msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64
```

If dependencies/submodules are not initialized:

```bash
git submodule update --init --recursive
```

Do not modify build/CI files merely to make P1 pass locally unless a pre-existing branch issue is independently demonstrated.

---

## 15. Runtime validation

P1 is intended to isolate backbuffer ownership effects.

Preferred initial matrix:

| Test | OptiScaler branch | REFramework | XeFG | Purpose |
|---|---|---|---|---|
| A | pristine `reframework-0.9` | fork REF | On | upstream 0.9 baseline |
| B | P1 branch | fork REF | On | isolate P1 effect |
| C | P1 branch | absent | On | OptiScaler-only regression smoke test |

Primary RE Engine games:

```text
Monster Hunter Wilds
Dragon's Dogma 2
```

Exercise, where supported:

1. cold launch;
2. reach stable menu/gameplay;
3. Alt+Tab out and back;
4. toggle fullscreen/window/borderless modes;
5. perform a resolution change that reaches ResizeBuffers/ResizeBuffers1;
6. open/close OptiScaler overlay;
7. return to title/menu if it recreates presentation resources;
8. exit normally.

Record whether P1 changes:

- crash/hang frequency;
- resize failure behavior;
- REF renderer/overlay survival;
- XeFG survival across resize/recreation;
- normal game exit.

Do not treat unrelated `release/0.9` FSRFG-to-XeFG scene corruption as a P1 failure by itself.

---

## 16. P1 acceptance criteria

P1 is ready for review when all of the following are true:

- [ ] `reframework-0.9` was created from the fetched upstream `release/0.9`, not fork `master`.
- [ ] At branch creation time, `reframework-0.9` matched upstream `release/0.9` exactly.
- [ ] P1 is implemented on a separate feature branch.
- [ ] `XEFG_RESOURCE_REF_LIMIT` is removed.
- [ ] `oldBackBuffers` is removed.
- [ ] `hkResizeBuffers()` no longer enumerates backbuffers for refcount draining.
- [ ] `hkResizeBuffers1()` no longer enumerates backbuffers for refcount draining.
- [ ] `hkFGRelease()` no longer drains swapchain backbuffers.
- [ ] drain-related 100 ms sleeps in both resize paths are removed.
- [ ] menu render-target cleanup remains intact.
- [ ] legitimate temporary `GetBuffer()` references are still released exactly once.
- [ ] no new `AddRef()` probing or replacement refcount threshold was introduced.
- [ ] P2's `currentWrappedSwapchain` cleanup is intentionally untouched.
- [ ] no P3 lifecycle behavior was imported from current master.
- [ ] `git diff --check` passes.
- [ ] formatting check passes for touched code.
- [ ] Release x64 build passes.
- [ ] runtime smoke test shows no obvious P1 regression.

---

## 17. Review guidance

A reviewer should reject P1 if it does any of the following:

```text
- cherry-picks current master wholesale;
- introduces a new refcount threshold;
- repeatedly calls Release() to make an externally shared COM object disappear;
- adds strong State ownership/ComPtr conversion;
- changes currentWrappedSwapchain ownership behavior;
- ports fail-closed XeFG Destroy logic early;
- changes XeLL/fakenvapi integration;
- changes FSRFG/DLSSG frame or texture handling;
- mixes Subnautica 2 geometry diagnostics/fixes;
- broadens into general DXGI cleanup.
```

A reviewer should specifically confirm that the remaining one-shot `backBuffer->Release()` calls are paired with locally acquired references rather than deleting them indiscriminately.

---

## 18. Commit and PR structure

Prefer one focused implementation commit, or at most a small number of commits that remain trivially reviewable.

Suggested implementation commit message:

```text
fix: stop draining XeFG backbuffer COM references on 0.9
```

Suggested PR title:

```text
P1: remove XeFG backbuffer COM force-drain on release/0.9
```

PR base:

```text
reframework-0.9
```

The PR description should explicitly state:

```text
This is P1 only.
It backports the final backbuffer ownership invariant from fork master/PR #2
into the upstream release/0.9 architecture.
It intentionally leaves wrapper/real swapchain ownership cleanup for P2 and
fail-closed XeFG lifecycle work for P3.
```

After review, squash merge P1 into `reframework-0.9` unless review uncovers a real correctness issue.

---

## 19. Handoff to P2

After P1 is merged, `reframework-0.9` will still intentionally contain other historical ownership problems, especially:

```text
currentWrappedSwapchain Release-until-zero
currentRealSwapchain Release-until-zero / recreate ownership behavior
wrapper-owned _real cleanup behavior
State alias cleanup authority
```

Do not attempt to repair these as P1 follow-up commits.

They form P2:

```text
Owner-Scoped Swapchain Cleanup
= final combined invariant from master PR #6 + PR #13
```

This staged separation is required so runtime changes can be attributed cleanly:

```text
upstream 0.9 baseline
    -> P1 backbuffer ownership effect
    -> P2 full swapchain owner-domain effect
    -> P3 fail-closed lifecycle effect
```

---

## 20. Final implementation rule

The P1 implementation can be summarized in one sentence:

> **Remove every `release/0.9` path that uses a backbuffer's returned COM refcount as permission to release additional references, while preserving one-for-one cleanup of references OptiScaler actually owns.**

Keep the patch narrow, branch-native, and independently testable.
