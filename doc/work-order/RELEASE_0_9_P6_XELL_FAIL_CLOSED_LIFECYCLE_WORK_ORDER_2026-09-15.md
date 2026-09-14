# Release 0.9 P6 Work Order — XeLL Fail-Closed Lifecycle Hardening

Date: 2026-09-15

## Status

Implementation work order.

P5-A and P5-B are complete. P6 is the next lifecycle-hardening step for the `reframework-0.9` compatibility branch.

P6 must keep XeLL as a **separate lifecycle subsystem** used by XeFG for latency reduction. It must not backport the current `master` low-latency/InputXeLL architecture.

The goal is narrow:

> A failed or uncertain XeLL retirement must never be treated as a clean lifecycle boundary, and a stale XeLL context must never be rebound to a newly created XeFG context.

---

## 1. Verified remote baselines

### OptiScaler implementation target

Repository: `onehoon/OptiScaler`

Branch: `reframework-0.9`

Current HEAD:

```text
b724ddd4dfb0d7d69351e5b3b7424f76b7e26d9a
P5-B2: detach REF before final XeFG proxy release (#29)
```

This already contains the P1-P4 release/ownership hardening and P5-B2 REFramework pre-retire handoff.

### REFramework compatibility baseline

Repository: `onehoon/REFramework`

Branch: `master`

Current HEAD:

```text
4bf45b370e602f7f6a3ca54f308daa4e353aab8a
XeFG P5-B1: add final proxy pre-retire handoff (#54)
```

No REFramework code change is required for P6.

### OptiScaler master — reference only

Current fork `master` reference:

```text
64874932da7ceb3475ed1e8fd8aaf837249f022a
```

`master` has a substantially different low-latency architecture (`InputXeLL`, low-latency inputs, additional XeLL hooks/exports, integrated fakenvapi ownership). It may be consulted for invariants only.

**Do not transplant the master architecture into `reframework-0.9`.**

---

## 2. Pinned Intel XeSS/XeLL SDK contract

The current `reframework-0.9` branch pins the Intel XeSS submodule to:

```text
external/xess = 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0
```

The pinned `inc/xell/xell.h` defines:

```cpp
typedef enum _xell_result_t
{
    XELL_RESULT_SUCCESS = 0,
    XELL_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    XELL_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    XELL_RESULT_ERROR_UNINITIALIZED = -3,
    XELL_RESULT_ERROR_INVALID_ARGUMENT = -4,
    XELL_RESULT_ERROR_DEVICE = -6,
    XELL_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    XELL_RESULT_ERROR_INVALID_CONTEXT = -8,
    XELL_RESULT_ERROR_UNSUPPORTED = -10,
    XELL_RESULT_ERROR_UNKNOWN = -1000,
} xell_result_t;
```

Important consequences:

1. XeLL has **exact success `0`** and negative errors in the pinned SDK.
2. There is no XeFG-style positive-warning success class to preserve here.
3. XeLL create is successful only when the API returns `XELL_RESULT_SUCCESS` **and** the returned context is non-null.
4. Intel's pinned developer guide requires the XeSS-FG object to be destroyed **before** the XeLL object.

P6 must preserve the vendor order:

```text
public XeFG proxy retirement
    -> XeFG context destroy
    -> XeLL publication detach/unpublish
    -> XeLL context destroy
```

Do not move XeLL destruction ahead of XeFG destruction.

Relevant pinned Intel references:

- `external/xess/inc/xell/xell.h`
- `external/xess/inc/xell/xell_d3d12.h`
- `external/xess/doc/xell_developer_guide_english.md`
- `external/xess/doc/xess_fg_developer_guide_english.md`

---

## 3. Architecture invariant to preserve

The release/0.9 compatibility architecture remains:

```text
XeFG
  ├─ owns its XeFG swapchain/context lifecycle
  ├─ performs REF P5-B pre-retire handoff at the final public proxy boundary
  └─ attaches one XeLL context for latency reduction

XeLL
  ├─ has its own context handle
  ├─ has its own create/destroy result
  ├─ has its own failure quarantine
  └─ may be published to fakenvapi for Reflex/NVAPI translation
```

P6 must not collapse these into one master-style low-latency owner.

The required invariant is:

> A XeFG lifecycle may depend on a confirmed-live XeLL context, but a failed XeLL retirement must remain visibly uncertain and must block any future XeLL/XeFG recreation that could reuse or overwrite that uncertain context.

---

## 4. Code-proven bug #1 — `DestroyXeLLContext()` always reports success

Current `OptiScaler/proxies/XeLL_Proxy.h`:

```cpp
static bool DestroyXeLLContext()
{
    LOG_DEBUG("");

    if (_xellContext != nullptr)
    {
        auto context = _xellContext;
        _xellContext = nullptr;
        auto xellResult = _xellDestroyContext(context);

        LOG_INFO("XeLL DestroyContext result: {} ({})",
                 magic_enum::enum_name(xellResult),
                 (UINT) xellResult);

        // Set it back because context is not destroyed
        if (xellResult != XELL_RESULT_SUCCESS)
            _xellContext = context;
    }

    return true;
}
```

