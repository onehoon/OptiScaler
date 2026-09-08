# Work Order: Remaining XeFG Swapchain COM Ownership Cleanup

**Repository:** `onehoon/OptiScaler`  
**Target branch:** `master`  
**Baseline reviewed:** `6faed8a5a2093ba72a5245bbba102463fda696ae`  
**Date:** 2026-09-08  
**Related completed work:**

- PR #2 / F-01: remove aggressive XeFG backbuffer reference draining
- PR #1 / F-02: preserve failed XeFG Destroy lifecycle and fail closed on recreation

**Compatibility reference:** `onehoon/REFramework` master `32f202c8e6944bf7f1eda8f2610ebef58c69b11f`

---

## 1. Goal

Remove the remaining swapchain COM reference-draining behavior from OptiScaler and make swapchain ownership explicit and compatible with REFramework's XeFG lifecycle model.

The central rule for this work is:

> **Every component releases only COM references that it actually owns. A global/state pointer that is only a borrowed alias must never be used to drain another object's reference count.**

This work is intentionally narrower than a general DXGI refactor. It addresses the remaining ownership hazards that are directly relevant to XeFG swapchain replacement, teardown, REFramework coexistence, and the Monster Hunter Wilds investigation.

Do not add a new recovery loop, do not infer outstanding ownership from returned COM refcounts, and do not attempt to force an object to zero references.

---

## 2. Why this is required after F-01 and F-02

F-01 removed the unsafe XeFG backbuffer pattern that repeatedly called `Release()` until the returned count dropped below a threshold. That pattern could consume references owned by REFramework, the game, Intel XeFG, or another component.

F-02 corrected XeFG context teardown so that a negative `xefgSwapChainDestroy()` result keeps the live context identity and blocks replacement instead of silently constructing a second context.

However, two similar COM-draining patterns remain in the current codebase:

1. `XeFG_Dx12::CreateSwapchain()` / `CreateSwapchain1()` repeatedly release `State::Instance().currentRealSwapchain` to zero after releasing the previous XeFG swapchain.
2. `FGHooks::hkFGRelease()` repeatedly releases `State::Instance().currentWrappedSwapchain` to zero.

These are the same ownership class of bug as F-01: a pointer being globally visible does not mean OptiScaler owns every reference represented by the object's current refcount.

The remaining work must remove those drains while preserving correct teardown of references that OptiScaler really does own.

---

## 3. Ownership model to enforce

The current `State` fields are raw pointer aliases:

```cpp
IDXGISwapChain* currentSwapchain = nullptr;
IDXGISwapChain* currentWrappedSwapchain = nullptr;
IDXGISwapChain* currentRealSwapchain = nullptr;
IDXGISwapChain* currentFGSwapchain = nullptr;
```

For this work, treat these as **non-owning aliases** unless a specific creation site explicitly acquires and documents a separate COM reference for State itself.

Expected model:

```text
Game / Intel / Opti wrapper / REFramework
    each own only the references they explicitly acquire

State::currentSwapchain         -> borrowed alias
State::currentWrappedSwapchain  -> borrowed alias
State::currentRealSwapchain     -> borrowed alias
State::currentFGSwapchain       -> borrowed alias

Borrowed alias behavior:
    may compare identity
    may be cleared when the referenced object is no longer current
    must not call Release merely because the alias exists
```

Do **not** convert `currentFGSwapchain` or the other State swapchain fields to `ComPtr` as part of this work.

In particular, making `currentFGSwapchain` an owning `ComPtr` would add an OptiScaler-owned XeFG public-proxy reference that could itself prevent Intel `xefgSwapChainDestroy()` from succeeding.

---

## 4. REFramework compatibility contract

This work must remain compatible with `onehoon/REFramework` master.

The fork's active XeFG binding intentionally stores the swapchain as borrowed:

```cpp
IDXGISwapChain3* m_swapchain{}; // borrowed; hook lifetime is bounded by the caller
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
```

REFramework does acquire strong references where it actually needs them:

- XeFG discovery candidates use `ComPtr` while being evaluated.
- renderer backbuffers are acquired through `GetBuffer()` into `ComPtr` storage.
- runtime detach uses a temporary `old_keepalive` `ComPtr` only to keep the observed swapchain valid while renderer resources/hooks are removed.
- `REFramework::on_reset()` / `deinit_d3d12()` releases renderer-owned backbuffer/resource references before the Intel runtime lifecycle operation proceeds.

