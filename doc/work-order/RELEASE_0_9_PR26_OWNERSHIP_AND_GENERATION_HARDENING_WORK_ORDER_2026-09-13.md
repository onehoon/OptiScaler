# Release/0.9 PR26 Ownership and Generation Hardening Work Order

Date: 2026-09-13

## Naming note

This document keeps the project-local name **PR26** because the release/0.9 compatibility plan has referred to the next ownership/generation pass by that name.

However, GitHub pull request **#26 is already occupied by the unrelated unsigned-release branch-selector change and has been merged to fork `master`**. Therefore the implementation PR created from this work order must use the next available GitHub PR number. Do not retarget, reopen, or reuse GitHub PR #26.

Suggested implementation title:

```text
P4: finish release/0.9 swapchain ownership and generation hardening
```

Suggested implementation branch:

```text
feature/reframework-0.9-p4-ownership-generation
```

---

## 1. Objective

Finish the remaining release/0.9 swapchain ownership and delayed-destroy generation hazards after P1, P2, P3, and the compatibility-parity pass.

Implementation target:

```text
branch: reframework-0.9
baseline: ce01abdf4ff28ec9a64aebd64e8612554630762d
```

Reference branch reviewed:

```text
branch: master
baseline reviewed: 1f9fe9074c2950093077a8e73891176d8346f51f
```

This is not a normal master backport. The important remaining ownership bugs exist on **both** branches. `reframework-0.9` already contains stronger P1/P2/P3 lifecycle protections than current master in several places, so do not copy master swapchain-lifecycle code wholesale.

The goal is to establish these final invariants:

```text
1. State::current*Swapchain pointers remain non-owning tracking aliases.
2. No FFX/FSR3 input path may Release() through a tracking alias to force an object to zero.
3. A wrapper may tear down an FG lifecycle only when it is still bound to that lifecycle generation.
4. A delayed destroy for an old FFX/FSR3 fake context must never destroy the current newer context.
5. QueryInterface-acquired factory references remain alive until their last use.
6. Existing P3 fail-closed XeFG Destroy/recreation behavior must remain unchanged.
```

---

# 2. Reviewed branch findings

## 2.1 `State::current*Swapchain` ownership contract is already explicit in release/0.9

P2 documented these pointers as borrowed aliases:

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

PR26 must preserve this contract.

Do not convert these fields into global strong `ComPtr`s. Doing so would introduce new lifetime ownership and can itself prevent XeFG destruction.

---

## 2.2 FFX API input still violates the P2 ownership contract

File:

```text
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
```

The release/0.9 branch still contains Release-until-zero logic in the swapchain creation paths and destroy path.

Current pattern:

```cpp
if (State::Instance().currentWrappedSwapchain != nullptr &&
    State::Instance().currentSwapchainDesc.OutputWindow == cDesc->desc->OutputWindow)
{
    auto refCount = State::Instance().currentWrappedSwapchain->Release();

    while (refCount > 0 && refCount < 0xffffff00)
    {
        refCount = State::Instance().currentWrappedSwapchain->Release();
    }

    State::Instance().currentWrappedSwapchain = nullptr;
}
```

Equivalent logic exists in:

```text
FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12
FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12
ffxDestroyContext_Dx12FG()
```

Current master still contains the same pattern.

This is not a release/0.9-only regression and there is no merged master implementation to backport.

### Why it is wrong

`currentWrappedSwapchain` is a borrowed alias. Its presence proves only that OptiScaler observed a wrapper pointer. It does not prove that OptiScaler owns every COM reference on the wrapper.

A Release-until-zero loop can consume references owned by:

```text
game
REFramework
Streamline
overlay/observer
another wrapper layer
other third-party integration
```

This is exactly the class of ownership violation P1/P2 removed from the direct XeFG path.

---

## 2.3 Legacy FSR3 input contains the same force-drain

File:

```text
OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp
```

Both legacy swapchain creation entry points still force-drain `currentWrappedSwapchain`:

```text
hkffxCreateFrameinterpolationSwapchainDX12
hkffxCreateFrameinterpolationSwapchainForHwndDX12
```

Current pattern:

```cpp
auto refCount = State::Instance().currentWrappedSwapchain->Release();

while (refCount > 0 && refCount < 0xffffff00)
{
    refCount = State::Instance().currentWrappedSwapchain->Release();
}

State::Instance().currentWrappedSwapchain = nullptr;
```

Current master also retains this pattern.

PR26 must remove these loops from both FFX APIs, not just the newer FfxApi path.

---

## 2.4 FFX/legacy FSR3 factory interfaces are released before their last use

The direct XeFG factory lifetime was fixed in the previous compatibility pass, but the input adapters still have the same COM lifetime smell.

