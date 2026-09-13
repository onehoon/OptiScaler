# Work Order: `release/0.9` REFramework Compatibility P3 — Fail-Closed XeFG Lifecycle

**Repository:** `onehoon/OptiScaler`  
**Work-order branch:** `master`  
**Fork `master` HEAD reviewed:** `8d5a546c4cd50c793921e11cdf4b71b286f396d7`  
**Fork `master` lifecycle-code baseline reviewed:** `7ee16d9b8b5405451322a840e1adbf1efe24dcf1`  
**Target long-lived branch:** `reframework-0.9`  
**Target baseline after P1 + P2:** `cc8645c8c9ccf9ff89fca907f06b772763ee2da3`  
**Implementation branch to create:** `feature/reframework-0.9-p3-fail-closed-xefg-lifecycle`  
**Date:** 2026-09-13

Related documents:

```text
doc/RELEASE_0_9_REF_COMPATIBILITY_BACKPORT_ANALYSIS_AND_PLAN_2026-09-13.md
doc/work-order/RELEASE_0_9_REF_COMPAT_P1_BACKBUFFER_OWNERSHIP_WORK_ORDER_2026-09-13.md
doc/work-order/RELEASE_0_9_REF_COMPAT_P2_OWNER_SCOPED_SWAPCHAIN_CLEANUP_WORK_ORDER_2026-09-13.md
```

Completed dependency chain:

```text
upstream release/0.9 @ 132bc110f371d273834681ae05c73db4212bd337
    -> P1 / PR #22 @ 8efe12389d93a2652eb8266068cd80f2e76f0891
    -> P2 / PR #23 @ cc8645c8c9ccf9ff89fca907f06b772763ee2da3
    -> P3 (this work order)
```

Master references used for lifecycle intent:

- merged PR #1 — XeFG destroy/recreation fail-closed lifecycle work;
- merged PR #14 — exact-success semantic correction and warning/error distinction;
- current fork `master` final lifecycle code;
- the dedicated `release/0.9` backport analysis document above.

---

# 1. Objective

Implement the final P3 lifecycle stage on top of the P1 + P2 `reframework-0.9` baseline.

P3 has one central invariant:

> **A failed or ambiguous XeFG teardown must never be reported as a successful lifecycle transition, and a replacement XeFG context must never be created over a previous lifecycle that did not complete exactly.**

The target state machine is:

```text
old XeFG lifecycle exists
        |
        v
release / destroy transaction
        |
        +-- exact XEFG_SWAPCHAIN_RESULT_SUCCESS
        |       -> commit teardown
        |       -> release 0.9-owned XeLL state in the expected order
        |       -> allow replacement creation
        |
        +-- warning / ambiguous non-success
        |       -> do NOT commit successful lifecycle state
        |       -> quarantine recreation
        |       -> do NOT pretend outputs/state are valid
        |
        +-- negative failure
                -> retain old XeFG context identity
                -> retain dependent lifetime state
                -> propagate failure
                -> block replacement creation
```

P3 is specifically about XeFG swapchain/context lifecycle correctness and caller failure propagation.

It is **not** a general synchronization rewrite and **not** a master-architecture backport.

---

# 2. Branch procedure

Start from the current long-lived compatibility branch after P2.

Expected base at work-order creation time:

```text
reframework-0.9
cc8645c8c9ccf9ff89fca907f06b772763ee2da3
```

Suggested commands:

```bash
git fetch origin

git switch reframework-0.9
git pull --ff-only origin reframework-0.9

git rev-parse HEAD
# Expected at work-order creation time:
# cc8645c8c9ccf9ff89fca907f06b772763ee2da3

git switch -c feature/reframework-0.9-p3-fail-closed-xefg-lifecycle
```

Open the implementation PR with:

```text
base: reframework-0.9
head: feature/reframework-0.9-p3-fail-closed-xefg-lifecycle
```

If `reframework-0.9` has legitimately advanced after this document was written, inspect the intervening commits first. Do not reset or force-move the long-lived branch solely to reproduce the recorded SHA.

Do not merge `master` into this implementation branch.

Do not cherry-pick master PR #1 or PR #14 mechanically.

The rule remains:

> **Backport lifecycle invariants, not master architecture.**

---

# 3. Files in scope

## Primary implementation files

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
```

## Caller files expected to require narrow changes

```text
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
```

## Audit result: not present on the target branch

The master planning document also names:

```text
OptiScaler/with_dx12/dx11_with_dx12_sc.cpp
```

That path is not present in the current `reframework-0.9` branch, so there is nothing to port there for P3.

Do not add the master file or its architecture merely to mirror master.

## Reference-only unless a concrete compile/lifecycle issue proves otherwise

```text
OptiScaler/State.h
OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp
OptiScaler/proxies/XeFG_Proxy.h
OptiScaler/proxies/XeLL_Proxy.h
OptiScaler/framegen/IFGFeature_Dx12.*
```

---

# 4. Important master-vs-0.9 comparison findings

The current master and `reframework-0.9` differ substantially around lifecycle handling.

The implementation must account for these exact differences rather than copying master blocks blindly.

## 4.1 `XeFG_Dx12.h`: 0.9 has no lifecycle transaction state

Current master has lifecycle-specific state such as:

```cpp
std::atomic_bool _swapchainReleaseInProgress { false };
std::atomic<DWORD> _swapchainReleaseOwnerThread { 0 };
std::mutex _swapchainLifecycleMutex;
bool _swapchainRecreationBlocked = false;
```

and helpers such as:

```cpp
bool AbortSwapchainInitialization(const char* stage);
bool ReleaseSwapchainLocked(HWND hwnd, std::function<void()> releaseFinalProxy = {});
bool ReleaseSwapchainFromFinalProxyRelease(HWND hwnd, std::function<void()> releaseFinalProxy);
```

Current `reframework-0.9` has none of these.

P3 needs lifecycle serialization and recreation quarantine, but it must remain local to XeFG lifecycle mutation.

## 4.2 `CreateSwapchainContext()` has a serious bool/enum return bug in 0.9

The target branch currently has code equivalent to:

```cpp
auto result = XeFGProxy::D3D12CreateContext()(device, &_swapChainContext);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    LOG_ERROR(...);
    return result;
}
```

The function returns `bool`, while `result` is an XeFG result enum.

That is unsafe.

Any non-zero enum value can convert to `true` in C++.

Therefore a failed XeFG API call can be reported to its caller as successful merely because the vendor result is non-zero.

The same bad pattern appears in later failure paths, including `SetLatencyReduction`.

P3 must replace failure returns from this bool function with explicit:

```cpp
return false;
```

Never return an XeFG result enum directly from a `bool` lifecycle helper.

## 4.3 0.9 can report XeLL setup success when no XeLL context exists

The current 0.9 flow is approximately:

```text
XeLLProxy::CreateContext(device)
if (XeLLProxy::Context() != nullptr)
    -> SetSleepMode
    -> fakenvapi::setModeAndContext
    -> XeFG SetLatencyReduction

