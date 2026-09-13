# Work Order: `release/0.9` REFramework Compatibility P2 — Owner-Scoped Swapchain Cleanup

**Repository:** `onehoon/OptiScaler`  
**Work-order branch:** `master`  
**Fork `master` HEAD reviewed:** `acd6a7f42a3545e86986871e7b412116065d4692`  
**Fork `master` code baseline reviewed:** `7ee16d9b8b5405451322a840e1adbf1efe24dcf1`  
**Target long-lived branch:** `reframework-0.9`  
**Target baseline after P1:** `8efe12389d93a2652eb8266068cd80f2e76f0891`  
**Implementation branch to create:** `feature/reframework-0.9-p2-owner-scoped-swapchain-cleanup`  
**Date:** 2026-09-13

Related planning document:

```text
doc/RELEASE_0_9_REF_COMPATIBILITY_BACKPORT_ANALYSIS_AND_PLAN_2026-09-13.md
```

Previous stage:

```text
P1 / PR #22
8efe12389d93a2652eb8266068cd80f2e76f0891
P1: remove XeFG backbuffer COM force-drain on release/0.9
```

Master references used to derive the final ownership model:

- PR #6 — `Remove remaining XeFG swapchain COM ownership drains`
- PR #13 — `Refine XeFG swapchain cleanup ownership boundaries`
- current fork `master` final owner-scoped behavior

---

## 1. Objective

Implement P2 on top of the already-merged P1 `reframework-0.9` baseline.

P2 has one architectural objective:

> A tracked swapchain pointer is not proof of COM ownership. Each concrete owner must release and clear only the object/reference that it actually owns.

The desired owner boundaries are:

```text
XeFG public proxy lifecycle
    owns/clears only XeFG public-proxy tracking identities

WrappedIDXGISwapChain4
    owns wrapper identity cleanup
    owns exactly one underlying _real swapchain reference

State::current*Swapchain
    tracking aliases only
    no implicit AddRef
    no Release authority

Game / REFramework / Intel XeFG / other DXGI observers
    retain responsibility for their own COM references
```

This work must remove the remaining active swapchain refcount-draining behavior in `reframework-0.9`, while preserving the older `release/0.9` XeLL/fakenvapi and XeFG lifecycle architecture for the later P3 stage.

---

## 2. Branch procedure

Start from the current long-lived compatibility branch, not from `master` and not from upstream `release/0.9` again.

Expected base:

```text
reframework-0.9
8efe12389d93a2652eb8266068cd80f2e76f0891
```

Suggested commands:

```bash
git fetch origin

git switch reframework-0.9
git pull --ff-only origin reframework-0.9

git rev-parse HEAD
# Expected at work-order creation time:
# 8efe12389d93a2652eb8266068cd80f2e76f0891

git switch -c feature/reframework-0.9-p2-owner-scoped-swapchain-cleanup
```

If `reframework-0.9` has legitimately advanced after this document was written, first inspect the intervening commits. Do not reset or force-move the long-lived branch merely to match the recorded SHA.

Open the implementation PR with:

```text
base: reframework-0.9
head: feature/reframework-0.9-p2-owner-scoped-swapchain-cleanup
```

Do not merge `master` into the implementation branch.

Do not cherry-pick PR #6 or PR #13 directly.

---

## 3. Important result of the branch-to-branch code review

The current `master` and `reframework-0.9` implementations are architecturally different enough that a mechanical backport is unsafe.

The relevant differences are summarized below.

### 3.1 `State.h`

Current `master` explicitly documents:

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

Current `reframework-0.9` has the same four raw pointers but no ownership contract documented above them.

P2 must add the ownership contract without converting the fields to owning smart pointers.

### 3.2 `XeFG_Dx12.cpp`

Current `reframework-0.9` contains an active force-drain in **both** XeFG recreate entry points:

```cpp
ReleaseSwapchain(_hwnd);

if (State::Instance().currentRealSwapchain != nullptr)
{
    UINT release = 0;
    do
    {
        release = State::Instance().currentRealSwapchain->Release();
        LOG_DEBUG("Releasing swapchain, ref count: {}", release);
    } while (release > 0);
}
```

This appears in:

```text
XeFG_Dx12::CreateSwapchain(...)
XeFG_Dx12::CreateSwapchain1(...)
```

Current fork `master` no longer releases through `currentRealSwapchain` at all. PR #13 further established that XeFG recreate/teardown must not opportunistically clear the wrapper/real aliases either.