### FfxApi path

Current pattern:

```cpp
IDXGIFactory2* factory = nullptr;
auto scResult = cDesc->dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory));

if (factory == nullptr)
    return FFX_API_RETURN_ERROR_PARAMETER;

factory->Release();

// factory used after releasing the QI reference
factory->CreateSwapChainForHwnd(...);
```

### legacy FSR3 path

Current pattern:

```cpp
IDXGIFactory2* df = nullptr;

if (dxgiFactory->QueryInterface(IID_PPV_ARGS(&df)) == S_OK)
{
    df->Release();

    // df used after releasing the QI reference
    df->CreateSwapChainForHwnd(...);
}
```

The object usually survives because another reference exists, but the local code no longer owns a valid lifetime guarantee after `Release()`.

Since PR26 already modifies these functions, fix this here using local RAII.

---

## 2.5 release/0.9 wrapper final Release is safer than master, but still generation-blind

File:

```text
OptiScaler/wrapped/wrapped_swapchain.cpp
```

release/0.9 currently performs identity-scoped alias cleanup:

```cpp
if (state.currentSwapchain == this)
    state.currentSwapchain = nullptr;

if (state.currentWrappedSwapchain == this)
    state.currentWrappedSwapchain = nullptr;

auto* real = std::exchange(_real, nullptr);

if (state.currentRealSwapchain == real)
    state.currentRealSwapchain = nullptr;
```

It also protects `currentFGSwapchain` clearing using a snapshot:

```cpp
auto* fgProxyBeforeRelease = state.currentFGSwapchain;
...
if (releaseCompleted && state.currentFGSwapchain == fgProxyBeforeRelease)
    state.currentFGSwapchain = nullptr;
```

Current master is weaker here and clears `currentFGSwapchain` after a successful wrapper-triggered release without the same exact proxy identity check.

Therefore **do not overwrite the release/0.9 wrapper Release implementation with master**.

### Remaining problem

The wrapper still calls:

```cpp
releaseCompleted = fg->ReleaseSwapchain(_handle);
```

based only on a current FG object and matching HWND inside the feature.

An old wrapper from generation A can survive because an external observer such as REFramework still holds it. A newer XeFG lifecycle B may then be created on the same HWND. When wrapper A finally reaches refcount zero, it can call `ReleaseSwapchain(_handle)` and destroy lifecycle B.

P3 solved this generation class for the public XeFG proxy by passing the exact triggering `finalProxy` and refusing stale proxy teardown. PR26 must extend equivalent protection to wrapper-triggered teardown.

---

## 2.6 FFX fake context identities are generation-unsafe

File:

```text
OptiScaler/inputs/FG/FfxApi_Dx12_FG.h
```

Current constants:

```cpp
const size_t scContext = 0x13375CC;
const size_t fgContext = 0x133757C;
```

Every swapchain context generation receives the same opaque value.
Every FG context generation receives the same opaque value.

Destroy currently distinguishes only by those static values:

```cpp
if (State::Instance().currentFG != nullptr && (void*) scContext == *context)
{
    State::Instance().currentFG->ReleaseSwapchain(...);
}
else if (State::Instance().currentFG != nullptr && (void*) fgContext == *context)
{
    State::Instance().currentFG->DestroyFGContext();
}
```

Delayed destroy scenario:

```text
FFX swapchain context A created
new context B replaces A
A destroy arrives late
A and B both equal 0x13375CC
A destroy tears down current lifecycle B
```

The same structural problem exists for `fgContext`.

PR26 must replace static fake identities with per-create identity.

---

## 2.7 Legacy FSR3 FG context also uses one static identity

File:

```text
OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp
```

Current legacy context marker:

```cpp
const UINT fgContext = 0x1337;
```

Create stores it in the public FSR3 context:

```cpp
*context = {};
context->data[0] = fgContext;
```

Destroy checks the same global marker:

```cpp
if (State::Instance().currentFG != nullptr && fgContext == context->data[0])
    State::Instance().currentFG->DestroyFGContext();
```

A delayed destroy for an old legacy FSR3 context is therefore also unable to distinguish old generation from current generation.

Because PR26 already touches this file for ownership cleanup, harden this marker as part of the same PR.

---

# 3. Required implementation structure

PR26 should remain focused. Expected files:

```text
OptiScaler/State.h
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.h
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/inputs/FG/FfxApi_Dx12_FG.h
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp
```

A small include-only adjustment is acceptable if required for WRL or a container type.

Do not broaden the PR into unrelated FG timing/resource/dispatch changes.

---

# 4. Part A — Remove every remaining wrapped-swapchain force-drain

## A1. Introduce detach-only tracking semantics