createResult = true
```

There is no required failure branch when `XeLLProxy::Context()` remains null.

For P3, the existing 0.9 XeLL architecture must be preserved, but absence of the required XeLL context must cause `CreateSwapchainContext()` to fail rather than fall through to success.

Do **not** replace the 0.9 XeLL/fakenvapi architecture with master LOW_LATENCY_INPUTS/InputXeLL architecture.

## 4.4 `DestroySwapchainContext()` restores a failed handle but always returns true

Current `reframework-0.9` behavior is approximately:

```cpp
auto context = _swapChainContext;
_swapChainContext = nullptr;

auto result = XeFGProxy::Destroy()(context);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _swapChainContext = context;
}
else
{
    // success cleanup
}

return true;
```

The local handle restoration recognizes failure, but the function return value discards that fact.

This is the central P3 correctness defect.

## 4.5 `ReleaseSwapchain()` ignores Destroy failure and then destroys dependent state anyway

The current target branch performs:

```cpp
if (_swapChainContext != nullptr)
    DestroySwapchainContext();

_swapChainContext = nullptr;
...
ReleaseObjects();
XeLLProxy::DestroyXeLLContext();
...
return true;
```

Therefore even when `DestroySwapchainContext()` restored the old context after failure, `ReleaseSwapchain()` immediately overwrites the handle with null, tears down dependent objects/XeLL, and reports success.

This converts a known failure into apparent success and enables a replacement context to be created over unresolved vendor state.

P3 must stop the successful-release tail on Destroy failure.

## 4.6 Both Create paths ignore previous-release failure

Current 0.9 recreate logic contains:

```cpp
LOG_INFO("Releasing old swapchain");
ReleaseSwapchain(_hwnd);
```

in both:

```text
CreateSwapchain(...)
CreateSwapchain1(...)
```

The return value is ignored.

P3 must make recreation conditional on successful previous teardown.

## 4.7 Partial initialization can be mistaken for a reusable valid context

After context creation, the current 0.9 code can fail at:

```text
GetProperties
D3D12InitFromSwapChainDesc
D3D12GetSwapChainPtr
```

without consistently destroying/quarantining the partially initialized XeFG context.

A later create can then see `_swapChainContext != nullptr` and skip context creation, incorrectly reusing half-initialized state.

## 4.8 `FG_Hooks` marks `oldSwapChain` before replacement creation has succeeded

Current 0.9 create hooks assign:

```cpp
oldSwapChain = State::Instance().currentFGSwapchain;
```

before calling `fg->CreateSwapchain(...)` / `CreateSwapchain1(...)`.

After P3, creation is intentionally allowed to fail when previous teardown is incomplete.

If `oldSwapChain` is assigned before that failure, the still-live current proxy can later enter:

```cpp
if (This == oldSwapChain)
    return 0;
```

and have legitimate Release calls suppressed.

P3 must update `oldSwapChain` only after successful replacement creation.

## 4.9 `hkFGRelease()` ignores lifecycle failure

Current P2-complete 0.9 behavior does the ownership cleanup correctly, but it still performs:

```cpp
State::Instance().currentFG->ReleaseSwapchain(_hwnd);
```

without checking success, and then clears public-proxy tracking aliases.

P3 must not clear/commit the normal success path when XeFG release failed.

## 4.10 Wrapper final release ignores lifecycle failure

After P2, wrapper-owned COM cleanup is correct, but the wrapper still performs roughly:

```cpp
fg->Deactivate();
fg->ReleaseSwapchain(_handle);

if (state.currentFGSwapchain != nullptr)
    state.currentFGSwapchain = nullptr;
```

without checking the release result.

P3 must preserve the P2 wrapper/real ownership changes while making FG-proxy clearing conditional on successful lifecycle teardown.

## 4.11 FFX API destroy caller also ignores release failure

`ffxDestroyContext_Dx12FG()` currently calls:

```cpp
State::Instance().currentFG->ReleaseSwapchain(...);
```

and then continues its normal destroy path.

P3 must return an error if the lifecycle release did not complete.

The separate FFX wrapper-release drain that exists in this callback is **not** the target of P3 and must not be opportunistically redesigned in this PR.

---

# 5. Hard semantic guardrail: exact XeFG success

The earlier master history briefly treated non-negative XeFG results as broad success.

Do not reproduce that policy.

For P3 lifecycle/state/output commits, exact success means:

```cpp
result == XEFG_SWAPCHAIN_RESULT_SUCCESS
```

Use exact success for at least:

```text
D3D12CreateContext
SetLatencyReduction
Destroy
GetProperties output commit
D3D12InitFromSwapChainDesc
D3D12GetSwapChainPtr
SetEnabled state transitions
resource-tagging paths whose existing recovery depends on exact success
```

The target 0.9 branch already uses exact-success comparisons in many of these paths. Preserve that behavior.

Do **not** add blanket logic such as:

```cpp
static_cast<int32_t>(result) >= 0
```

A positive warning may be logged differently from a negative error, but it is not proof that a lifecycle/state/output transition completed exactly.

---

# 6. Warning semantics: fail closed, but do not blindly reuse a possibly destroyed handle

The original conceptual backport plan says to retain the old context on non-success.

Current master PR #14 refined one important corner case:

```text
negative error
    -> old handle is considered still live enough to retain
    -> retain `_swapChainContext = context`
    -> block recreation