Therefore OptiScaler must **not** compensate for REFramework by force-releasing a real swapchain, public proxy, wrapper, or backbuffer to zero.

The expected interoperability contract is:

```text
REFramework:
    releases its own renderer/backbuffer references

OptiScaler wrapper:
    releases the reference(s) explicitly owned by the wrapper

Game/runtime/other overlays:
    release their own references

State aliases:
    only cleared; never drained

Intel XeFG Destroy:
    succeeds when all real owners have released their references
    otherwise F-02 retains/quarantines the context
```

No REFramework code change is requested by this work order.

---

## 5. Files expected to change

Primary scope:

```text
OptiScaler/State.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
```

Reference-only unless a concrete implementation need is found:

```text
OptiScaler/with_dx12/dx11_with_dx12_sc.cpp
OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp
OptiScaler/Util.cpp
```

Do not modify REFramework in this PR.

---

# 6. Task A — Document State swapchain pointers as borrowed aliases

Update the swapchain section in `OptiScaler/State.h` so future code does not treat these fields as owned references.

Suggested form:

```cpp
// Non-owning swapchain aliases.
//
// These fields do not own COM references. Assigning a pointer here does not
// transfer ownership and these aliases must never be used to drain an object's
// reference count. The concrete owner (game/runtime/wrapper/etc.) is responsible
// for releasing the references it acquired.
IDXGISwapChain* currentSwapchain = nullptr;
IDXGISwapChain* currentWrappedSwapchain = nullptr;
IDXGISwapChain* currentRealSwapchain = nullptr;
IDXGISwapChain* currentFGSwapchain = nullptr;
```

Do not add `AddRef()` merely to make this comment true in reverse. The intended State model is non-owning.

---

# 7. Task B — Remove `currentRealSwapchain` Release-until-zero loops

## 7.1 Locations

Audit both:

```cpp
XeFG_Dx12::CreateSwapchain(...)
XeFG_Dx12::CreateSwapchain1(...)
```

The current pattern is equivalent to:

```cpp
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

Delete the reference-draining behavior completely.

## 7.2 Why `currentRealSwapchain` is not an owned reference

`DxgiFactory_WrappedCalls.cpp` stores the pointer directly:

```cpp
State::Instance().currentRealSwapchain = realSC;
```

There is no State-owned `AddRef()` associated with that assignment.

Also, the Streamline unwrap helper obtains a real-object pointer with `QueryInterface`, immediately releases the QI reference, and returns the pointer as an alias:

```cpp
auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

if (qResult == S_OK && *ppRealObject != nullptr)
{
    LOG_INFO("{} Streamline proxy found!", functionName);
    (*ppRealObject)->Release();
    return true;
}
```

Therefore `currentRealSwapchain` must not be treated as an owner.

## 7.3 Replacement behavior

If alias cleanup is needed after a completed teardown, use identity-checked alias clearing, not COM release.

Recommended pattern:

```cpp
auto& state = State::Instance();
auto* oldRealAlias = state.currentRealSwapchain;
auto* oldWrappedAlias = state.currentWrappedSwapchain;

if (!ReleaseSwapchainLocked(_hwnd))
{
    LOG_ERROR("[XeFG][Lifecycle] action = recreate_aborted, api = CreateSwapchain, "
              "reason = release_not_completed");
    return false;
}

// State stores borrowed aliases only. Do not call Release() through these
// pointers. Clear an alias only if no newer object has replaced it.
if (state.currentRealSwapchain == oldRealAlias)
    state.currentRealSwapchain = nullptr;

if (state.currentWrappedSwapchain == oldWrappedAlias)
    state.currentWrappedSwapchain = nullptr;
```

Apply equivalent logic to `CreateSwapchain1()`.

### Important

Do not blindly clear a State pointer after the lifecycle call if it may already have been replaced by a new generation. Always compare to the old snapshot first.

---

# 8. Task C — Fix `WrappedIDXGISwapChain4` self-cleanup

This is required before removing the external wrapper drain.

The current `WrappedIDXGISwapChain4::Release()` clears:

```cpp
if (State::Instance().currentSwapchain == this)
    State::Instance().currentSwapchain = nullptr;

if (State::Instance().currentRealSwapchain == this)
    State::Instance().currentRealSwapchain = nullptr;