When an FFX/FSR3 replacement path detects an older tracked wrapper for the same HWND, it may detach OptiScaler's **aliases**, but it must not consume unknown COM references.

Recommended helper local to the relevant input file, or shared only if doing so stays trivial:

```cpp
static void DetachTrackedWrappedSwapchainForReplacement(HWND hwnd, const char* reason)
{
    auto& state = State::Instance();
    auto* tracked = state.currentWrappedSwapchain;

    if (tracked == nullptr || state.currentSwapchainDesc.OutputWindow != hwnd)
        return;

    LOG_INFO("[FG][Ownership] action = detach_wrapped_alias, wrapper = {:X}, hwnd = {:X}, reason = {}",
             (size_t) tracked, (size_t) hwnd, reason);

    if (state.currentSwapchain == tracked)
        state.currentSwapchain = nullptr;

    // Borrowed alias only. Do not Release().
    state.currentWrappedSwapchain = nullptr;
}
```

The exact helper placement is flexible.

The non-negotiable rule is:

```text
alias detach != COM Release
```

Do not call:

```cpp
tracked->Release();
```

from this helper.

Do not infer ownership from the returned refcount.

---

## A2. Apply to FfxApi NEW path

Replace the current Release-until-zero block in:

```text
FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12
```

with:

```cpp
DetachTrackedWrappedSwapchainForReplacement(cDesc->desc->OutputWindow, "ffx_new_swapchain");
```

Then continue with the factory create call.

---

## A3. Apply to FfxApi FOR_HWND path

Replace the current drain with:

```cpp
DetachTrackedWrappedSwapchainForReplacement(cDesc->hwnd, "ffx_for_hwnd_swapchain");
```

No loop and no single speculative `Release()` are allowed.

---

## A4. Apply to FfxApi destroy path

After a successful current-generation FG `ReleaseSwapchain`, do not force-drain the tracked wrapper.

If an alias still points at an object that the current destroyed lifecycle no longer tracks, clear only the alias by exact identity / expected HWND.

Example:

```cpp
if (releaseCompleted)
{
    auto& state = State::Instance();

    if (state.currentWrappedSwapchain != nullptr &&
        state.currentSwapchainDesc.OutputWindow == destroyedHwnd)
    {
        LOG_DEBUG("[FG][Ownership] clearing tracked wrapped alias after successful FFX lifecycle destroy");
        state.currentWrappedSwapchain = nullptr;
    }
}
```

Prefer an exact binding/generation check where available. Do not use this cleanup to release a wrapper.

---

## A5. Apply equivalent detach-only behavior to legacy FSR3

Functions:

```text
hkffxCreateFrameinterpolationSwapchainDX12
hkffxCreateFrameinterpolationSwapchainForHwndDX12
```

Replace both drain loops with detach-only alias cleanup.

No `while (refCount...)` pattern should remain in these swapchain replacement paths after PR26.

---

# 5. Part B — Fix FFX/FSR3 factory COM lifetime

Use local WRL `ComPtr` in both remaining `IDXGIFactory2` query paths.

## B1. FfxApi example

```cpp
Microsoft::WRL::ComPtr<IDXGIFactory2> factory;

const auto scResult = cDesc->dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory));
if (FAILED(scResult) || factory == nullptr)
    return FFX_API_RETURN_ERROR_PARAMETER;

DetachTrackedWrappedSwapchainForReplacement(cDesc->hwnd, "ffx_for_hwnd_swapchain");

const auto result = factory->CreateSwapChainForHwnd(
    cDesc->gameQueue,
    cDesc->hwnd,
    cDesc->desc,
    cDesc->fullscreenDesc,
    nullptr,
    reinterpret_cast<IDXGISwapChain1**>(cDesc->swapchain));
```

The QI-acquired reference stays alive through `CreateSwapChainForHwnd()` and is released by RAII on return.

Do not manually call `factory->Release()` before the create call.

---

## B2. legacy FSR3 example

```cpp
Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;

if (FAILED(dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory2))))
    return Fsr3::FFX_ERROR_BACKEND_API_ERROR;

DetachTrackedWrappedSwapchainForReplacement(hWnd, "fsr3_for_hwnd_swapchain");

const auto result = factory2->CreateSwapChainForHwnd(
    queue,
    hWnd,
    desc1,
    fullscreenDesc,
    nullptr,
    reinterpret_cast<IDXGISwapChain1**>(&outGameSwapChain));
```

Preserve the existing API's exact error mapping unless a change is required for compilation.

---

# 6. Part C — Add FG swapchain lifecycle generation tracking

The wrapper needs an identity stronger than HWND.

A lightweight release/0.9-specific generation counter is preferred over widening the virtual FG interface.