The function restores `_xellContext` after a vendor failure, correctly acknowledging that the context is not known to be destroyed, but then returns `true` unconditionally.

Therefore callers cannot distinguish:

```text
XeLL exact destroy success
```

from:

```text
XeLL destroy failure + old context retained
```

This is a lifecycle correctness bug, not merely missing logging.

---

## 5. Code-proven bug #2 — create continues after failed old-context destroy

Current `XeLLProxy::CreateContext(...)`:

```cpp
if (_xellContext != nullptr)
    DestroyXeLLContext();

xellResult = _xellD3D12CreateContext(device, &_xellContext);
```

The result of `DestroyXeLLContext()` is ignored.

Current failure sequence can therefore be:

```text
old XeLL context A is live
    -> DestroyXeLLContext(A)
    -> xellDestroyContext(A) fails
    -> _xellContext restored to A
    -> DestroyXeLLContext still returns true
    -> CreateContext continues
    -> xellD3D12CreateContext(device, &_xellContext)
```

This passes the storage containing the retained old context directly as the new create output parameter.

P6 must stop before the create call if retirement of A was not confirmed.

---

## 6. Code-proven bug #3 — XeFG ignores the XeLL create result

Current `XeFG_Dx12::CreateSwapchainContext(...)`:

```cpp
// if (XeLLProxy::Context() == nullptr)
XeLLProxy::CreateContext(device);

if (XeLLProxy::Context() != nullptr)
{
    ...
    auto xellResult =
        XeLLProxy::SetSleepMode()(XeLLProxy::Context(), &sleepParams);
    ...

    auto fnaResult =
        fakenvapi::setModeAndContext(XeLLProxy::Context(), Mode::XeLL);

    result = XeFGProxy::SetLatencyReduction()(
        _swapChainContext,
        XeLLProxy::Context());
    ...
}
```

The boolean returned by `XeLLProxy::CreateContext(device)` is ignored.

This creates a concrete stale-context path:

```text
old XeLL A exists
    -> destroy A fails
    -> A is restored
    -> new XeLL create is attempted and returns failure
    -> CreateContext() returns false
    -> caller ignores false
    -> XeLLProxy::Context() may still be A
    -> caller sees non-null A
    -> SetSleepMode(A)
    -> fakenvapi publishes A
    -> SetLatencyReduction(new XeFG context, A)
```

P6 must make this impossible even if a vendor create failure leaves its output storage unchanged.

---

## 7. Code-proven bug #4 — new XeLL create writes directly into global committed state

Current XeLL create passes:

```cpp
&_xellContext
```

directly to `xellD3D12CreateContext`.

That means the vendor call mutates the committed global lifecycle handle before OptiScaler has validated both:

```text
result == XELL_RESULT_SUCCESS
```

and:

```text
returned handle != nullptr
```

Intel's pinned guide explicitly requires both checks.

P6 must create into a local candidate first and publish the handle to `_xellContext` only after validation.

---

## 8. Code-proven bug #5 — XeFG teardown ignores XeLL destroy failure

Current `XeFG_Dx12::DestroySwapchainContext()` correctly destroys XeFG first.

After exact XeFG destroy success it does:

```cpp
if (XeLLProxy::Context() != nullptr)
    XeLLProxy::DestroyXeLLContext();

_swapchainRecreationBlocked = false;
...
return true;
```

The XeLL result is ignored.

So this sequence is currently treated as a successful whole-lifecycle teardown:

```text
XeFG destroy = success
XeLL destroy = failure
old XeLL context retained
DestroySwapchainContext() = true
recreation allowed
```

P6 must return lifecycle failure in this case and preserve a **XeLL-specific recreation quarantine**.

Do not overload XeFG's existing `_swapchainRecreationBlocked` as the only representation of this state. XeFG destroy and XeLL destroy are separate lifecycle facts.

---

## 9. Important secondary hole — `_swapChainContext == nullptr` is not sufficient after XeLL failure

Current `DestroySwapchainContext()` starts with:

```cpp
if (_swapChainContext == nullptr || State::Instance().isShuttingDown)
    return true;
```

After P6, a valid fail-closed state can be:

```text
XeFG destroy succeeded
    -> _swapChainContext == nullptr
XeLL unpublish/destroy failed
    -> XeLL context retained
    -> XeLL recreation quarantined
```

A subsequent teardown call must **not** return success merely because `_swapChainContext == nullptr`.

P6 must preserve the outstanding XeLL uncertainty.

Conceptually:

```cpp
if (State::Instance().isShuttingDown)
    return true; // preserve existing shutdown policy in P6

if (_swapChainContext == nullptr)
{
    if (XeLLProxy::ContextRecreationBlocked())
    {
        LOG_ERROR(
            "[XeLL][Lifecycle] action = teardown_incomplete, "
            "reason = retained_quarantined_context, context = {:X}",
            (size_t) XeLLProxy::Context());
        return false;
    }

    return true;
}
```

Do not broaden P6 into shutdown-policy redesign. The `isShuttingDown` behavior remains a later/lower-priority issue unless independent crash evidence requires it.

---

## 10. Code-proven partial-init poisoning in `InitXeLL()` / `HookXeLL()`

Current `InitXeLL()` begins with:

```cpp
if (_dll != nullptr)
    return true;
```

Current `HookXeLL()` sets:

```cpp
_dll = libxellModule;
```

before all function pointers are resolved, and finally validates only:

```cpp
bool loadResult = _xellDestroyContext != nullptr;
```

However the current creation path later directly calls at least:

```cpp
_xellD3D12CreateContext(...)
_xellSetSleepMode(...)
_xellSetLoggingCallback(...)
```

Consequences:

1. `_dll` can be non-null while required exports are missing.
2. A later `InitXeLL()` can return `true` solely because `_dll` is non-null.
3. The subsequent code can dereference a null required function pointer.

P6 must separate:

```text
module discovered
```

from:

```text
required XeLL context API ready
```

Do not unload/reload DLLs as part of this work. Just stop reporting readiness when required exports are missing.

---

## 11. fakenvapi stale-context publication window

Current creation order is:

```cpp
auto fnaResult =
    fakenvapi::setModeAndContext(XeLLProxy::Context(), Mode::XeLL);

result = XeFGProxy::SetLatencyReduction()(
    _swapChainContext,
    XeLLProxy::Context());
```

So the XeLL context is published to fakenvapi **before** XeFG confirms that it accepted the latency-reduction context.

Current teardown does not unpublish the XeLL context before `xellDestroyContext`.

The legacy fakenvapi interface contract used by release/0.9 includes:

```cpp
NvAPI_Status __cdecl Fake_GetLowLatencyCtx(
    void** low_latency_context,
    Mode* mode);

NvAPI_Status __cdecl Fake_SetLowLatencyCtx(
    void* low_latency_context,
    Mode mode);
```

and explicitly states that passing a null context to `Fake_SetLowLatencyCtx` uninitializes the low-latency technology using that context.

P6 must therefore:

1. publish XeLL to fakenvapi **after** successful `SetLatencyReduction`, not before;
2. remember whether this lifecycle actually published the XeLL context successfully;
3. before destroying that XeLL context, remove the fakenvapi publication only if the current published pointer/mode still matches the expected XeLL context;
4. never blindly clear a different context that may have replaced it;
5. if OptiScaler knows it published the context but cannot safely prove/unpublish it, **do not destroy the XeLL context** — quarantine instead.

This is deliberately fail-closed: retaining a live context is safer than leaving a destroyed pointer published through fakenvapi.

---

# P6 implementation

## 12. Recommended PR shape

Repository: `onehoon/OptiScaler`

Base: `reframework-0.9`

Recommended PR title:

```text
P6: fail closed on XeLL lifecycle uncertainty
```

Expected files:

```text
OptiScaler/proxies/XeLL_Proxy.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/nvapi/fakenvapi.h
OptiScaler/nvapi/fakenvapi.cpp
```

`XeFG_Dx12.h` may be touched only if a small per-lifecycle publication flag is chosen instead of keeping publication ownership inside `fakenvapi`.

No REFramework file should change.

Keep this as one focused PR unless implementation reveals that fakenvapi ownership requires a substantially larger synchronization redesign. If that happens, stop and split the fakenvapi ownership portion into a follow-up rather than importing P7 synchronization work into P6.

---

## 13. Add an explicit XeLL context quarantine

Recommended minimal state in `XeLLProxy`:

```cpp
inline static bool _contextRecreationBlocked = false;
```

Expose read-only state:

```cpp
static bool ContextRecreationBlocked() noexcept
{
    return _contextRecreationBlocked;
}
```

Add a narrow fail-closed setter for cases where the context must remain live without calling vendor destroy, for example when fakenvapi unpublish cannot be confirmed:

```cpp
static void QuarantineContext() noexcept
{
    if (_xellContext != nullptr)
        _contextRecreationBlocked = true;
}
```

Do not reuse `_swapchainRecreationBlocked` for this purpose.

The two states have different meanings:

```text
_swapchainRecreationBlocked
    = XeFG swapchain context retirement is uncertain

XeLLProxy::_contextRecreationBlocked
    = XeLL context retirement/publication cleanup is uncertain
```

---

## 14. Make XeLL readiness explicit

Add a helper that represents the minimum API required by this release/0.9 XeFG lifecycle.

Conceptual example:

```cpp
static bool HasRequiredContextExports() noexcept
{
    return _dll != nullptr &&
           _xellDestroyContext != nullptr &&
           _xellSetSleepMode != nullptr &&
           _xellD3D12CreateContext != nullptr;
}
```