### 3.3 `FG_Hooks.cpp`

P1 already removed the backbuffer drains, but `reframework-0.9` still has this active wrapper drain in `hkFGRelease()`:

```cpp
if (State::Instance().currentWrappedSwapchain != nullptr &&
    State::Instance().currentSwapchainDesc.OutputWindow == _hwnd)
{
    auto refCount = State::Instance().currentWrappedSwapchain->Release();

    while (refCount > 0 && refCount < 0xffffff00)
    {
        refCount = State::Instance().currentWrappedSwapchain->Release();
    }

    State::Instance().currentWrappedSwapchain = nullptr;
}
```

This is an active ownership violation.

`currentWrappedSwapchain` is a tracking alias. The XeFG public-proxy release path does not own every reference represented by the wrapper object's refcount.

Final `master` does not release or clear wrapper/real aliases from this owner domain.

### 3.4 `wrapped_swapchain.cpp`

There is an important nuance in the current `reframework-0.9` code:

- the old `_real` `Release()`-until-zero loop is already inside a disabled/commented block;
- therefore P2 must **not** incorrectly describe that loop as active runtime behavior;
- the active code already calls `_real->Release()` once.

However, the wrapper's alias cleanup is still wrong/incomplete:

```cpp
if (State::Instance().currentSwapchain == this)
    State::Instance().currentSwapchain = nullptr;

if (State::Instance().currentRealSwapchain == this)
    State::Instance().currentRealSwapchain = nullptr;
```

The second comparison is conceptually wrong for the normal wrapper creation path.

`DxgiFactory_WrappedCalls.cpp` assigns:

```cpp
State::Instance().currentRealSwapchain = realSC;
...
*ppSwapChain = new WrappedIDXGISwapChain4(realSC, ...);
State::Instance().currentWrappedSwapchain = *ppSwapChain;
```

Therefore:

```text
currentRealSwapchain    -> underlying realSC / wrapper->_real
currentWrappedSwapchain -> WrappedIDXGISwapChain4 object
```

Comparing `currentRealSwapchain == this` does not express the actual identity relationship.

The wrapper also currently does not clear `currentWrappedSwapchain` when that wrapper reaches its final release.

---

## 4. Scope

### Files expected to change

```text
OptiScaler/State.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
```

### Reference-only unless a concrete compile issue proves otherwise

```text
OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp
OptiScaler/wrapped/wrapped_swapchain.h
```

`DxgiFactory_WrappedCalls.cpp` is useful for confirming alias provenance, but P2 should not need to change the factory ownership structure.

---

## 5. Explicitly out of scope

P2 is **not** P3.

Do not port current-master lifecycle machinery while doing this owner cleanup.

Specifically, do not add or backport:

- `_swapchainLifecycleMutex`;
- `_swapchainRecreationBlocked`;
- `ReleaseSwapchainLocked()`;
- `ReleaseSwapchainFromFinalProxyRelease()`;
- `SwapchainReleaseOwnedByCurrentThread()`;
- `AbortSwapchainInitialization()`;
- Destroy failure propagation changes;
- partial-init fail-closed cleanup;
- lifecycle retry logic;
- thread-local `skipReleaseChecks` solely because current master has it;
- PR #17 reentrancy-guard experiments;
- PR #18 Present/OwnedMutex changes;
- PR #19 tracing;
- XeLL/fakenvapi architecture changes;
- Reflex changes;
- Subnautica 2 geometry/resource diagnostics;
- FSRFG/DLSSG frame-ID or texture fixes.

The current `reframework-0.9` behavior of `DestroySwapchainContext()` and `ReleaseSwapchain()` is intentionally left for P3.

P2 must make ownership correct **without hiding P3 behavior inside this PR**.

---

# Task A — Document the State swapchain pointers as tracking aliases

## 6. Required change

In `OptiScaler/State.h`, add the final owner-scoped wording immediately above the four swapchain pointers.

Use this wording or a semantically equivalent form:

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

### Do not

Do not change these fields to:

```cpp
ComPtr<IDXGISwapChain>
CComPtr<IDXGISwapChain>
std::shared_ptr<...>
```

and do not add unconditional `AddRef()` merely because a pointer is stored in `State`.

A strong reference from global tracking state would change lifetime behavior and can itself interfere with XeFG teardown.

---

# Task B — Remove real-swapchain force-drain from both XeFG recreate paths

## 7. `XeFG_Dx12::CreateSwapchain()`

Current `reframework-0.9` code performs:

```cpp
LOG_INFO("Releasing old swapchain");
ReleaseSwapchain(_hwnd);

if (State::Instance().currentRealSwapchain != nullptr)
{
    UINT release = 0;
    do
    {
        release = State::Instance().currentRealSwapchain->Release();
        LOG_DEBUG("Releasing swapchain, ref count: {}", release);
    } while (release > 0);
}
```

Replace this with the ownership-safe P2 form:

```cpp
LOG_INFO("Releasing old swapchain");
ReleaseSwapchain(_hwnd);
```

That is intentionally simple.

### Why there is no replacement alias cleanup here

Do **not** replace the drain with:

```cpp
State::Instance().currentRealSwapchain = nullptr;
State::Instance().currentWrappedSwapchain = nullptr;
```

PR #13 corrected that approach on `master`.

A completed XeFG public-proxy/context transaction does not prove that the wrapper or underlying real swapchain has reached its own lifetime boundary.

Their concrete owner paths must clear them.

### Why there is no `ReleaseSwapchainLocked()` here

Current `master` uses a P3-era lifecycle helper:

```cpp
if (!ReleaseSwapchainLocked(_hwnd))
{
    ...
    return false;
}
```

`reframework-0.9` does not yet have that architecture.

Do not invent or backport it in P2.

Keep the current 0.9 `ReleaseSwapchain(_hwnd)` call semantics unchanged; P3 will separately make teardown/recreation fail-closed.

---

## 8. `XeFG_Dx12::CreateSwapchain1()`

Apply the exact same owner-boundary correction to `CreateSwapchain1()`.

Target shape:

```cpp
else if (readyToRelease)
{
    LOG_INFO("Releasing old swapchain");
    ReleaseSwapchain(_hwnd);

    // No Release through currentRealSwapchain.
    // No currentRealSwapchain/currentWrappedSwapchain clear here.
}
```

The comments shown above are explanatory only; avoid adding unnecessary permanent comments if the code is already clear.

### Acceptance for Task B

After the change there must be no `currentRealSwapchain->Release()` in either XeFG create/recreate function.

The returned COM refcount must not control additional releases.

---

# Task C — Remove wrapper ownership from `hkFGRelease()`

## 9. Remove the active `currentWrappedSwapchain` drain

In `OptiScaler/hooks/FG_Hooks.cpp`, delete the entire block that calls:

```cpp
State::Instance().currentWrappedSwapchain->Release()
```

and then loops while the returned count remains above zero.

Do not replace it with a one-shot release.

Wrong replacement:

```cpp
State::Instance().currentWrappedSwapchain->Release();
State::Instance().currentWrappedSwapchain = nullptr;
```

The issue is not merely the number of `Release()` calls. The XeFG public-proxy hook does not own a wrapper reference through this `State` alias.

---

## 10. Keep public-proxy alias cleanup owner-scoped

`hkFGRelease()` is operating on `This`, the tracked FG/XeFG public proxy when the normal guarded path is entered.

After the existing FG swapchain release call, clear only State aliases that still identify that same public proxy.

Recommended 0.9-adapted shape:

```cpp
This->AddRef();

auto& state = State::Instance();

if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
{
    if (o_FGRelease(This) == 1)
    {
        LOG_DEBUG("");

        WaitForGPUIdle();

        // Keep the existing 0.9 reentrancy/deadlock guard behavior in P2.
        skipReleaseChecks = true;

        if (state.currentFG != nullptr)
        {
            LOG_DEBUG("FG Swapchain released, release FG & swapchain context");
            state.currentFG->ReleaseSwapchain(_hwnd);
        }

        LOG_DEBUG("FG Swapchain released, clearing public proxy aliases");

        if (state.currentSwapchain == This)
            state.currentSwapchain = nullptr;

        if (state.currentFGSwapchain == This)
            state.currentFGSwapchain = nullptr;

        // Do NOT release or clear currentWrappedSwapchain here.
        // Do NOT release or clear currentRealSwapchain here.

        skipReleaseChecks = false;
        return 0;
    }
}
```

Adapt this to the existing file style rather than replacing the whole function blindly.

### Important

Do not change:

```cpp
static bool skipReleaseChecks = false;
```

to a thread-local or new synchronization mechanism in this P2 PR solely to match current `master`.

That belongs to other investigations/lifecycle work.

Do not add the current-master `dynamic_cast<XeFG_Dx12*>`, `ReleaseSwapchainFromFinalProxyRelease()`, `releaseSucceeded`, or lifecycle-reentry path in this PR.