## C1. Add State tracking fields

File:

```text
OptiScaler/State.h
```

Recommended fields next to swapchain tracking aliases:

```cpp
// Monotonic identity for the currently published FG swapchain lifecycle.
// This is tracking metadata only; it does not imply COM ownership.
std::atomic_uint64_t nextFGSwapchainGeneration { 0 };
std::atomic_uint64_t currentFGSwapchainGeneration { 0 };
```

If `State.h` does not already include `<atomic>` in this branch, add it explicitly.

Generation `0` means "not bound to a published FG lifecycle".

Do not reset `nextFGSwapchainGeneration` during normal runtime.

---

## C2. Publish a new generation only for a genuinely new FG proxy

Files:

```text
OptiScaler/hooks/FG_Hooks.cpp
```

Functions:

```text
FGHooks::CreateSwapChain
FGHooks::CreateSwapChainForHwnd
```

After successful FG swapchain creation, compare the old and new public FG proxy.

Example:

```cpp
auto& state = State::Instance();
IUnknown* previousFGSwapchain = state.currentFGSwapchain;
...

if (scResult)
{
    auto* newFGSwapchain =
        ppSwapChain != nullptr ? static_cast<IUnknown*>(*ppSwapChain) : nullptr;

    if (newFGSwapchain != nullptr && newFGSwapchain != previousFGSwapchain)
    {
        const uint64_t generation =
            state.nextFGSwapchainGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

        state.currentFGSwapchainGeneration.store(generation, std::memory_order_release);

        LOG_INFO("[FG][Lifecycle] action = publish_generation, generation = {}, proxy = {:X}, hwnd = {:X}",
                 generation, (size_t) newFGSwapchain, (size_t) pDesc->OutputWindow);
    }

    state.currentFGSwapchain = *ppSwapChain;
    ...
}
```

Equivalent logic applies to `CreateSwapChainForHwnd`.

Important:

```text
same public proxy + preserve/resize path => do not increment generation
new public proxy => increment generation
failed creation => do not increment generation
```

The generation is metadata only. Never AddRef/Release because of it.

---

# 7. Part D — Bind wrappers to the observed lifecycle generation

## D1. Capture generation in the wrapper

File:

```text
OptiScaler/wrapped/wrapped_swapchain.h
```

Add:

```cpp
uint64_t _fgGenerationAtCreation = 0;
```

In the constructor:

```cpp
_fgGenerationAtCreation =
    State::Instance().currentFGSwapchainGeneration.load(std::memory_order_acquire);
```

This snapshot does not grant ownership. It is only one part of the authorization test.

---

## D2. Final wrapper release must prove both alias identity and lifecycle generation

Before clearing aliases, capture whether this wrapper was still the tracked wrapper:

```cpp
auto& state = State::Instance();

const bool wasCurrentWrapped = state.currentWrappedSwapchain == this;
const uint64_t wrapperGeneration = _fgGenerationAtCreation;
const uint64_t currentGeneration =
    state.currentFGSwapchainGeneration.load(std::memory_order_acquire);
```

Then preserve the existing P2 identity-based alias clears.

Only authorize wrapper-triggered FG teardown when all required conditions match.

Recommended shape:

```cpp
const bool canReleaseCurrentFgLifecycle =
    wasCurrentWrapped &&
    wrapperGeneration != 0 &&
    wrapperGeneration == currentGeneration &&
    fg != nullptr &&
    state.currentFGSwapchain != nullptr &&
    fg->Hwnd() == _handle;

if (canReleaseCurrentFgLifecycle)
{
    if (fg->Mutex.getOwner() == 1)
    {
        LOG_WARN("[FG][Lifecycle] action = wrapped_release_deferred, generation = {}, "
                 "reason = release_already_in_progress",
                 wrapperGeneration);
    }
    else
    {
        releaseCompleted = fg->ReleaseSwapchain(_handle);

        if (!releaseCompleted)
            LOG_ERROR("[FG][Lifecycle] action = wrapped_release_aborted, generation = {}, "
                      "reason = release_not_completed",
                      wrapperGeneration);
    }
}
else if (fg != nullptr && state.currentFGSwapchain != nullptr)
{
    LOG_INFO("[FG][Lifecycle] action = stale_wrapped_release_skipped, wrapper = {:X}, "
             "wrapper_generation = {}, current_generation = {}, was_current_wrapped = {}, hwnd = {:X}",
             (size_t) this,
             wrapperGeneration,
             currentGeneration,
             wasCurrentWrapped,
             (size_t) _handle);
}
```

### Critical behavior

```text
old wrapper A, generation 4
new FG lifecycle B, generation 5
A final Release
=> release A's owned `_real` reference exactly once
=> delete wrapper A
=> DO NOT call ReleaseSwapchain() for lifecycle B
```