positive warning / ambiguous non-success
    -> do NOT call it success
    -> do NOT assume the old handle is safe to reuse either
    -> quarantine recreation
    -> leave `_swapChainContext == nullptr`
```

This is the safer final policy for P3.

The key invariant is not “always retain a raw handle”; the key invariant is:

> **Never create a replacement after any non-exact Destroy result.**

Recommended local classifier:

```cpp
static bool IsXeFGWarning(xefg_swapchain_result_t result)
{
    return static_cast<int32_t>(result) > 0;
}
```

This classifier is only for logging/quarantine handling.

It must never be used to turn a warning into successful lifecycle completion.

---

# Task A — Add narrow lifecycle state to `XeFG_Dx12`

## 7. Header changes

Add only lifecycle-local synchronization/state required by this P3 transaction model.

Recommended includes:

```cpp
#include <atomic>
#include <functional>
#include <mutex>
```

Recommended members, adapted from the proven master lifecycle design:

```cpp
std::atomic_bool _swapchainReleaseInProgress { false };
std::atomic<DWORD> _swapchainReleaseOwnerThread { 0 };
std::mutex _swapchainLifecycleMutex;
bool _swapchainRecreationBlocked = false;
```

Recommended helpers:

```cpp
bool AbortSwapchainInitialization(const char* stage);
bool DestroySwapchainContext();
bool ReleaseSwapchainLocked(HWND hwnd, std::function<void()> releaseFinalProxy = {});
```

Public helper for the final-public-proxy Release hook:

```cpp
bool ReleaseSwapchainFromFinalProxyRelease(HWND hwnd, std::function<void()> releaseFinalProxy);
```

Lifecycle query used only to avoid same-thread release recursion while a XeFG release transaction is already executing:

```cpp
bool SwapchainReleaseOwnedByCurrentThread() const noexcept
{
    return _swapchainReleaseInProgress.load(std::memory_order_acquire) &&
           _swapchainReleaseOwnerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
}
```

### Important scope boundary

Do **not** convert the existing global FG Present mutex system or owner-tag mechanism as part of this task.

Do not port PR #18's broader thread-aware Present mutex changes.

Do not add PR #19 tracing.

The state above exists only to serialize/identify XeFG create/release lifecycle transactions.

---

# Task B — Make `CreateSwapchainContext()` report real bool success

## 8. Replace enum-to-bool failure returns

Every failure branch in this `bool` function must explicitly return `false`.

Wrong:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    LOG_ERROR(...);
    return result;
}
```

Required:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    LOG_ERROR(...);
    return false;
}
```

Do this for:

```text
D3D12CreateContext failure
XeLL SetSleepMode failure
XeFG SetLatencyReduction failure
any other failure path in this bool helper
```

## 9. Treat missing XeLL context as creation failure

Preserve the 0.9 architecture:

```text
XeLLProxy::CreateContext
    -> XeLL SetSleepMode
    -> fakenvapi::setModeAndContext
    -> XeFG SetLatencyReduction
```

But require the expected XeLL context to exist.

Recommended shape:

```cpp
XeLLProxy::CreateContext(device);

if (XeLLProxy::Context() != nullptr)
{
    xell_sleep_params_t sleepParams {};
    sleepParams.bLowLatencyMode = true;
    sleepParams.bLowLatencyBoost = false;
    sleepParams.minimumIntervalUs = 0;

    const auto xellResult = XeLLProxy::SetSleepMode()(XeLLProxy::Context(), &sleepParams);
    if (xellResult != XELL_RESULT_SUCCESS)
    {
        LOG_ERROR("SetSleepMode error: {} ({})", magic_enum::enum_name(xellResult), (UINT) xellResult);
        return false;
    }

    const auto fnaResult = fakenvapi::setModeAndContext(XeLLProxy::Context(), Mode::XeLL);
    LOG_DEBUG("fakenvapi::setModeAndContext: {}", fnaResult);

    const auto result = XeFGProxy::SetLatencyReduction()(_swapChainContext, XeLLProxy::Context());
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("SetLatencyReduction error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        return false;
    }
}
else
{
    LOG_ERROR("Couldn't create XeLL");
    return false;
}

return true;
```

Do not replace this block with master `InputXeLL` / `LOW_LATENCY_INPUTS` architecture.

---

# Task C — Add partial-initialization cleanup

## 10. Add `AbortSwapchainInitialization(stage)`

Recommended intent:

```cpp
bool XeFG_Dx12::AbortSwapchainInitialization(const char* stage)
{
    if (_swapChainContext == nullptr)
        return false;

    LOG_ERROR("[XeFG][Lifecycle] action=init_aborted stage={} context={:X}",
              stage, (size_t) _swapChainContext);

    if (!DestroySwapchainContext())
    {
        LOG_ERROR("[XeFG][Lifecycle] action=init_cleanup_failed stage={} context={:X}",
                  stage, (size_t) _swapChainContext);
    }

    return false;
}
```

The helper always returns false because it is used from failure paths.

Its Destroy call must obey the same fail-closed policy as normal teardown.

## 11. Use the helper after context creation fails midway

Both Create entry points must route post-context failures through cleanup.

At minimum include:

```text
CreateSwapchainContext failure after a context handle may have been produced
GetProperties non-exact-success
D3D12InitFromSwapChainDesc non-exact-success
D3D12GetSwapChainPtr non-exact-success
```

Also audit other `return false` paths after `_swapChainContext` has been created, such as factory interface acquisition failure.

If the function is abandoning a newly created/initializing XeFG lifecycle, do not leave the context silently resident.

### `GetProperties` is intentionally stricter than current master

Current master logs a `GetProperties` failure and may continue.

The dedicated `release/0.9` backport plan intentionally requires fail-closed partial initialization.

For this backport use:

```cpp
xefg_swapchain_properties_t props {};
auto result = XeFGProxy::GetProperties()(_swapChainContext, &props);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    LOG_ERROR(...);
    return AbortSwapchainInitialization("GetProperties");
}