```

The second comparison is incorrect for the normal wrapper layout.

`currentRealSwapchain` stores the underlying real swapchain, while `this` is the wrapper object. The correct identity is `_real`.

The wrapper also needs to clear `currentWrappedSwapchain` when that alias points to `this`.

Use the already-correct `Dx11wDx12SC::Release()` alias cleanup as the reference pattern.

Recommended implementation shape:

```cpp
ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif

        auto& state = State::Instance();

        MenuOverlayDx::CleanupRenderTarget(true, _handle);

        // State holds borrowed aliases. Clear them before releasing the real
        // object so re-entrant teardown cannot observe a freed object as current.
        if (state.currentSwapchain == this)
            state.currentSwapchain = nullptr;

        if (state.currentWrappedSwapchain == this)
            state.currentWrappedSwapchain = nullptr;

        if (state.currentRealSwapchain == _real)
            state.currentRealSwapchain = nullptr;

        auto fg = state.currentFG;
        bool releaseCompleted = false;

        if (fg != nullptr)
        {
            if (fg->Mutex.getOwner() == 1)
            {
                releaseCompleted = false;
                LOG_WARN("[XeFG][Lifecycle] action = wrapped_release_deferred, "
                         "reason = release_already_in_progress");
            }
            else
            {
                releaseCompleted = fg->ReleaseSwapchain(_handle);

                if (!releaseCompleted)
                    LOG_ERROR("[XeFG][Lifecycle] action = wrapped_release_aborted, "
                              "reason = release_not_completed");
            }
        }

        if (releaseCompleted)
            state.currentFGSwapchain = nullptr;

        // Release only the real-swapchain reference owned by this wrapper.
        auto* real = std::exchange(_real, nullptr);
        const auto realRefCount = real != nullptr ? real->Release() : 0;

        LOG_DEBUG("Real swapchain released, refCount: {}", realRefCount);

        delete this;
    }

    return ret;
}
```

The exact local variable style may be adapted to the surrounding code, but preserve these invariants:

1. clear State aliases before freeing the underlying object;
2. compare `currentRealSwapchain` to `_real`, not `this`;
3. clear `currentWrappedSwapchain` when it points to `this`;
4. release the wrapper-owned `_real` reference exactly once;
5. never loop on the returned COM refcount.

Do not use the returned `realRefCount` as an instruction to call `Release()` again. It may be logged diagnostically only.

---

# 9. Task D — Remove `currentWrappedSwapchain` Release-until-zero from `hkFGRelease()`

Current code includes a pattern equivalent to:

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

Delete the COM-drain logic.

`currentWrappedSwapchain` is also a raw State alias. The wrapper's own COM refcount belongs to callers that acquired references through the wrapper's COM interface.

After Task C, a wrapper reaching its legitimate final `Release()` will clear its own State aliases.

If `hkFGRelease()` still needs to detach a stale alias after a successful XeFG teardown, clear only the pointer identity that was present before the transaction.

Example:

```cpp
auto& state = State::Instance();
auto* wrappedAliasBeforeRelease = state.currentWrappedSwapchain;
auto* realAliasBeforeRelease = state.currentRealSwapchain;

// ... existing XeFG final-proxy lifecycle transaction ...

if (!releaseSucceeded)
{
    skipReleaseChecks = false;
    LOG_ERROR("[XeFG][Lifecycle] action = fg_release_aborted, "
              "reason = release_swapchain_failed");
    return 0;
}

// Borrowed aliases only: do not Release through them.
if (state.currentWrappedSwapchain == wrappedAliasBeforeRelease)
    state.currentWrappedSwapchain = nullptr;

if (state.currentRealSwapchain == realAliasBeforeRelease)
    state.currentRealSwapchain = nullptr;