### Owner-domain rule

The final shape must be:

```text
hkFGRelease(This)
    may clear currentSwapchain if == This
    may clear currentFGSwapchain if == This

hkFGRelease(This)
    must NOT Release currentWrappedSwapchain
    must NOT clear currentWrappedSwapchain as a teardown side effect
    must NOT Release currentRealSwapchain
    must NOT clear currentRealSwapchain as a teardown side effect
```

---

# Task D — Make the wrapper clean up only wrapper-owned identities/references

## 11. Add `<utility>` for `std::exchange`

In `OptiScaler/wrapped/wrapped_swapchain.cpp`, add:

```cpp
#include <utility>
```

Do not add a new ownership helper abstraction for this small change.

---

## 12. Fix final wrapper release alias cleanup

The current 0.9 wrapper final-release path contains:

```cpp
if (State::Instance().currentSwapchain == this)
    State::Instance().currentSwapchain = nullptr;

if (State::Instance().currentRealSwapchain == this)
    State::Instance().currentRealSwapchain = nullptr;
```

Replace the alias handling with explicit wrapper/real identities.

Recommended P2-adapted shape:

```cpp
auto& state = State::Instance();

MenuOverlayDx::CleanupRenderTarget(true, _handle);

if (state.currentSwapchain == this)
    state.currentSwapchain = nullptr;

if (state.currentWrappedSwapchain == this)
    state.currentWrappedSwapchain = nullptr;

auto* real = std::exchange(_real, nullptr);

if (state.currentRealSwapchain == real)
    state.currentRealSwapchain = nullptr;
```

This must happen only in the legitimate `ret == 0` wrapper final-release path, after the existing preserve-swapchain early-return logic.

### Why `std::exchange`

It makes the owned pointer transition explicit:

```text
wrapper owns _real
    -> snapshot owned pointer
    -> wrapper no longer stores it
    -> clear matching tracking alias
    -> release exactly that owned reference once
```

It also prevents accidental second release through `_real` later in the same final-release path.

---

## 13. Preserve the existing 0.9 FG lifecycle behavior in this task

The current wrapper also contains 0.9-specific FG cleanup:

```cpp
auto fg = State::Instance().currentFG;
if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
{
    fg->Deactivate();
    fg->ReleaseSwapchain(_handle);

    if (State::Instance().currentFGSwapchain != nullptr)
        State::Instance().currentFGSwapchain = nullptr;
}
```

Do not replace this with the current-master `releaseCompleted`/fail-closed wrapper logic in P2.

P3 will audit and correct lifecycle success/failure propagation separately.

For P2, keep the behavior structurally equivalent except for normal local `state` reuse if desired.

---

## 14. Release the wrapper-owned real swapchain reference exactly once

After the existing FG cleanup, use the saved `real` pointer:

```cpp
const auto refCount = real != nullptr ? real->Release() : 0;

LOG_DEBUG("Real swapchain released, refCount: {}", refCount);

delete this;
```

The returned `refCount` may be logged.

It must **not** control another `Release()`.

### Remove the obsolete disabled force-drain block

The current 0.9 file still contains an old commented block equivalent to:

```cpp
/*
while (refCount > 0)
{
    ...
    refCount = _real->Release();
}
*/
```

This is not active runtime behavior today, so do not describe its deletion as a runtime fix.

However, remove this dead block as part of P2 so the owner contract is unambiguous and the invalid pattern cannot be casually re-enabled later.

Do not remove unrelated historical comments elsewhere in the file.

---

# Task E — Audit factory alias provenance, but do not refactor it

## 15. `DxgiFactory_WrappedCalls.cpp`

Audit all wrapper creation variants.

Current 0.9 code repeatedly follows the pattern:

```cpp
IDXGISwapChain* realSC = nullptr;
...
State::Instance().currentRealSwapchain = realSC;
...
*ppSwapChain = new WrappedIDXGISwapChain4(realSC, ...);
...
State::Instance().currentWrappedSwapchain = *ppSwapChain;
```

Equivalent `IDXGISwapChain1` / UWP variants exist as well.

This confirms the intended identity relationship used by Task D.

No change is expected here.

Do not add `AddRef()` to the aliases.

Do not convert these assignments to owning smart pointers.

If the implementation discovers a concrete creation variant where the alias identities differ from this model, stop and document the exact code path in the PR rather than broadening P2 into a DXGI factory refactor.