`xellSetLoggingCallback` is diagnostic, not lifecycle-critical. Either require it too or guard its use. The preferred release/0.9 behavior is to guard it and keep logging registration optional:

```cpp
if (_xellSetLoggingCallback != nullptr)
{
    const auto logResult = _xellSetLoggingCallback(
        _xellContext,
        XELL_LOGGING_LEVEL_DEBUG,
        xellLogCallback);

    if (logResult != XELL_RESULT_SUCCESS)
    {
        LOG_WARN(
            "XeLL SetLoggingCallback failed: {} ({})",
            magic_enum::enum_name(logResult),
            (int32_t) logResult);
    }
}
```

Then change the module-ready shortcuts.

Conceptually:

```cpp
static bool InitXeLL()
{
    if (_dll != nullptr)
        return HookXeLL(_dll); // revalidate exports, do not trust module presence

    ... existing module discovery ...
}
```

and in `HookXeLL()`:

```cpp
if (libxellModule == nullptr)
    return false;

if (_dll == libxellModule && HasRequiredContextExports())
    return true;

_dll = libxellModule;

... resolve exports ...

const bool loadResult = HasRequiredContextExports();
LOG_INFO("XeLL required context exports ready: {}", loadResult);
return loadResult;
```

The exact helper name can differ.

Required invariant:

> `_dll != nullptr` must never by itself mean XeLL is usable.

---

## 15. Make `DestroyXeLLContext()` return the real lifecycle result

Recommended behavior:

```cpp
static bool DestroyXeLLContext()
{
    LOG_DEBUG("");

    if (_xellContext == nullptr)
        return !_contextRecreationBlocked;

    if (_xellDestroyContext == nullptr)
    {
        _contextRecreationBlocked = true;
        LOG_ERROR(
            "[XeLL][Lifecycle] action = destroy_blocked, "
            "reason = destroy_export_missing, context = {:X}",
            (size_t) _xellContext);
        return false;
    }

    auto context = _xellContext;

    // Hide the context from normal users while vendor retirement is in flight.
    _xellContext = nullptr;

    const auto result = _xellDestroyContext(context);

    LOG_INFO(
        "[XeLL][Lifecycle] action = destroy_return, "
        "context = {:X}, result = {} ({})",
        (size_t) context,
        magic_enum::enum_name(result),
        static_cast<int32_t>(result));

    if (result != XELL_RESULT_SUCCESS)
    {
        // Exact success was not confirmed. Retain the handle and quarantine.
        _xellContext = context;
        _contextRecreationBlocked = true;

        LOG_ERROR(
            "[XeLL][Lifecycle] action = destroy_failed, "
            "context = {:X}, result = {} ({}), retained = true",
            (size_t) context,
            magic_enum::enum_name(result),
            static_cast<int32_t>(result));
        return false;
    }

    _contextRecreationBlocked = false;

    LOG_INFO(
        "[XeLL][Lifecycle] action = destroy_complete, context = {:X}",
        (size_t) context);
    return true;
}
```

Do not treat any non-zero result as lifecycle completion.

The pinned SDK has no positive-warning XeLL result category.

---

## 16. Make XeLL creation transactional

Do not pass `_xellContext` directly as the vendor output parameter.

Recommended structure:

```cpp
static bool CreateContext(ID3D12Device* device)
{
    if (device == nullptr)
        return false;

    if (_contextRecreationBlocked)
    {
        LOG_ERROR(
            "[XeLL][Lifecycle] action = create_blocked, "
            "reason = previous_retirement_uncertain, context = {:X}",
            (size_t) _xellContext);
        return false;
    }

    if (!InitXeLL())
    {
        LOG_ERROR("XeLL proxy is not ready");
        return false;
    }

    if (_xellContext != nullptr)
    {
        if (!DestroyXeLLContext())
        {
            LOG_ERROR(
                "[XeLL][Lifecycle] action = create_blocked, "
                "reason = previous_context_destroy_failed, context = {:X}",
                (size_t) _xellContext);
            return false;
        }
    }

    xell_context_handle_t newContext = nullptr;
    xell_result_t result{};

    {
#ifndef DONT_USE_XMX
        ScopedSkipSpoofing skipSpoofing {};
#endif
        result = _xellD3D12CreateContext(device, &newContext);
    }

    if (result != XELL_RESULT_SUCCESS || newContext == nullptr)
    {
        LOG_ERROR(
            "[XeLL][Lifecycle] action = create_failed, "
            "result = {} ({}), candidate = {:X}",
            magic_enum::enum_name(result),
            static_cast<int32_t>(result),
            (size_t) newContext);
        return false;
    }

    // Commit only after exact success + non-null candidate.
    _xellContext = newContext;
    _contextRecreationBlocked = false;

    LOG_INFO(
        "[XeLL][Lifecycle] action = create_complete, context = {:X}",
        (size_t) _xellContext);

    ... optional logging callback ...

    return true;
}
```