```

Only add the alias clearing if it is actually necessary after checking all wrapper self-cleanup paths. Prefer wrapper self-cleanup as the primary authority.

Under no circumstances replace the old loop with:

```cpp
currentWrappedSwapchain->Release(); // one unconditional release
```

unless there is a separately documented OptiScaler-owned reference acquired specifically for State. At present the reviewed design does not establish such ownership.

---

# 10. Task E — Clear all aliases to the final XeFG public proxy before its final COM Release

`FGHooks::SetFGSwapchain()` assigns the same XeFG proxy to at least:

```cpp
State::Instance().currentFGSwapchain = pSwapChain;
State::Instance().currentSwapchain = pSwapChain;
```

PR #1's `ReleaseSwapchainFromFinalProxyRelease()` correctly consumes the final proxy reference before Intel Destroy. Before that final COM release executes, any State alias pointing to the same proxy must be cleared so no later/re-entrant code observes a freed proxy as current.

Current helper shape already clears `currentFGSwapchain`. Extend the invariant to `currentSwapchain` when it aliases the exact same proxy.

Recommended shape:

```cpp
bool finalProxyReleased = false;
auto releaseFinalProxyOnce = [&]() {
    if (finalProxyReleased)
        return;

    auto& state = State::Instance();
    auto* finalProxy = state.currentFGSwapchain;

    if (state.currentSwapchain == finalProxy)
        state.currentSwapchain = nullptr;

    state.currentFGSwapchain = nullptr;

    releaseFinalProxy();
    finalProxyReleased = true;
};
```

Do not clear a different `currentSwapchain` identity that may already represent a newer swapchain.

If additional State fields are found to alias the same final XeFG proxy, apply the same identity-checked cleanup principle. Do not broaden this into a generic global pointer wipe.

---

# 11. Task F — Audit all remaining Release-until-zero swapchain code in the modified paths

Within the files touched by this PR, search for patterns such as:

```text
while (... Release())
do { ... Release(); } while (...)
refCount > 0
refCount < 0xffffff00
currentRealSwapchain->Release
currentWrappedSwapchain->Release
```

For each result, classify it:

```text
A. Reference definitely owned by the current component
   -> release exactly the number of references acquired by that component

B. Borrowed/raw State alias
   -> no Release; identity-checked nullptr only

C. Diagnostic AddRef/Release probe
   -> net-zero only; must not be used to drain unrelated references

D. Unknown ownership
   -> do not force-release; leave fail-closed and add a concise diagnostic if needed
```

Do not turn this work into a repository-wide COM rewrite. Report additional suspicious locations in the PR description if found, but modify only paths directly relevant to swapchain/XeFG lifecycle unless a concrete crash/UAF is demonstrated.

---

# 12. Important wrapper ownership details

`WrappedIDXGISwapChain4` must remain responsible for its own real swapchain lifetime.

Do not move that ownership into `State`.

The expected wrapper-level model is:

```text
new WrappedIDXGISwapChain4(realSC, ...)
    wrapper represents one logical ownership relationship to realSC

wrapper final Release()
    clear State aliases that still point at wrapper/realSC
    run FG teardown coordination
    release wrapper-owned realSC reference exactly once
    delete wrapper
```

If investigation of constructor ownership shows `_real` is currently relying on the factory's returned reference rather than an explicit `AddRef()`, preserve the existing net ownership count while removing only the illegal external drains.

Do not add a second permanent `_real->AddRef()` without balancing creation-site ownership. The goal is not to increase references; it is to make the existing ownership boundary explicit and release it once.

---

# 13. Preserve F-02 fail-closed behavior

This cleanup must not weaken PR #1.

Still required:

```text
negative xefgSwapChainDestroy
    -> exact _swapChainContext retained
    -> _swapchainRecreationBlocked = true
    -> ReleaseSwapchain returns false
    -> same-window replacement aborts
    -> no new XeFG context is initialized over the live old context
```

If removing force-release behavior causes Intel Destroy to fail more visibly because another real owner still holds a reference, **that is correct behavior**.

Do not reintroduce draining as a way to make Destroy succeed.

The correct result is:

```text
remaining legitimate owner exists
    -> Intel Destroy reports failure
    -> F-02 quarantine/fail-closed path activates
    -> logs identify the lifecycle failure
```

rather than silently consuming another component's references.

---

# 14. Preserve F-01 behavior

Do not reintroduce any backbuffer refcount draining in:

```text
hkResizeBuffers
hkResizeBuffers1
hkFGRelease
```

Specifically forbidden:

```cpp
while (backBuffer->Release() > N) { ... }
```

or any variation that uses a returned COM refcount as permission to release references that OptiScaler did not acquire.

---

# 15. REFramework-specific validation reasoning

This change should improve coexistence with REFramework rather than require coordinated code modifications.

The REF fork's relevant lifecycle is:

```text
Intel/Opti XeFG Destroy path intercepted by REFramework
    -> runtime transition begins
    -> active XeFG renderer binding detached
    -> REFramework on_reset()
    -> D3D12 renderer resources/backbuffer ComPtrs released
    -> swapchain instance hooks removed
    -> temporary keepalive released at scope end
    -> Intel original Destroy executes