---

## D3. Preserve release/0.9's existing proxy identity clear guard

Keep the current release/0.9 rule:

```cpp
auto* fgProxyBeforeRelease = state.currentFGSwapchain;
...
if (releaseCompleted && state.currentFGSwapchain == fgProxyBeforeRelease)
    state.currentFGSwapchain = nullptr;
```

Do not replace it with master's broader:

```cpp
if (releaseCompleted)
    state.currentFGSwapchain = nullptr;
```

The release/0.9 behavior is safer.

After a successful authorized teardown, clear the generation only if the exact published proxy/generation still matches the values captured before teardown.

Example:

```cpp
if (releaseCompleted &&
    state.currentFGSwapchain == fgProxyBeforeRelease &&
    state.currentFGSwapchainGeneration.load(std::memory_order_acquire) == wrapperGeneration)
{
    state.currentFGSwapchain = nullptr;
    state.currentFGSwapchainGeneration.store(0, std::memory_order_release);
}
```

If a newer generation was published concurrently, do not clear it.

---

# 8. Part E — Replace static FfxApi fake contexts with per-create tokens

## E1. Remove static fake integer identity as authorization

File:

```text
OptiScaler/inputs/FG/FfxApi_Dx12_FG.h
```

Remove authorization dependence on:

```cpp
const size_t scContext = 0x13375CC;
const size_t fgContext = 0x133757C;
```

They may remain temporarily only if required by ABI diagnostics, but destroy logic must not use them as current-generation proof.

---

## E2. Use an owned token type

Recommended implementation local to `FfxApi_Dx12_FG.cpp`:

```cpp
enum class FfxFakeContextKind : uint8_t
{
    FrameGeneration,
    Swapchain,
};

struct FfxFakeContextToken
{
    FfxFakeContextKind kind;
    uint64_t serial;
    uint64_t fgSwapchainGeneration;
};

static std::mutex _fakeContextMutex;
static std::unordered_set<FfxFakeContextToken*> _ownedFakeContexts;
static std::atomic_uint64_t _nextFakeContextSerial { 0 };
static FfxFakeContextToken* _activeFgContextToken = nullptr;
static FfxFakeContextToken* _activeSwapchainContextToken = nullptr;
```

Why keep an owned-pointer registry:

`ffxDestroyContext_Dx12FG()` can receive contexts that belong to the real FFX passthrough path. Do not dereference an arbitrary external pointer and assume it is an OptiScaler token.

First prove pointer identity exists in `_ownedFakeContexts`, then dereference.

---

## E3. Token creation helper

Example:

```cpp
static FfxFakeContextToken* CreateFakeContextToken(FfxFakeContextKind kind)
{
    auto& state = State::Instance();

    auto* token = new FfxFakeContextToken {
        kind,
        _nextFakeContextSerial.fetch_add(1, std::memory_order_relaxed) + 1,
        state.currentFGSwapchainGeneration.load(std::memory_order_acquire),
    };

    std::scoped_lock lock(_fakeContextMutex);
    _ownedFakeContexts.insert(token);

    if (kind == FfxFakeContextKind::Swapchain)
        _activeSwapchainContextToken = token;
    else
        _activeFgContextToken = token;

    return token;
}
```

Do not delete the previous token merely because a new token becomes active. The caller may still legally issue a delayed destroy for the old token.

The old token becomes **stale but still owned** until its destroy arrives.

---

## E4. Use tokens on create

### FG context

Replace:

```cpp
*context = (ffxContext) fgContext;
```

with:

```cpp
auto* token = CreateFakeContextToken(FfxFakeContextKind::FrameGeneration);
*context = reinterpret_cast<ffxContext>(token);
```

### swapchain wrap/new/for-HWND

After the swapchain operation has succeeded and `currentFG/currentFGSwapchain` are valid:

```cpp
auto* token = CreateFakeContextToken(FfxFakeContextKind::Swapchain);
*context = reinterpret_cast<ffxContext>(token);
```

Do not publish a token before the underlying operation succeeds.

---

## E5. Safe token lookup

Example:

```cpp
static FfxFakeContextToken* FindOwnedFakeContext(ffxContext value)
{
    auto* candidate = reinterpret_cast<FfxFakeContextToken*>(value);

    std::scoped_lock lock(_fakeContextMutex);
    return _ownedFakeContexts.contains(candidate) ? candidate : nullptr;
}
```

Do not dereference `candidate` before membership is proven.

---

## E6. Stale swapchain destroy must be a no-op for current lifecycle

Required destroy semantics:

```cpp
auto* token = FindOwnedFakeContext(*context);
if (token == nullptr)
    return PASSTHRU_RETURN_CODE;

if (token->kind == FfxFakeContextKind::Swapchain)
{
    bool isCurrent = false;
    {
        std::scoped_lock lock(_fakeContextMutex);
        isCurrent = token == _activeSwapchainContextToken;
    }

    if (!isCurrent)
    {
        LOG_INFO("[FFX][Lifecycle] action = stale_swapchain_context_destroy_ignored, serial = {}, generation = {}",
                 token->serial, token->fgSwapchainGeneration);

        DestroyOwnedFakeContextToken(token);
        *context = nullptr;
        return FFX_API_RETURN_OK;
    }

    // Current token: perform existing preserve/release policy.
    // If ReleaseSwapchain fails, return error and RETAIN the token so the
    // lifecycle is still represented and a retry remains possible.
}
```

On successful current swapchain destroy:

```text
clear active pointer if still token
remove token from registry
delete token
set *context = nullptr
return OK
```

On P3 fail-closed `ReleaseSwapchain()` failure:

```text
do not clear active token
do not delete token
do not clear current generation
return FFX error
```

This preserves fail-closed semantics.

---

## E7. FG context delayed destroy gets equivalent stale protection

If an old FG token is not `_activeFgContextToken`, destroy only the token and return OK.

Do not call `DestroyFGContext()` for a stale token.

Only the active FG token may destroy the current FG context.

After active destroy:

```text
clear active FG token
remove/delete token
set *context = nullptr
```

---

# 9. Part F — Harden legacy FSR3 context identity

The old FSR3 API stores a small integral marker in `FfxFrameInterpolationContext::data[0]`. Use a monotonic per-create generation marker instead of the single constant `0x1337`.

Recommended local state:

```cpp
static std::atomic_uint32_t _nextLegacyFgContextToken { 0x1337 };
static std::atomic_uint32_t _activeLegacyFgContextToken { 0 };
```

Create:

```cpp
uint32_t token = _nextLegacyFgContextToken.fetch_add(1, std::memory_order_relaxed) + 1;
if (token == 0)
    token = _nextLegacyFgContextToken.fetch_add(1, std::memory_order_relaxed) + 1;

_activeLegacyFgContextToken.store(token, std::memory_order_release);

*context = {};
context->data[0] = token;
```

Destroy:

```cpp
const uint32_t token = static_cast<uint32_t>(context->data[0]);
const uint32_t active = _activeLegacyFgContextToken.load(std::memory_order_acquire);

if (token == 0)
    return Fsr3::FFX_OK;

if (token != active)
{
    LOG_INFO("[FSR3][Lifecycle] action = stale_fg_context_destroy_ignored, token = {}, active = {}",
             token, active);
    context->data[0] = 0;
    return Fsr3::FFX_OK;
}

if (State::Instance().currentFG != nullptr)
    State::Instance().currentFG->DestroyFGContext();

uint32_t expected = token;
_activeLegacyFgContextToken.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
context->data[0] = 0;
return Fsr3::FFX_OK;
```

Use the actual width/type of `context->data[0]` from the bundled FSR3 header. Do not truncate a wider token incorrectly.

This is an input-side identity guard only. Do not redesign the FSR3 dispatch protocol.

---

# 10. Generation clearing rules

A generation counter is useful only if clearing is equally strict.

## 10.1 Clear current generation only after successful current-lifecycle teardown

Do not clear it:

```text
on stale wrapper release
on stale FFX context destroy
on failed XeFG Destroy
on warning/ambiguous XeFG Destroy
on failed recreation
on unrelated wrapper destruction
```

Clear only when the code has proved it is completing teardown for the current published proxy/generation.

Example helper if useful:

```cpp
static void ClearFGGenerationIfCurrent(IUnknown* expectedProxy, uint64_t expectedGeneration)
{
    auto& state = State::Instance();

    if (state.currentFGSwapchain == expectedProxy &&
        state.currentFGSwapchainGeneration.load(std::memory_order_acquire) == expectedGeneration)
    {
        state.currentFGSwapchain = nullptr;
        state.currentFGSwapchainGeneration.store(0, std::memory_order_release);
    }
}
```

Do not clear a newer generation.

---

# 11. Do not regress P3 final-proxy protection

Current release/0.9 P3 contains a stronger stale final-proxy guard than master.

Conceptually:

```cpp
if (state.currentFGSwapchain != finalProxy)
{
    // stale old public proxy final release
    releaseFinalProxyOnce();
    return true;
}
```

PR26 must not weaken this behavior.

Wrapper generation logic is an additional authorization layer for a different entry path; it is not a replacement for the P3 final-proxy identity guard.

---