Do not attempt a second new context after an old-context destroy failure.

Do not clear `_contextRecreationBlocked` merely because a later create was requested.

---

## 17. Caller must honor `XeLLProxy::CreateContext()`

Current caller ignores the boolean result.

Replace the pattern:

```cpp
XeLLProxy::CreateContext(device);

if (XeLLProxy::Context() != nullptr)
{
    ...
}
```

with explicit validation:

```cpp
if (!XeLLProxy::CreateContext(device))
{
    LOG_ERROR(
        "[XeLL][Lifecycle] action = xefg_init_aborted, "
        "reason = xell_create_failed");
    return false;
}

auto* xellContext = XeLLProxy::Context();
if (xellContext == nullptr)
{
    LOG_ERROR(
        "[XeLL][Lifecycle] action = xefg_init_aborted, "
        "reason = xell_context_missing_after_success");
    return false;
}
```

From this point onward, use the captured `xellContext` for the current initialization transaction instead of repeatedly rereading the global pointer.

This reduces the chance of accidentally observing a different lifecycle handle during the same transaction.

---

## 18. Preflight the XeLL quarantine before creating a new XeFG context

At the beginning of `XeFG_Dx12::CreateSwapchainContext(...)`, before creating a new vendor XeFG context, reject an outstanding XeLL quarantine:

```cpp
if (XeLLProxy::ContextRecreationBlocked())
{
    LOG_ERROR(
        "[XeLL][Lifecycle] action = xefg_create_blocked, "
        "reason = xell_context_quarantined, context = {:X}",
        (size_t) XeLLProxy::Context());
    return false;
}
```

This prevents the following wasteful/ambiguous sequence:

```text
known-uncertain old XeLL exists
    -> create a brand-new XeFG context anyway
    -> discover XeLL is blocked
    -> immediately destroy new XeFG context again
```

P6 should fail before new XeFG creation when XeLL is already known to be quarantined.

---

## 19. Publish to fakenvapi only after XeFG accepts XeLL

Current order:

```text
SetSleepMode
    -> fakenvapi publish
    -> XeFG SetLatencyReduction
```

Required P6 order:

```text
XeLL CreateContext exact success
    -> SetSleepMode exact success
    -> XeFG SetLatencyReduction exact success
    -> optional fakenvapi publish
```

Conceptual code:

```cpp
const auto xellResult =
    XeLLProxy::SetSleepMode()(xellContext, &sleepParams);
if (xellResult != XELL_RESULT_SUCCESS)
{
    ...
    return false;
}

const auto latencyResult =
    XeFGProxy::SetLatencyReduction()(_swapChainContext, xellContext);
if (latencyResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    ...
    return false;
}

const auto fnaResult =
    fakenvapi::setModeAndContext(xellContext, Mode::XeLL);
LOG_DEBUG("fakenvapi::setModeAndContext: {}", fnaResult);
```

fakenvapi remains optional. Failure to publish does not by itself invalidate a successfully bound XeFG+XeLL pair.

However, if publication succeeds, teardown must not destroy that XeLL context while the same pointer is still published.

---

## 20. Add expected-old fakenvapi unpublish

Add a narrow helper such as:

```cpp
static bool clearModeAndContextIfMatches(
    void* expectedContext,
    Mode expectedMode);
```

The helper must never blindly clear another low-latency owner.

A recommended pattern is to track the last context successfully published by this helper:

```cpp
inline static void* _publishedLowLatencyContext = nullptr;
inline static Mode _publishedLowLatencyMode = Mode::LatencyFlex;
```

On successful `setModeAndContext(...)`:

```cpp
if (result == NVAPI_OK)
{
    _publishedLowLatencyContext = context;
    _publishedLowLatencyMode = mode;
}
```

Then clear only when the expected handle is the one OptiScaler believes it published.

Conceptual implementation:

```cpp
bool fakenvapi::clearModeAndContextIfMatches(
    void* expectedContext,
    Mode expectedMode)
{
    if (expectedContext == nullptr)
        return true;

    if (_publishedLowLatencyContext != expectedContext ||
        _publishedLowLatencyMode != expectedMode)
    {
        // This lifecycle did not publish the expected context, or ownership
        // already moved. Do not touch another owner.
        return true;
    }

    if (!Fake_GetLowLatencyCtx || !Fake_SetLowLatencyCtx)
        return false;

    void* currentContext = nullptr;
    Mode currentMode = Mode::LatencyFlex;

    const auto getResult =
        Fake_GetLowLatencyCtx(&currentContext, &currentMode);
    if (getResult != NVAPI_OK)
        return false;

    if (currentContext != expectedContext || currentMode != expectedMode)
    {
        // Underlying ownership moved after our earlier publish. Do not clear it.
        _publishedLowLatencyContext = nullptr;
        _publishedLowLatencyMode = Mode::LatencyFlex;
        return true;
    }

    // fakenvapi 1.3.5 contract: nullptr uninits the low-latency context.
    const auto clearResult =
        Fake_SetLowLatencyCtx(nullptr, expectedMode);
    if (clearResult != NVAPI_OK)
        return false;

    if (_lowLatencyContext == expectedContext)
        _lowLatencyContext = nullptr;

    _lowLatencyMode = Mode::LatencyFlex;
    _publishedLowLatencyContext = nullptr;
    _publishedLowLatencyMode = Mode::LatencyFlex;
    return true;
}
```