State::Instance().xefgMaxInterpolationCount = props.maxSupportedInterpolations;
```

Do not commit `props` output on a warning or error.

## 12. Retrieve the public swapchain through a local pointer

Do not pass the caller's output directly to `D3D12GetSwapChainPtr` on a failing path.

Recommended 0.9-adapted form:

```cpp
IDXGISwapChain* queriedSwapChain = nullptr;
result = XeFGProxy::D3D12GetSwapChainPtr()(_swapChainContext, IID_PPV_ARGS(&queriedSwapChain));

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    SAFE_RELEASE(queriedSwapChain);
    return AbortSwapchainInitialization("D3D12GetSwapChainPtr");
}

*swapChain = queriedSwapChain;
```

Use `IDXGISwapChain1*` in `CreateSwapchain1()`.

This prevents a non-exact result from leaking or exposing a partially returned COM output.

---

# Task D — Make Destroy fail closed

## 13. `DestroySwapchainContext()` target behavior

Preserve the existing 0.9 XeLL lifetime ordering while adopting the final fail-closed result policy.

Recommended shape:

```cpp
bool XeFG_Dx12::DestroySwapchainContext()
{
    if (_swapChainContext == nullptr || State::Instance().isShuttingDown)
        return true;

    auto context = _swapChainContext;
    _swapChainContext = nullptr;

    LOG_INFO("[XeFG][Lifecycle] action=destroy_begin context={:X}", (size_t) context);

    const auto result = XeFGProxy::Destroy()(context);

    LOG_INFO("[XeFG][Lifecycle] action=destroy_return context={:X} result={} ({})",
             (size_t) context, magic_enum::enum_name(result), static_cast<int32_t>(result));

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        _swapchainRecreationBlocked = true;

        if (IsXeFGWarning(result))
        {
            // Ambiguous: never call it success, but do not reuse a handle the SDK
            // may already have invalidated.
            _swapChainContext = nullptr;
            LOG_WARN("[XeFG][Lifecycle] action=destroy_warning_quarantined context={:X} retained=false",
                     (size_t) context);
        }
        else
        {
            // Negative failure: preserve the known old lifecycle identity.
            _swapChainContext = context;
            LOG_ERROR("[XeFG][Lifecycle] action=destroy_failed context={:X} retained=true",
                      (size_t) context);
        }

        return false;
    }

    // release/0.9-specific lifetime rule:
    // XeLL remains alive through the XeFG Destroy call and is destroyed only
    // after exact successful XeFG teardown.
    if (XeLLProxy::Context() != nullptr)
        XeLLProxy::DestroyXeLLContext();

    _swapchainRecreationBlocked = false;
    State::Instance().currentFGSwapchain = nullptr;
    return true;
}
```

### Hard requirements

On non-exact Destroy:

```text
return false
set recreation blocked
never execute normal success cleanup
never destroy the 0.9 XeLL context as if teardown succeeded
never create a replacement lifecycle
```

On exact success:

```text
Destroy XeFG first
then destroy the release/0.9 XeLL context
then unblock recreation
then commit tracking-state cleanup
```

## 14. Remove unconditional XeLL teardown from successful-tail assumptions

Current 0.9 `ReleaseSwapchain()` ends with:

```cpp
ReleaseObjects();
XeLLProxy::DestroyXeLLContext();
```

After P3, XeLL destruction must be tied to exact successful XeFG Destroy, not to unconditional release-tail execution.

Do not destroy XeLL after a failed/ambiguous XeFG Destroy.

Avoid double-destroying XeLL by leaving both the Destroy-success path and an unconditional release tail active.

---

# Task E — Serialize create/release lifecycle transactions

## 15. Public `ReleaseSwapchain()`

Acquire the XeFG lifecycle mutex with non-blocking semantics for normal release entry:

```cpp
bool XeFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
    if (!lifecycleLock.owns_lock())
    {
        LOG_WARN("[XeFG][Lifecycle] action=release_swapchain_deferred reason=lifecycle_transaction_in_progress");
        return false;
    }

    return ReleaseSwapchainLocked(hwnd);
}
```

The caller must see false rather than silently running a second overlapping teardown.

## 16. Create paths

Both Create entry points must also acquire the lifecycle mutex with `std::try_to_lock`.

Recommended entry guard:

```cpp
std::unique_lock lifecycleLock(_swapchainLifecycleMutex, std::try_to_lock);
if (!lifecycleLock.owns_lock())
{
    LOG_WARN("[XeFG][Lifecycle] action=recreate_aborted api=CreateSwapchain reason=lifecycle_transaction_in_progress");
    return false;
}