# 12. Preserve fail-closed XeFG semantics

Do not alter these P3 rules:

```text
exact XEFG_SWAPCHAIN_RESULT_SUCCESS is the only lifecycle success
negative Destroy result: context retained + recreation blocked
positive warning/ambiguous Destroy result: context quarantined + recreation blocked
XeLL destroyed only after exact XeFG Destroy success
ReleaseSwapchain propagates failure
creation aborts while lifecycle transaction/recreation quarantine is active
```

If a new generation/token cleanup path encounters `ReleaseSwapchain() == false`, it must preserve the current authorization token/generation rather than pretending teardown completed.

---

# 13. Required static audits

After implementation, run searches over the release/0.9 branch.

## 13.1 No wrapped-swapchain force drain

Search:

```text
currentWrappedSwapchain->Release
0xffffff00
while (refCount
```

Expected:

```text
No Release-until-zero path using State::currentWrappedSwapchain.
```

Any remaining match must be reviewed manually and justified by explicit ownership.

---

## 13.2 No factory use after QI reference release in touched paths

Search around:

```text
QueryInterface(IID_PPV_ARGS(&factory
QueryInterface(IID_PPV_ARGS(&df
factory->Release
 df->Release
```

Expected in touched FFX/FSR3 create paths:

```text
ComPtr/RAII lifetime extends through the factory call.
```

---

## 13.3 No static fake-context authorization

Search:

```text
0x13375CC
0x133757C
0x1337
(void*) scContext == *context
(void*) fgContext == *context
fgContext == context->data[0]
```

Expected:

```text
No current-generation destroy authorization based solely on one process-global constant.
```

A constant may remain only for unrelated compatibility or debugging and must not authorize teardown.

---

## 13.4 Generation writes

Search:

```text
currentFGSwapchainGeneration
nextFGSwapchainGeneration
```

Review every write.

Expected writers should be limited to:

```text
successful new FG proxy publication
successful teardown of the exact current generation
process-shutdown/reset-only exceptional handling if explicitly justified
```

---

# 14. Build and formatting validation

Required before review:

```text
git diff --check
clang-format check
Release|x64 build
```

Do not mark ready for merge with formatting or compile failures.

No unrelated code-format sweep.

---

# 15. Runtime validation matrix

PR26 changes ownership and delayed-destroy behavior, so runtime validation is more important than PR25.

## 15.1 Primary REF + XeFG validation

Preferred games:

```text
Monster Hunter Wilds
Dragon's Dogma 2
```

Setup:

```text
OptiScaler as dxgi.dll
fork REFramework as dinput8.dll
Intel XeFG output
Special K absent
```

Actions:

```text
cold launch
load into gameplay
open/close REFramework overlay
Alt+Tab repeatedly
windowed <-> borderless/fullscreen where supported
resolution change / ResizeBuffers
menu/title transitions that recreate swapchain
normal game exit
```

Expected:

```text
no fatal D3D E_ABORT introduced
no hang during swapchain recreation
no Release-until-zero log storm
no stale wrapper destroying the newly-created XeFG lifecycle
no P3 recreate quarantine regression
```

---

## 15.2 FFX API input -> XeFG validation

This is mandatory for PR26 because the highest-risk ownership changes are in the FFX input adapter.

Use a game/path that exercises:

```text
FSR 3.1 / FFX API frame generation input
XeFG output
```

Exercise:

```text
context create
swapchain create/wrap
FG enable/disable
resize
swapchain recreation
context destroy
recreate context
normal exit
```

Important delayed-destroy test if a synthetic/unit harness is practical:

```text
create context A
create context B
call destroy(A)
verify current lifecycle B remains active
call destroy(B)
verify B tears down normally
```

---

## 15.3 legacy FSR3 input -> XeFG validation

Exercise at least:

```text
legacy frame-interpolation context create
replacement swapchain creation
FG context recreation
destroy old/current context
normal exit
```

Synthetic expectation:

```text
create legacy FG context token A
create token B
destroy A
=> B remains active
destroy B
=> current FG context is destroyed
```

---

## 15.4 REF-held old wrapper reproduction

If a deterministic harness is available, explicitly validate the scenario PR26 is designed to solve:

```text
wrapper A created for HWND X
external observer holds A reference
new FG lifecycle B published for HWND X
A reaches final Release later
```

Expected log:

```text
stale_wrapped_release_skipped
```

and lifecycle B remains valid.

---

# 16. Logging requirements

Keep logs event-oriented and low volume.

Useful one-shot lifecycle diagnostics:

```text
[FG][Ownership] action = detach_wrapped_alias
[FG][Lifecycle] action = publish_generation
[FG][Lifecycle] action = stale_wrapped_release_skipped
[FFX][Lifecycle] action = stale_swapchain_context_destroy_ignored
[FFX][Lifecycle] action = stale_fg_context_destroy_ignored
[FSR3][Lifecycle] action = stale_fg_context_destroy_ignored
```

Do not add per-frame generation logs.

Include pointer/generation/serial/HWND only where useful for proving ownership transitions.

---

# 17. Unit/static test recommendations

If the existing test environment can host isolated lifecycle tests without pulling in full DXGI runtime, add focused tests for helper logic.

Suggested cases:

```text
Detach alias does not call Release
stale wrapper generation cannot authorize FG teardown
current wrapper generation can authorize teardown
newer generation is not cleared by old-generation completion
old FFX token destroy does not call current ReleaseSwapchain
ReleaseSwapchain failure retains active FFX token
old FFX FG token destroy does not call DestroyFGContext
legacy FSR3 old token destroy is ignored
```

Do not create a large new testing framework solely for PR26.

---

# 18. Explicit non-goals

Do not include any of the following in this PR:

```text
PR17 global skipReleaseChecks redesign
PR18 thread-aware OwnedMutex experiment
PR19 diagnostics / crash trace work
PR20/PR21 Subnautica diagnostics
XeFG geometry/tag extent investigation
FSRFG rendering corruption fixes
Streamline frame ID redesign
new XeLL/fakenvapi architecture from master
Reflex selective-DXGI changes already completed in PR25
menu/config/UI changes
CI/release workflow changes
```

Also do not "clean up" every premature QI Release in the entire repository. PR26 should fix the two remaining factory uses in the FFX/FSR3 paths it already touches.

---

# 19. Review checklist

A reviewer should be able to answer **yes** to all of these before merge:

```text
[ ] No State::currentWrappedSwapchain Release-until-zero remains in FfxApi/FSR3 replacement paths.
[ ] Alias detach never consumes an unknown COM reference.
[ ] FfxApi IDXGIFactory2 stays alive through CreateSwapChainForHwnd.
[ ] legacy FSR3 IDXGIFactory2 stays alive through CreateSwapChainForHwnd.
[ ] A monotonic FG public-proxy generation is published only on successful new proxy creation.
[ ] Wrapper records generation metadata without taking COM ownership.
[ ] Old wrapper final Release cannot destroy a newer FG lifecycle on the same HWND.
[ ] release/0.9's existing exact proxy identity clear guard is preserved.
[ ] FfxApi swapchain fake context is unique per create.
[ ] FfxApi FG fake context is unique per create.
[ ] Stale FfxApi destroy removes only the stale token.
[ ] Failed current ReleaseSwapchain retains the active token and fail-closed state.
[ ] Legacy FSR3 FG context uses per-create identity.
[ ] P3 exact-success/quarantine behavior is unchanged.
[ ] No newer generation can be cleared by an older teardown completion.
[ ] git diff --check passes.
[ ] clang-format passes.
[ ] Release|x64 build passes.
```

---

# 20. Suggested commit structure

Keep implementation reviewable with 2-4 commits before squash.

Suggested sequence:

```text
1. fix: remove FFX/FSR3 wrapped swapchain force drains
2. fix: guard wrapper teardown with FG lifecycle generation
3. fix: make FFX and legacy FSR3 fake contexts generation-aware
4. fix: retain FFX/FSR3 factory QI refs through use
```

The exact ordering may change if compile dependencies make another sequence easier.

Final PR should be squash-merged after review.

---

# 21. Acceptance criteria

PR26 is complete when all of the following hold:

```text
Ownership:
- tracking aliases are never used as permission to drain COM refs
- wrapper-owned `_real` reference is still released exactly once on wrapper destruction

Generation safety:
- stale wrapper cannot teardown current FG lifecycle
- stale FFX swapchain context cannot teardown current FG lifecycle
- stale FFX FG context cannot destroy current FG context
- stale legacy FSR3 FG context cannot destroy current FG context

Fail-closed lifecycle:
- current-generation XeFG release failure remains visible to the caller
- failed/quarantined destroy cannot silently advance/clear generation state

Compatibility:
- P1/P2/P3 behavior remains intact
- PR25 Reflex/selective-DXGI behavior remains untouched
- master-only architecture changes are not imported

Validation:
- clean diff/build/format
- REF + XeFG runtime smoke
- FFX API -> XeFG recreation/destroy validation
- legacy FSR3 -> XeFG context recreation validation
```

Once this PR is merged and runtime validation is clean, the release/0.9 REF/XeFG ownership/lifecycle compatibility pass can be considered substantially complete. Remaining PR17/18/19-style concurrency experiments should stay separate unless runtime evidence demonstrates they are still required.