```

Therefore runtime validation with REFramework loaded must specifically verify:

- no REF overlay disappearance caused by OptiScaler over-releasing a swapchain;
- no REF renderer UAF around reset/rebind;
- no abnormal huge COM refcount values followed by repeated Release logging;
- XeFG Destroy failure, if one occurs, remains visible as F-02 lifecycle diagnostics rather than being hidden by reference draining;
- after successful replacement, REF can discover/rebind to the new Intel-owned native swapchain as before.

Do not add special-case calls from OptiScaler into REFramework.

---

# 16. Logging requirements

Keep logs concise and lifecycle-oriented.

Useful new logs, if needed:

```text
[XeFG][Ownership] action = alias_cleared, kind = real_swapchain, ptr = ...
[XeFG][Ownership] action = alias_cleared, kind = wrapped_swapchain, ptr = ...
[XeFG][Ownership] action = final_proxy_aliases_cleared, ptr = ...
```

Do **not** add per-frame ownership logging.

Do not log returned COM refcount values as an instruction or success criterion. If a wrapper-owned single `Release()` logs its returned count, make the message informational only.

---

# 17. Tests / static validation

At minimum run:

```text
MSBuild.exe OptiScaler\OptiScaler.vcxproj /m:1 /p:Configuration=Release /p:Platform=x64 /t:Build /nologo /verbosity:minimal

git diff --check
```

Static searches after the patch:

```text
currentRealSwapchain->Release
currentWrappedSwapchain->Release
while.*Release
refCount > 0
0xffffff00
```

Expected result in the targeted XeFG/wrapper lifecycle:

- no `currentRealSwapchain` drain-to-zero loop;
- no `currentWrappedSwapchain` drain-to-zero loop;
- no replacement one-shot Release through a borrowed State alias;
- wrapper final Release clears `currentWrappedSwapchain == this`;
- wrapper final Release clears `currentRealSwapchain == _real`;
- final XeFG proxy release clears `currentSwapchain` if it aliases the same proxy;
- F-02 negative Destroy state retention remains intact.

---

# 18. Runtime validation matrix

Hardware/runtime validation is strongly recommended before merge.

## A. Monster Hunter Wilds — Intel XeFG + REFramework

Primary target.

Exercise:

```text
cold launch
main menu
load gameplay
multiple alt-tabs
window/fullscreen or borderless transitions if supported
resolution changes / resize events
several scene transitions
FG toggle where supported
exit to title / reload
normal game exit
```

Check:

```text
REFramework overlay remains usable
no fatal D3D error introduced
no repeated swapchain Release-to-zero logging
no use-after-free symptoms in REF hooks
no duplicate XeFG context creation after failed Destroy
```

## B. Monster Hunter Wilds — Intel XeFG without REFramework

Confirms the cleanup is not accidentally depending on REF lifecycle hooks.

## C. Dragon's Dogma 2 — Intel XeFG + REFramework

Useful secondary RE Engine validation because prior P3/P3.1 alt-tab testing was stable.

Exercise several alt-tab / resize / scene transition cycles.

## D. Non-XeFG frame generation smoke test

At least one FSR-FG path if readily available.

The State alias comment and wrapper self-cleanup must not accidentally break non-XeFG wrapper lifetime.

---

# 19. Expected successful lifecycle after this PR

```text
Game requests old swapchain teardown/replacement
    -> OptiScaler serializes XeFG lifecycle
    -> REFramework runtime hook sees transition
       -> releases its own renderer/backbuffer refs
       -> detaches borrowed swapchain binding
    -> OptiScaler releases only references it actually owns
       -> wrapper releases its own underlying real swapchain ref once
       -> State borrowed aliases are cleared by identity
    -> Intel xefgSwapChainDestroy
       -> success/non-negative warning
    -> replacement may proceed
```

No component force-releases references owned by another component.

---

# 20. Expected Destroy-failure lifecycle after this PR

```text
A legitimate external owner still has a XeFG/swapchain reference
    -> OptiScaler does NOT drain it
    -> Intel xefgSwapChainDestroy returns negative
    -> F-02 restores exact context
    -> recreation is blocked
    -> ReleaseSwapchain reports failure
    -> no second context is created
    -> support log exposes the failed lifecycle