if (_swapchainRecreationBlocked)
{
    LOG_WARN("[XeFG][Lifecycle] action=recreate_aborted api=CreateSwapchain reason=previous_destroy_failed");
    return false;
}
```

Use the equivalent API name in `CreateSwapchain1()`.

## 17. Previous release inside Create must use the already-held transaction

Do not call the public locking `ReleaseSwapchain()` while Create already owns `_swapchainLifecycleMutex`.

Use:

```cpp
if (!ReleaseSwapchainLocked(_hwnd))
{
    LOG_ERROR("[XeFG][Lifecycle] action=recreate_aborted api=CreateSwapchain reason=release_not_completed");
    return false;
}
```

Repeat for `CreateSwapchain1()`.

This is the key replacement for current 0.9's unchecked:

```cpp
ReleaseSwapchain(_hwnd);
```

---

# Task F — Implement `ReleaseSwapchainLocked()` with real failure propagation

## 18. Internal release transaction

The helper must:

1. validate the hwnd;
2. prevent recursive/overlapping release execution;
3. preserve the existing optional `FGUseMutexForSwapchain` behavior where possible;
4. clean up the FG context;
5. optionally consume the final public-proxy COM release at the correct point;
6. call `DestroySwapchainContext()`;
7. stop immediately if Destroy failed;
8. run the normal success tail only after exact successful teardown.

Recommended skeleton:

```cpp
bool XeFG_Dx12::ReleaseSwapchainLocked(HWND hwnd, std::function<void()> releaseFinalProxy)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    if (_swapchainReleaseInProgress.exchange(true, std::memory_order_acq_rel))
    {
        LOG_WARN("[XeFG][Lifecycle] action=release_swapchain_deferred reason=release_already_in_progress");
        return false;
    }

    _swapchainReleaseOwnerThread.store(GetCurrentThreadId(), std::memory_order_release);

    struct ReleaseGuard
    {
        std::atomic_bool& inProgress;
        std::atomic<DWORD>& ownerThread;

        ~ReleaseGuard()
        {
            ownerThread.store(0, std::memory_order_release);
            inProgress.store(false, std::memory_order_release);
        }
    } guard { _swapchainReleaseInProgress, _swapchainReleaseOwnerThread };

    const bool useConfiguredMutex = Config::Instance()->FGUseMutexForSwapchain.value_or_default();

    if (useConfiguredMutex)
    {
        if (Mutex.getOwner() == 1)
        {
            // Current 0.9 incorrectly treats this as successful release.
            LOG_WARN("[XeFG][Lifecycle] action=release_swapchain_deferred reason=release_already_in_progress");
            return false;
        }

        Mutex.lock(1);
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    if (releaseFinalProxy)
    {
        releaseFinalProxy();
        // Do not access the public proxy after this callback.
    }

    if (!State::Instance().isShuttingDown && _swapChainContext != nullptr)
    {
        if (!DestroySwapchainContext())
        {
            if (useConfiguredMutex)
                Mutex.unlockThis(1);

            LOG_ERROR("[XeFG][Lifecycle] action=release_swapchain_aborted reason=destroy_failed context={:X}",
                      (size_t) _swapChainContext);
            return false;
        }
    }

    // Exact-success / shutdown tail only.
    _swapChainContext = nullptr;
    ReleaseObjects();

    if (useConfiguredMutex)
        Mutex.unlockThis(1);

    return true;
}
```

Adapt to existing logging/style and ensure every acquired configured mutex is unlocked on every return path.

### Do not

Do not retain current 0.9 behavior:

```cpp
if (Mutex.getOwner() == 1)
    return true;
```

An already-running release is not proof that this caller's requested lifecycle transition completed.

Return false/deferred instead.

---

# Task G — Coordinate the final hooked public-proxy Release

## 19. Why this needs special handling

`hkFGRelease()` currently adds a temporary COM reference before probing the release count:

```cpp
This->AddRef();
...
if (o_FGRelease(This) == 1)
{
    ...
}
```

When the count reaches the final Opti-held reference, XeFG teardown must consume that reference at a controlled point before vendor Destroy completes.

Do not leave the artificial final reference outstanding across a Destroy transaction and then simply return `0`.

## 20. Add `ReleaseSwapchainFromFinalProxyRelease()`

Use a once-only callback model.

Recommended shape, preserving the P2 owner-domain rule:

```cpp
bool XeFG_Dx12::ReleaseSwapchainFromFinalProxyRelease(HWND hwnd,
                                                       std::function<void()> releaseFinalProxy)
{
    std::unique_lock lifecycleLock(_swapchainLifecycleMutex);

    if (!releaseFinalProxy)
        return false;

    auto& state = State::Instance();
    auto* const finalProxy = state.currentFGSwapchain;

    bool finalProxyReleased = false;
    auto releaseFinalProxyOnce = [&]()
    {
        if (finalProxyReleased)
            return;

        if (state.currentSwapchain == finalProxy)
            state.currentSwapchain = nullptr;

        if (state.currentFGSwapchain == finalProxy)
            state.currentFGSwapchain = nullptr;

        releaseFinalProxy();
        finalProxyReleased = true;
    };

    const bool releaseSucceeded = ReleaseSwapchainLocked(hwnd, releaseFinalProxyOnce);

    if (!finalProxyReleased)
    {
        // A confirmed final COM release cannot be left pending indefinitely.
        // Consume it exactly once, quarantine recreation, and report failure.
        _swapchainRecreationBlocked = true;
        releaseFinalProxyOnce();

        LOG_ERROR("[XeFG][Lifecycle] action=final_proxy_release_quarantined reason=teardown_not_completed");
    }

    return releaseSucceeded;
}
```

### P2 ownership rule still applies

This callback may clear only public-proxy aliases matching `finalProxy`.

It must **not** clear or Release:

```text
currentWrappedSwapchain
currentRealSwapchain
```

Those remain owned by their P2 owner domains.

---

# Task H — Update `FG_Hooks.cpp` without importing PR #17

## 21. Do not mark an old proxy before new creation succeeds

For both Create hook entry points, capture the prior public proxy first:

```cpp
IUnknown* previousFGSwapchain = State::Instance().currentFGSwapchain;
```

Perform the existing GPU-idle wait using this snapshot, but do **not** immediately assign it to `oldSwapChain`.

Before calling Create, clear a stale previous marker if necessary:

```cpp
if (oldSwapChain == previousFGSwapchain)
    oldSwapChain = nullptr;
```

Only after `scResult == true`, compare the old and new proxy identities:

```cpp
IUnknown* newFGSwapchain = ppSwapChain != nullptr
                                ? static_cast<IUnknown*>(*ppSwapChain)
                                : nullptr;

if (previousFGSwapchain != nullptr && previousFGSwapchain != newFGSwapchain)
    oldSwapChain = previousFGSwapchain;
else if (oldSwapChain == newFGSwapchain)
    oldSwapChain = nullptr;