---

## 16. Do not mechanically reproduce PR #6's intermediate state

PR #6 correctly removed the active drains, but its first implementation also allowed XeFG teardown paths to snapshot and clear wrapper/real aliases.

PR #13 explicitly removed that behavior.

Therefore the P2 backport must implement this final model directly:

```text
CreateSwapchain / CreateSwapchain1
    remove currentRealSwapchain force-release
    do not clear wrapper/real aliases

hkFGRelease
    remove currentWrappedSwapchain force-release
    clear only public-proxy aliases matching This
    do not clear wrapper/real aliases

WrappedIDXGISwapChain4::Release final path
    clear wrapper alias if == this
    clear real alias if == owned real
    release owned real exactly once
```

Do not first implement PR #6's cross-owner alias clearing and then correct it in a second commit.

The implementation branch should contain the final PR #13-corrected ownership model from the beginning.

---

## 17. Static validation

Run at minimum:

```bash
git diff --check
```

Build/search from the repository root.

### Required ownership searches

```bash
rg -n "currentRealSwapchain->Release" OptiScaler
rg -n "currentWrappedSwapchain->Release" OptiScaler
```

Expected for the P2-owned paths:

```text
no active release through either State alias
```

If matches remain elsewhere, inspect them individually. Do not delete unrelated code merely to make a global search empty.

Search the four touched files for release-until-zero patterns:

```bash
rg -n "while\s*\(.*refCount|while\s*\(.*release|do\s*\{" \
    OptiScaler/framegen/xefg/XeFG_Dx12.cpp \
    OptiScaler/hooks/FG_Hooks.cpp \
    OptiScaler/wrapped/wrapped_swapchain.cpp
```

This search is intentionally broad and can produce unrelated loops. Review matches manually.

The acceptance rule is not “no loops exist”; it is:

> No loop may repeatedly call COM `Release()` based on a returned refcount for `currentRealSwapchain`, `currentWrappedSwapchain`, or wrapper-owned `_real` cleanup.

### Verify ownership comments

Confirm `State.h` identifies all four pointers as tracking/non-owning aliases.

### Verify dead drain code is gone

The old commented `_real` release-until-zero block in `wrapped_swapchain.cpp` should be removed.

---

## 18. Build and format validation

Required:

```text
Release | x64 build
```

Also run the repository's existing formatting checks for touched C/C++ files.

At minimum report:

```text
git diff --check
Release|x64 build result
clang-format/check result
```

If the repository baseline itself emits known CRLF/style diagnostics, distinguish baseline diagnostics from changes introduced by this PR.

Do not perform broad formatting churn on these legacy files.

---

## 19. Runtime validation

P2 is intended to improve real multi-owner operation with REFramework, so runtime validation is important when the environment is available.

Recommended minimum matrix:

| Test | OptiScaler branch | REFramework | XeFG | Purpose |
|---|---|---|---|---|
| A | `reframework-0.9` after P1 | fork REF | On | P1 baseline |
| B | P1 + P2 branch | fork REF | On | owner-boundary effect |
| C | P1 + P2 branch | absent | On | OptiScaler-only regression smoke test |

Priority RE Engine titles:

```text
Monster Hunter Wilds
Dragon's Dogma 2
```

Exercise when possible:

1. cold launch;
2. enter stable rendering/gameplay;
3. Alt+Tab out and back;
4. fullscreen/window/borderless transition if supported;
5. resolution change / ResizeBuffers path;
6. overlay open/close;
7. menu/title transition that recreates presentation objects;
8. normal exit.

### Watch specifically for

```text
crash or hang during swapchain recreation
DXGI invalid-call failures on resize
XeFG proxy recreation failure
REF overlay/renderer disappearing after resize
stale-wrapper use after old wrapper destruction
new swapchain alias accidentally cleared by old wrapper final release
```

Runtime validation is not permission to fold P3 lifecycle fixes into this PR. If a Destroy failure or create/release overlap is observed, capture the log and leave it for P3 unless P2 itself introduced the regression.

---

## 20. What counts as success

P2 succeeds when all of the following are true:

1. `currentRealSwapchain` is no longer used as authority to drain a COM object during XeFG recreation.
2. `currentWrappedSwapchain` is no longer released/drained from `hkFGRelease()`.
3. XeFG public-proxy cleanup does not opportunistically clear wrapper/real aliases.
4. Public-proxy aliases are cleared only by pointer identity for the public proxy being retired.
5. `WrappedIDXGISwapChain4` clears `currentWrappedSwapchain` only when it points to that wrapper.
6. `WrappedIDXGISwapChain4` clears `currentRealSwapchain` only when it points to that wrapper's owned underlying real swapchain.
7. The wrapper releases its owned real swapchain reference exactly once.
8. The returned COM refcount is diagnostic only and never release authority.
9. P1 backbuffer ownership cleanup remains intact.
10. No P3 fail-closed lifecycle architecture is imported early.

---

## 21. Review checklist

### Branch/scope

- [ ] PR base is `reframework-0.9`.
- [ ] Implementation started from P1 baseline `8efe1238...` or a reviewed descendant.
- [ ] No merge from `master`.
- [ ] No raw cherry-pick of PR #6/#13.
- [ ] No unrelated modernization.

### `State.h`

- [ ] Four swapchain fields are documented as tracking aliases.
- [ ] No new AddRef/ComPtr ownership was introduced.

### `XeFG_Dx12.cpp`

- [ ] `CreateSwapchain()` no longer force-releases `currentRealSwapchain`.
- [ ] `CreateSwapchain1()` no longer force-releases `currentRealSwapchain`.
- [ ] Neither recreate path clears wrapper/real aliases as a substitute.
- [ ] Existing 0.9 `ReleaseSwapchain()` architecture is preserved for P3.

### `FG_Hooks.cpp`

- [ ] `hkFGRelease()` no longer calls `currentWrappedSwapchain->Release()`.
- [ ] No one-shot State-alias release was substituted for the loop.
- [ ] `currentWrappedSwapchain` is not cleared from the XeFG public-proxy owner domain.
- [ ] `currentRealSwapchain` is not cleared from the XeFG public-proxy owner domain.
- [ ] `currentSwapchain` is cleared only if it equals `This`.
- [ ] `currentFGSwapchain` is cleared only if it equals `This`.
- [ ] No unrelated thread-local/reentrancy/lifecycle machinery was imported.

### `wrapped_swapchain.cpp`

- [ ] `currentWrappedSwapchain == this` is cleared on actual wrapper final release.
- [ ] the underlying `real` pointer is captured from `_real` with explicit ownership transition.
- [ ] `currentRealSwapchain == real` is cleared by identity.
- [ ] wrapper-owned `real->Release()` occurs exactly once.
- [ ] returned refcount is only logged.
- [ ] obsolete commented release-until-zero block is removed.
- [ ] existing 0.9 FG lifecycle behavior is otherwise preserved for P3.

### Validation

- [ ] `git diff --check` passes.
- [ ] Release x64 build passes.
- [ ] formatting check completed and reported.
- [ ] ownership searches reviewed.
- [ ] runtime status documented honestly.

---

## 22. Suggested PR title and body

Suggested title:

```text
P2: scope swapchain cleanup to actual COM owners on release/0.9
```

Suggested summary:

```markdown
## Summary

- Remove the remaining `currentRealSwapchain` Release-until-zero behavior from both XeFG recreate paths.
- Remove `currentWrappedSwapchain` release draining from `hkFGRelease()`.
- Treat `State::current*Swapchain` as tracking aliases rather than COM owners.
- Make `WrappedIDXGISwapChain4` clear only its own wrapper/real aliases and release its owned real-swapchain reference once.
- Preserve the existing release/0.9 XeFG lifecycle architecture; fail-closed Destroy/recreation remains P3.

## Scope

P2 only. No XeLL/fakenvapi redesign, lifecycle mutex port, Destroy failure propagation, Reflex work, frame-ID changes, or diagnostic tracing.

## Validation

- `git diff --check`: ...
- Release|x64 build: ...
- clang-format/check: ...
- Runtime: ...
```

---

## 23. Final implementation rule

The central rule for this PR is:

> **A State alias may tell us which object is being tracked; it does not tell us which COM references we own.**

In practical terms:

```text
XeFG recreate
    may retire XeFG lifecycle state
    may NOT drain real swapchain refs through State

XeFG public-proxy Release hook
    may clear aliases equal to that public proxy
    may NOT release/clear wrapper or real owner identities

WrappedIDXGISwapChain4 final Release
    clears itself if tracked
    clears its actual underlying real object if tracked
    releases exactly the real reference it owns
```

Implement these invariants directly in the existing `release/0.9` architecture.

Do not make P2 look like current `master` by importing P3 infrastructure. The purpose of the staged backport is to keep ownership cleanup independently reviewable and independently testable.