The exact local ownership representation may differ.

Important constraints:

- no unconditional `Fake_SetLowLatencyCtx(nullptr, ...)`;
- no clearing when current pointer/mode differs from the expected XeLL publication;
- no new broad mutex or lock-order redesign in P6;
- do not treat an inability to verify/clear a context that OptiScaler knows it published as safe.

If implementation proves the helper cannot be made narrow without introducing new synchronization ownership, stop and split this portion into a follow-up instead of importing P7 into P6.

---

## 21. Teardown sequence after P6

`XeFG_Dx12::DestroySwapchainContext()` must preserve Intel's order and propagate XeLL failure.

Recommended sequence:

```text
1. XeFG context exists
2. call xefgSwapChainDestroy
3. require exact XeFG lifecycle completion under existing P4/P5 rules
4. capture the current XeLL context
5. if this XeLL was published to fakenvapi:
      expected-old unpublish
      - mismatch / ownership moved -> do not touch new owner; continue
      - unpublish success          -> continue
      - cannot verify/clear        -> quarantine XeLL, return false
6. call xellDestroyContext
7. require exact XELL_RESULT_SUCCESS
8. only then report whole teardown success
```

Conceptual integration:

```cpp
const auto xellContext = XeLLProxy::Context();
if (xellContext != nullptr)
{
    if (!fakenvapi::clearModeAndContextIfMatches(
            xellContext,
            Mode::XeLL))
    {
        XeLLProxy::QuarantineContext();

        LOG_ERROR(
            "[XeLL][Lifecycle] action = destroy_blocked, "
            "reason = fakenvapi_unpublish_failed, context = {:X}",
            (size_t) xellContext);
        return false;
    }

    if (!XeLLProxy::DestroyXeLLContext())
    {
        LOG_ERROR(
            "[XeLL][Lifecycle] action = xefg_teardown_incomplete, "
            "reason = xell_destroy_failed, context = {:X}",
            (size_t) XeLLProxy::Context());
        return false;
    }
}
```

Only after the XeLL lifecycle is complete should the function execute its existing normal-success cleanup and return `true`.

---

## 22. Do not let null XeFG context hide a quarantined XeLL context

After XeFG destroy succeeds but XeLL cleanup fails, `_swapChainContext` is intentionally null.

Therefore the beginning of `DestroySwapchainContext()` should distinguish:

```text
no XeFG + no XeLL uncertainty
```

from:

```text
no XeFG + quarantined XeLL
```

Conceptual form:

```cpp
if (State::Instance().isShuttingDown)
    return true;

if (_swapChainContext == nullptr)
{
    if (XeLLProxy::ContextRecreationBlocked())
    {
        LOG_ERROR(
            "[XeLL][Lifecycle] action = destroy_incomplete, "
            "reason = quarantined_context_without_xefg, context = {:X}",
            (size_t) XeLLProxy::Context());
        return false;
    }

    return true;
}
```

Do not silently clear the XeLL quarantine from this branch.

---

## 23. Expected normal lifecycle after P6

```text
CreateSwapchainContext
    -> XeLL quarantine preflight
    -> XeFG create
    -> XeLL Init/required-export validation
    -> XeLL old-context retirement if required
    -> XeLL new context created into local candidate
    -> exact SUCCESS + non-null candidate
    -> commit _xellContext
    -> SetSleepMode
    -> XeFG SetLatencyReduction
    -> optional fakenvapi publish

...

P5-B final proxy pre-retire handoff
    -> REF detaches borrowed presentation lifecycle
    -> final public proxy release
    -> XeFG Destroy exact lifecycle handling
    -> expected-old fakenvapi unpublish
    -> XeLL Destroy exact SUCCESS
    -> XeLL quarantine clear
    -> normal release completes
```

---

## 24. Expected destroy-failure lifecycle after P6

```text
XeFG Destroy succeeds
    -> _swapChainContext retired
    -> fakenvapi publication removed or confirmed moved
    -> xellDestroyContext(old XeLL) fails
    -> old XeLL handle restored
    -> XeLL recreation quarantine = true
    -> DestroySwapchainContext returns false
    -> ReleaseSwapchainLocked returns false
    -> no new XeLL context creation
    -> no new XeFG lifecycle allowed past XeLL preflight
```

Most important forbidden outcome:

```text
old XeLL destroy failed
    -> old pointer retained
    -> new XeLL create attempted
    -> old pointer mistaken for new success
    -> stale old XeLL bound to new XeFG
```

That sequence must become structurally impossible.

---

## 25. Expected fakenvapi-unpublish failure lifecycle

```text
XeFG Destroy succeeds
    -> Opti knows old XeLL was published to fakenvapi
    -> current published owner cannot be safely verified/cleared
    -> do NOT call xellDestroyContext
    -> keep XeLL context live
    -> quarantine XeLL recreation
    -> return teardown failure
```

A retained live context is preferable to a freed XeLL pointer remaining reachable through fakenvapi.

---

## 26. Logging requirements

Add lifecycle logs that make the following states distinguishable without verbose tracing:

```text
[XeLL][Lifecycle] action = create_complete
[XeLL][Lifecycle] action = create_failed
[XeLL][Lifecycle] action = create_blocked
[XeLL][Lifecycle] action = destroy_return
[XeLL][Lifecycle] action = destroy_complete
[XeLL][Lifecycle] action = destroy_failed
[XeLL][Lifecycle] action = destroy_blocked
[XeLL][Lifecycle] action = xefg_create_blocked
[XeLL][Lifecycle] action = xefg_init_aborted
[XeLL][Lifecycle] action = xefg_teardown_incomplete
```

Include the context pointer and raw result where applicable.

Do not add per-frame logging.

Do not add sleeps/yields/retries to mask timing behavior.

---

## 27. Scope exclusions

P6 must not include:

- REFramework changes;
- P5-A/P5-B ABI or lifecycle changes;
- PR17/PR18 experimental thread/reentrancy changes;
- PR19 diagnostic ring-trace behavior;
- master `InputXeLL` / `LowLatencyCtx` architecture backport;
- `LOW_LATENCY_INPUTS` integration;
- XeLL D3D12 app-queue/display-info/generated-frame APIs from master;
- general FG mutex redesign;
- REF hook-monitor mutex changes;
- command-queue lifetime ownership changes;
- shutdown/vendor-destroy policy redesign;
- arbitrary DLL unload/reload logic;
- broad fakenvapi modernization.

P7 remains responsible for cross-system mutex/re-entry and D3D12 queue ownership work.

---

## 28. Do not copy these master concepts into 0.9

Current `master` has substantially different XeLL ownership including:

```text
hooks/Xell_Hooks.h
low_latency/input/input_xell.h
LOW_LATENCY_INPUTS
InputXeLL
additional XeLL APIs
integrated fakenvapi low-latency ownership
```

Those are not P6 solutions for release/0.9.

P6 should borrow only general invariants such as:

```text
validate before publish
fail closed on uncertain ownership
never use a destroyed context
```

not the implementation architecture.

---

## 29. Validation — source/build

Required before PR review:

```text
git diff --check
```

Run the repository clang-format workflow/check on changed C/C++ files.

Build the exact target branch as Release x64 using the branch's existing workflow-equivalent command:

```text
msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
```

Confirm the final artifact remains:

```text
x64\Release\a\OptiScaler.dll
```

No new REFramework link/import dependency may be introduced.

---

## 30. Validation — required failure injection

A normal successful run is not sufficient for P6. The lifecycle changes specifically target failure states.

### Test A — XeLL destroy failure

Force or shim:

```text
xellDestroyContext -> XELL_RESULT_ERROR_UNKNOWN
```

Expected:

```text
old XeLL handle retained
ContextRecreationBlocked == true
DestroyXeLLContext == false
DestroySwapchainContext == false
no xellD3D12CreateContext for a replacement context
no SetLatencyReduction with the old handle on a new XeFG context
```

### Test B — new XeLL create failure

Force:

```text
xellD3D12CreateContext -> error
```

Expected:

```text
CreateContext == false
_xellContext remains null after a clean previous retirement
XeFG initialization aborts
no SetSleepMode call with a stale context
no fakenvapi XeLL publication
no SetLatencyReduction call
```

### Test C — success result with null candidate

If practical, shim:

```text
xellD3D12CreateContext -> XELL_RESULT_SUCCESS
out_context -> nullptr
```

Expected:

```text
CreateContext == false
no context commit
no later XeLL use
```

This validates the Intel-required `SUCCESS && non-null` rule.

### Test D — missing required export

Simulate one required pointer unavailable, especially:

```text
xellD3D12CreateContext == nullptr
```

or:

```text
xellSetSleepMode == nullptr
```

Expected:

```text
InitXeLL/HookXeLL reports not ready
no null function call
XeFG initialization fails cleanly
```

### Test E — fakenvapi clear succeeds

With a successfully published XeLL context:

```text
Fake_GetLowLatencyCtx -> same pointer + Mode::XeLL
Fake_SetLowLatencyCtx(nullptr, Mode::XeLL) -> NVAPI_OK
```

Expected:

```text
publication removed before xellDestroyContext
XeLL destroy proceeds
```