```

Then continue the existing 0.9 success path that sets:

```text
currentFGSwapchain
currentSwapchain
hook state
```

Do not import unrelated master `SetFGSwapchain()` refactors.

## 22. Preserve the current non-thread-local `skipReleaseChecks`

The target branch currently has:

```cpp
static bool skipReleaseChecks = false;
```

Keep it as-is for P3.

Do **not** change it to:

```cpp
static thread_local bool skipReleaseChecks = false;
```

The dedicated backport plan explicitly excludes PR #17's thread-local FG-hook reentrancy experiment.

## 23. Add only the lifecycle-owned-thread reentry check needed by merged P3 logic

When a normal public `ReleaseSwapchain()` transaction is already owned by the current thread, forward a reentrant hooked Release directly to the original function instead of starting another lifecycle transaction:

```cpp
if (State::Instance().activeFgOutput == FGOutput::XeFG &&
    State::Instance().currentFG != nullptr)
{
    auto* xefg = dynamic_cast<XeFG_Dx12*>(State::Instance().currentFG);
    if (xefg != nullptr && xefg->SwapchainReleaseOwnedByCurrentThread())
    {
        LOG_TRACE("[XeFG][Lifecycle] action=fg_release_forwarded reason=same_thread_lifecycle_reentry");
        return o_FGRelease(This);
    }
}
```

This is tied to the P3 lifecycle transaction ownership above.

Do not add broader thread-local Present/release experiments.

## 24. Propagate final proxy lifecycle failure

Keep the P2 alias-ownership behavior, but replace the unchecked XeFG release with the final-proxy helper.

Conceptual 0.9-adapted block:

```cpp
This->AddRef();

auto& state = State::Instance();

if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
{
    const auto releaseResult = o_FGRelease(This);
    if (releaseResult == 1)
    {
        WaitForGPUIdle();

        skipReleaseChecks = true;
        bool releaseSucceeded = true;

        if (state.currentFG != nullptr)
        {
            if (auto* xefg = dynamic_cast<XeFG_Dx12*>(state.currentFG); xefg != nullptr)
            {
                releaseSucceeded = xefg->ReleaseSwapchainFromFinalProxyRelease(
                    _hwnd,
                    [This]() { o_FGRelease(This); });
            }
            else
            {
                releaseSucceeded = state.currentFG->ReleaseSwapchain(_hwnd);
            }
        }

        if (!releaseSucceeded)
        {
            skipReleaseChecks = false;
            LOG_ERROR("[XeFG][Lifecycle] action=fg_release_aborted reason=release_swapchain_failed");
            return 0;
        }

        // Preserve the P2 identity rule. These may already have been cleared by
        // the final-proxy callback; this is intentionally idempotent.
        if (state.currentSwapchain == This)
            state.currentSwapchain = nullptr;

        if (state.currentFGSwapchain == This)
            state.currentFGSwapchain = nullptr;

        skipReleaseChecks = false;
        return 0;
    }
}
```

Do not reintroduce wrapper/real cleanup into this hook.

---

# Task I — Make wrapper cleanup respect lifecycle failure

## 25. Preserve all P2 COM ownership fixes

Do not regress these P2 changes:

```text
currentWrappedSwapchain == this identity cleanup
currentRealSwapchain == real identity cleanup
std::exchange(_real, nullptr)
exactly one real->Release()
no refcount drain loop
```

## 26. Stop clearing FG lifecycle state after failed release

The wrapper may initiate release, but it must observe the boolean result.

Recommended adaptation:

```cpp
auto fg = state.currentFG;
bool releaseCompleted = false;
auto* fgProxyBeforeRelease = state.currentFGSwapchain;

if (fg != nullptr)
{
    if (fg->Mutex.getOwner() == 1)
    {
        LOG_WARN("[XeFG][Lifecycle] action=wrapped_release_deferred reason=release_already_in_progress");
    }
    else
    {
        // ReleaseSwapchain performs DestroyFGContext() -> Deactivate() inside
        // the serialized lifecycle transaction. Do not pre-Deactivate outside it.
        releaseCompleted = fg->ReleaseSwapchain(_handle);

        if (!releaseCompleted)
            LOG_ERROR("[XeFG][Lifecycle] action=wrapped_release_aborted reason=release_not_completed");
    }
}

if (releaseCompleted && state.currentFGSwapchain == fgProxyBeforeRelease)
    state.currentFGSwapchain = nullptr;
```

The identity check avoids an old wrapper clearing a newer public proxy if another lifecycle becomes visible after the successful release returns.

If `DestroySwapchainContext()` fails, do not clear `currentFGSwapchain` merely because the wrapper itself is being deleted.

The wrapper's own P2 `_real` reference must still be released exactly once.

---

# Task J — Propagate release failure from FFX API destroy

## 27. `ffxDestroyContext_Dx12FG()`

Keep the existing `FGPreserveSwapChain` behavior of `release/0.9`.

Inside the branch that actually destroys the swapchain context, replace the unchecked call with:

```cpp
if (!State::Instance().currentFG->ReleaseSwapchain(State::Instance().currentFG->Hwnd()))
{
    LOG_ERROR("[XeFG][Lifecycle] action=ffx_destroy_context_aborted reason=release_not_completed");
    return FFX_API_RETURN_ERROR_PARAMETER;
}
```

Only continue to the existing successful-destroy tail if the release completed.

### Important scope boundary

The existing FFX callback also contains separate `currentWrappedSwapchain` release/drain behavior.

Do not redesign that unrelated integration ownership path in P3.

P3 only guarantees that it is not executed **after a failed XeFG lifecycle release**.

If that FFX ownership behavior needs a dedicated follow-up, handle it separately with its own runtime evidence and scope.

---

# Task K — Preserve exact-success state transitions

## 28. `SetEnabled(false)` state commit

The target 0.9 code already keeps `_isActive` true if `SetEnabled(false)` is non-success, but it unconditionally clears `_waitingNewFrameData` afterward.

Keep state changes tied to exact success.

Recommended shape:

```cpp
xefg_swapchain_result_t result = XEFG_SWAPCHAIN_RESULT_SUCCESS;

if (_swapChainContext != nullptr)
{
    result = XeFGProxy::SetEnabled()(_swapChainContext, false);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        _isActive = false;
}
else
{
    _isActive = false;
}