```

This is preferable to forcing refcount zero and risking UAF/corruption.

---

# 21. Non-goals

Do not include the following unless a concrete regression is found while implementing this work:

- REFramework source changes;
- broad conversion of raw DXGI pointers to `ComPtr`;
- general Streamline refactor;
- Special K removal work;
- generic DXGI wrapper redesign;
- automatic XeFG Destroy retry loops;
- background recovery threads;
- backbuffer ownership changes already completed by F-01;
- changes to XeFG result semantics already completed by F-02/F-03 work;
- unrelated `Release()` cleanup elsewhere in the repository;
- speculative locking redesign unrelated to the ownership paths above.

---

# 22. Suggested implementation order

Use this sequence to avoid exposing stale aliases between intermediate commits:

### Commit 1 — Wrapper alias correctness

- document State swapchain aliases as non-owning;
- fix `WrappedIDXGISwapChain4::Release()`:
  - `currentWrappedSwapchain == this` clear;
  - `currentRealSwapchain == _real` clear;
  - alias clearing before underlying real release;
  - single wrapper-owned `_real->Release()` only.

### Commit 2 — Remove real-swapchain force drain

- remove drain loops from `XeFG_Dx12::CreateSwapchain()`;
- remove drain loops from `XeFG_Dx12::CreateSwapchain1()`;
- use identity-checked alias clearing where lifecycle cleanup requires it.

### Commit 3 — Remove wrapped-swapchain force drain and final-proxy alias cleanup

- remove `currentWrappedSwapchain` Release-until-zero logic from `hkFGRelease()`;
- ensure alias cleanup remains correct without external forced wrapper destruction;
- clear `currentSwapchain` before final XeFG proxy release when it aliases `currentFGSwapchain`.

Keeping these as separate commits in one PR makes review easier while still landing the ownership contract atomically at PR level.

---

# 23. Review checklist

Before marking the PR ready:

- [ ] PR is based on current `master` containing merged PR #1 and PR #2.
- [ ] `State` swapchain raw pointers are explicitly documented as non-owning aliases.
- [ ] No State swapchain pointer is converted to an owning `ComPtr`.
- [ ] Both `currentRealSwapchain` drain-to-zero loops are removed.
- [ ] `hkFGRelease()` no longer drains `currentWrappedSwapchain` to zero.
- [ ] No one-shot replacement `Release()` is performed merely because a borrowed State alias is non-null.
- [ ] `WrappedIDXGISwapChain4::Release()` clears `currentWrappedSwapchain == this`.
- [ ] `WrappedIDXGISwapChain4::Release()` clears `currentRealSwapchain == _real`, not `== this`.
- [ ] alias clearing occurs before the underlying real swapchain may be freed.
- [ ] wrapper-owned `_real` reference is released exactly once.
- [ ] final XeFG proxy release clears `currentSwapchain` when it aliases the same proxy.
- [ ] identity checks prevent a newer swapchain generation from being cleared accidentally.
- [ ] PR #1 Destroy failure quarantine remains intact.
- [ ] F-01 Resize/Resize1 behavior remains intact.
- [ ] Release x64 build passes.
- [ ] `git diff --check` passes.
- [ ] static searches confirm the targeted force-drain patterns are gone.
- [ ] runtime validation status is explicitly reported.

---

# 24. PR description requirements

The PR description should clearly state that this is an ownership cleanup following F-01/F-02, not a recovery feature.

Suggested summary:

```text
This PR removes the remaining swapchain COM reference-draining behavior from
OptiScaler's XeFG/wrapper lifecycle. State swapchain pointers are treated as
borrowed aliases, wrapper-owned references are released exactly once by their
owner, and XeFG teardown no longer attempts to force real/wrapped swapchains to
refcount zero.

This aligns OptiScaler with the ownership model used by the REFramework XeFG
compatibility fork: REFramework releases its own renderer/backbuffer ComPtrs
before Intel lifecycle calls, while OptiScaler releases only references that it
owns. A legitimate outstanding reference is therefore allowed to surface as an
Intel Destroy failure and is handled by the existing F-02 fail-closed lifecycle.
```

Mention explicitly:

```text
REFramework code changes: none
Automatic Destroy retries: none
Force-release loops added: none
```

---

# 25. Completion handoff

When implementation is finished, report:

1. branch name;
2. head SHA;
3. PR number/link;
4. changed files;
5. exact force-release loops removed;
6. how each State swapchain alias is now treated;
7. `WrappedIDXGISwapChain4` alias-cleanup changes;
8. final XeFG proxy alias-cleanup changes;
9. static search results;
10. Release x64 build result;
11. `git diff --check` result;
12. runtime validation performed/not performed;
13. any additional suspicious ownership locations discovered but intentionally left out of scope.

Do not auto-merge the implementation PR. Submit it for review first.