### Test F — fakenvapi ownership moved

Before teardown make the current fakenvapi context differ from the expected XeLL pointer.

Expected:

```text
P6 does not clear the replacement context
P6 relinquishes its local publication ownership
old XeLL teardown may continue because fakenvapi no longer references it
```

### Test G — fakenvapi clear cannot be confirmed

If OptiScaler successfully published the expected XeLL context, force getter or clear failure.

Expected:

```text
xellDestroyContext is NOT called
XeLL context remains live
XeLL recreation is quarantined
teardown returns false
```

---

## 31. Runtime validation matrix

After source/build validation, test at minimum:

| Case | Configuration | Expected |
| --- | --- | --- |
| 1 | MHW + current fork REF + current `reframework-0.9` + XeFG | launch, FG active, repeated scene/swapchain transitions, clean exit |
| 2 | DD2 + current fork REF + XeFG | same lifecycle behavior, no new E_ABORT/crash regression |
| 3 | Capcom game + current REF, repeated XeFG enable/disable if supported | no stale XeLL reuse |
| 4 | Non-Capcom XeFG game without REF | no REF dependency, XeLL lifecycle remains functional |
| 5 | Intel GPU XeFG | native XeLL path works |
| 6 | NVIDIA/AMD cross-vendor XeFG with fakenvapi path | publish/unpublish works and does not leave stale context |
| 7 | XeFG disabled / non-XeFG launch | no behavior change |

For repeated transition testing, look specifically for pointer reuse in logs:

```text
old XeLL context A destroy_complete
new XeLL context B create_complete
```

or a legitimate allocator reuse only after confirmed destruction.

There must never be:

```text
destroy_failed(A)
    -> create_complete(A interpreted as new)
```

without process restart.

---

## 32. P5-B interaction validation

P6 must not disturb the P5-B final-proxy sequence.

Expected combined normal order:

```text
1. hkFGRelease detects confirmed final public proxy boundary
2. P5-B2 calls REFramework_XeFG_PreRetireSwapchainV1
3. REF P5-B1 detaches active borrowed presentation lifecycle
4. Opti consumes final public proxy reference
5. Opti calls XeFG vendor Destroy
6. REF P5-A reconciles exact-success Destroy
7. Opti safely unpublishes its XeLL fakenvapi context if still owned
8. Opti destroys XeLL
9. release completes
```

Do not call REF from the new XeLL code.

Do not move P5-B around the XeLL work.

---

## 33. Review checklist

A reviewer should explicitly verify all of the following:

- `DestroyXeLLContext()` returns `false` on any non-success vendor result.
- Failed XeLL destroy restores the old handle and sets XeLL quarantine.
- `CreateContext()` refuses to create while XeLL quarantine is active.
- Old-context destroy failure stops before `xellD3D12CreateContext`.
- New XeLL context is created into a local candidate, not directly into `_xellContext`.
- New context is committed only on exact success and non-null output.
- `XeFG_Dx12::CreateSwapchainContext()` checks the boolean create result.
- New XeFG creation is rejected early while XeLL quarantine is active.
- `SetLatencyReduction` occurs before optional fakenvapi publication.
- fakenvapi clear is expected-old/context-specific, never unconditional.
- If a known-own publication cannot be safely removed, XeLL is retained and quarantined.
- XeFG remains destroyed before XeLL.
- `_swapChainContext == nullptr` cannot hide a quarantined XeLL lifecycle.
- P5-B code is unchanged except for unavoidable nearby conflict resolution.
- No master low-latency architecture is imported.
- No P7 synchronization/queue work is mixed in.

---

## 34. Definition of Done

P6 is complete when all of the following are true:

1. XeLL module presence and XeLL context-API readiness are no longer conflated.
2. XeLL create uses a local candidate and validates exact success plus non-null output.
3. XeLL destroy returns the real lifecycle result.
4. Failed/uncertain XeLL retirement retains the old handle and blocks recreation.
5. XeFG caller honors XeLL create and destroy failures.
6. A stale retained XeLL context cannot be rebound to a new XeFG context.
7. fakenvapi publication occurs only after successful XeFG latency binding.
8. A published XeLL context is safely removed before destruction, or destruction is blocked/quarantined.
9. Intel-required XeFG-before-XeLL destroy order is preserved.
10. P5-B final-proxy handoff remains unchanged and functional.
11. Release x64 build and formatting validation pass.
12. Failure-injection cases prove that destroy/create/unpublish failures are fail-closed rather than silently reused.
13. MHW/DD2 current REF + XeFG runtime smoke tests do not regress.

---

## 35. Follow-up after P6

Do not extend P6 beyond the scope above.

The next lifecycle work remains:

```text
P7-A — external re-entry / REFramework hook-monitor mutex vs Opti FG mutex ordering
P7-B — D3D12 command-queue lifetime ownership
```

Those concerns are independent of the XeLL context correctness addressed here and should remain separate PRs/work orders.