if (_swapChainContext == nullptr || result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    _waitingNewFrameData = false;
```

Do not broadly rewrite unrelated dispatch logic in P3 if it already uses exact-success recovery semantics.

---

# 29. Explicitly out of scope

Do not import or redesign any of the following in this P3 PR:

```text
master LOW_LATENCY_INPUTS / InputXeLL architecture
master fakenvapi redesign
PR #17 thread-local FG hook experiment
PR #18 thread-aware Present mutex changes
PR #19 crash tracing
Reflex work
Subnautica 2 geometry/resource diagnostics
DLSSG / FSRFG frame-ID architecture
FSRFG-to-XeFG corrupted-scene investigation
general DXGI modernization
Special K integration
REFramework code changes
```

Do not solve unrelated release/0.9 rendering defects in this PR.

Do not convert warnings to broad success.

Do not add retries that eventually pretend Destroy succeeded.

Do not asynchronously destroy a context and continue immediately.

Do not clear `_swapChainContext` merely to make recreation proceed after failure.

---

# 30. Required static validation

Run:

```bash
git diff --check
```

## Lifecycle searches

Inspect:

```bash
rg -n "DestroySwapchainContext\(" OptiScaler/framegen/xefg OptiScaler/hooks OptiScaler/inputs/FG OptiScaler/wrapped
rg -n "ReleaseSwapchain\(" OptiScaler/hooks OptiScaler/inputs/FG OptiScaler/wrapped OptiScaler/framegen/xefg
rg -n "return result;" OptiScaler/framegen/xefg/XeFG_Dx12.cpp
rg -n "static_cast<int32_t>\(result\)\s*>=\s*0" OptiScaler/framegen/xefg
rg -n "_swapchainRecreationBlocked" OptiScaler/framegen/xefg
rg -n "oldSwapChain\s*=" OptiScaler/hooks/FG_Hooks.cpp
```

### Required review conclusions

- no XeFG result enum is returned directly from the bool `CreateSwapchainContext()` failure paths;
- `DestroySwapchainContext()` returns false on every non-exact Destroy result;
- negative Destroy failure retains the old context handle;
- warning Destroy result quarantines recreation and is not treated as success;
- `ReleaseSwapchain()` does not overwrite a retained failed-Destroy handle and continue;
- callers do not clear FG lifecycle state after a false release result;
- Create paths cannot construct a replacement while `_swapchainRecreationBlocked` is true;
- no blanket `>= 0` success policy was introduced;
- existing P1/P2 COM ownership fixes remain intact.

---

# 31. Build / formatting validation

Required:

```text
Release | x64 build
git diff --check
repository clang-format / formatting check
```

Do not perform broad formatting churn on legacy release/0.9 files.

Report baseline-only warnings separately from issues introduced by the P3 branch.

---

# 32. Runtime validation

P3 should be evaluated as the final staged REF compatibility baseline.

Recommended matrix:

| Test | OptiScaler | REFramework | XeFG | Purpose |
|---|---|---|---|---|
| A | P1+P2 baseline (`cc8645c8...`) | fork REF | On | pre-P3 comparison |
| B | P1+P2+P3 | fork REF | On | final coexistence candidate |
| C | P1+P2+P3 | absent | On | Opti-only regression smoke |

Priority RE Engine titles:

```text
Monster Hunter Wilds
Dragon's Dogma 2
```

Exercise when possible:

1. cold launch;
2. stable gameplay/menu;
3. Alt+Tab out and back repeatedly;
4. fullscreen/window/borderless transition;
5. resolution change / ResizeBuffers;
6. overlay open/close;
7. title/menu transitions that recreate presentation objects;
8. FG toggle off/on if the title permits it;
9. normal exit.

Watch specifically for:

```text
hang in Destroy
new context created immediately after failed Destroy
stale proxy Release suppression via oldSwapChain
REF renderer/overlay loss after recreate
DXGI invalid-call errors
XeFG context recreation loops
wrapper deletion followed by invalid public-proxy clearing
MHW/DD2 E_ABORT / device-loss behavior around resize/recreate
```

---

# 33. Lifecycle diagnostics

Keep diagnostics low-volume and lifecycle-only.

Recommended events:

```text
[XeFG][Lifecycle] action=destroy_begin
[XeFG][Lifecycle] action=destroy_return
[XeFG][Lifecycle] action=destroy_failed retained=true
[XeFG][Lifecycle] action=destroy_warning_quarantined retained=false
[XeFG][Lifecycle] action=release_swapchain_aborted reason=destroy_failed
[XeFG][Lifecycle] action=recreate_aborted reason=previous_destroy_failed
[XeFG][Lifecycle] action=recreate_aborted reason=lifecycle_transaction_in_progress
[XeFG][Lifecycle] action=init_aborted stage=...
[XeFG][Lifecycle] action=init_cleanup_failed stage=...
[XeFG][Lifecycle] action=final_proxy_release_quarantined
```

Do not add per-frame Present/resource tracing.

Do not port PR #19 diagnostics.

---

# 34. Failure-path validation expectations

A real vendor Destroy failure can be difficult to force on demand.

Do not add permanent production fault-injection merely to make the test easy.

Reviewers should verify the failure paths statically and, if a natural runtime failure occurs, confirm the following sequence from logs:

```text
destroy_begin
    -> destroy_return non-success
    -> destroy_failed OR destroy_warning_quarantined
    -> release_swapchain_aborted
    -> future recreate_aborted previous_destroy_failed
```

There must **not** be:

```text
destroy_return non-success
    -> normal release success tail
    -> replacement XeFG context created
```

---

# 35. What does not count as a P3 failure by itself

The following remain separate investigations:

```text
release/0.9 FSRFG -> XeFG corrupted 3D scene
Subnautica 2 scene/presentation geometry mismatch
DLSSG/FSRFG frame-ID or slot-generation issues
RE9/PRAGMATA Reflex spoofing quirks
master-only XeLL/fakenvapi behavior differences
```

If one of these still reproduces while P3 lifecycle invariants are correct, record it separately rather than expanding this PR.

---

# 36. Review checklist

## Branch / scope

- [ ] PR base is `reframework-0.9`.
- [ ] Implementation starts from `cc8645c8...` or a reviewed descendant.
- [ ] No merge from `master`.
- [ ] No mechanical cherry-pick of PR #1/#14.
- [ ] No unrelated architecture modernization.

## `XeFG_Dx12.h`

- [ ] lifecycle mutex exists only for XeFG create/release mutation;
- [ ] recreation-block state exists;
- [ ] release-in-progress ownership is local to XeFG lifecycle;
- [ ] no PR #18 Present mutex redesign was imported.

## `CreateSwapchainContext()`

- [ ] no `return result;` enum-to-bool failure path remains;
- [ ] D3D12CreateContext non-success returns false;
- [ ] XeLL SetSleepMode failure returns false;
- [ ] SetLatencyReduction non-success returns false;
- [ ] missing XeLL context returns false;
- [ ] release/0.9 XeLL/fakenvapi sequence is preserved.

## Partial initialization

- [ ] `AbortSwapchainInitialization(stage)` exists or equivalent logic is used;
- [ ] GetProperties non-success does not commit output;
- [ ] Init failure cleans/quarantines context;
- [ ] GetSwapChainPtr uses a local output pointer;
- [ ] partial returned COM pointer is released on non-success;
- [ ] failed cleanup blocks replacement rather than pretending success.

## Destroy

- [ ] only exact `XEFG_SWAPCHAIN_RESULT_SUCCESS` commits successful teardown;
- [ ] negative failure retains context handle;
- [ ] warning is quarantined and is not treated as success;
- [ ] every non-exact result returns false;
- [ ] XeLL survives failed/ambiguous XeFG Destroy;
- [ ] XeLL is destroyed only after exact successful XeFG teardown;
- [ ] recreation block is cleared only by exact successful teardown.

## Release

- [ ] public Release acquires lifecycle guard;
- [ ] overlapping lifecycle transaction returns false/deferred;
- [ ] configured mutex already-owned path no longer returns fake success;
- [ ] Destroy failure stops successful release tail;
- [ ] retained failed-Destroy handle is not overwritten with null;
- [ ] ReleaseObjects is not executed as if failed Destroy succeeded.

## Create / recreate

- [ ] both Create entry points serialize lifecycle mutation;
- [ ] both refuse creation while recreation is blocked;
- [ ] both check prior release result;
- [ ] neither creates a replacement after failed teardown.

## `FG_Hooks.cpp`

- [ ] `oldSwapChain` is not committed before replacement creation succeeds;
- [ ] `skipReleaseChecks` remains non-thread-local in this P3 backport;
- [ ] same-thread P3 lifecycle reentry is forwarded safely;
- [ ] XeFG final proxy release uses once-only lifecycle callback;
- [ ] failed release does not execute normal public-proxy success cleanup;
- [ ] P2 wrapper/real owner boundaries remain intact.

## Wrapper caller

- [ ] P2 `std::exchange(_real, nullptr)` cleanup remains;
- [ ] wrapper-owned real ref is still released exactly once;
- [ ] wrapper observes ReleaseSwapchain boolean result;
- [ ] wrapper does not pre-Deactivate outside serialized release transaction;
- [ ] failed FG release does not clear current FG proxy state;
- [ ] any success cleanup uses identity-aware state handling.

## FFX caller

- [ ] failed ReleaseSwapchain returns FFX error;
- [ ] successful tail is not run after lifecycle failure;
- [ ] unrelated FFX ownership code is not expanded into this PR.

## Result semantics

- [ ] no blanket `>= 0` lifecycle success policy;
- [ ] state/output commits require exact success;
- [ ] warning logging does not imply successful transition.

## Validation

- [ ] `git diff --check` passes;
- [ ] Release x64 build passes;
- [ ] clang-format/check passes;
- [ ] static lifecycle searches reviewed;
- [ ] runtime status documented honestly.

---

# 37. Suggested PR title and body

Suggested title:

```text
P3: fail closed on incomplete XeFG teardown for release/0.9
```

Suggested body:

```markdown
## Summary

- Make XeFG Destroy/release return meaningful success or failure on release/0.9.
- Block replacement creation after failed or ambiguous teardown.
- Serialize XeFG create/release lifecycle transactions without porting master Present-mutex architecture.
- Clean up partial XeFG initialization fail-closed.
- Preserve release/0.9 XeLL/fakenvapi lifetime ordering.
- Propagate release failure through FG hook, wrapper, and FFX destroy callers.

## Scope

P3 only. No LOW_LATENCY_INPUTS/InputXeLL port, PR #17/#18/#19 experiments, Reflex changes, frame-ID work, or unrelated rendering fixes.

## Validation

- `git diff --check`: ...
- Release|x64 build: ...
- clang-format/check: ...
- lifecycle searches: ...
- runtime MHW/DD2: ...
```

---

# 38. Final implementation rule

The final rule for P3 is:

> **Do not turn uncertainty into success.**

In concrete terms:

```text
exact successful Destroy
    -> commit teardown
    -> release dependent 0.9 XeLL lifetime
    -> allow future create

negative Destroy failure
    -> retain old XeFG context identity
    -> retain dependent lifetime
    -> return failure
    -> block replacement

warning / ambiguous Destroy result
    -> do not claim success
    -> do not blindly reuse potentially invalid handle
    -> quarantine replacement
    -> return failure

partial create failure
    -> clean up through same fail-closed Destroy policy
    -> never leave a half-initialized context looking valid

caller receives false
    -> caller must not clear/recreate as though teardown succeeded
```

P1 fixed backbuffer ownership.

P2 fixed swapchain COM owner boundaries.

P3 must now ensure that the XeFG lifecycle itself cannot advance from **failed/ambiguous teardown** to **apparently clean replacement creation**.

When P3 is complete, `reframework-0.9` becomes the intended staged REF/XeFG compatibility validation baseline.