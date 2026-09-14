# Release 0.9 P5-B Work Order — XeFG Final Public Proxy Pre-Retire Handoff

Date: 2026-09-15

## Status

Implementation work order.

This is the next lifecycle-hardening step after P5-A. P5-A is already merged into the forked REFramework and fixes Destroy-result reconciliation semantics. P5-B addresses a separate cross-project lifetime boundary: REFramework must stop borrowing and hooking the active XeFG presentation swapchain before OptiScaler consumes the final public XeFG proxy COM reference.

P5-B is intentionally split into two small PRs and must be rolled out in this order:

1. **P5-B1 — REFramework:** add a dormant, versioned pre-retire handoff export and reuse the existing XeFG runtime-detach implementation.
2. **P5-B2 — OptiScaler `reframework-0.9`:** detect that export and call it immediately before the current lifecycle consumes the final public XeFG proxy reference.

Do not combine P5-B with XeLL/P6, command-queue ownership/P7, FG mutex redesign/P7, PR17/PR18 experiments, or broad master backports.

---

## 1. Verified remote baselines

This work order was prepared against the current remote heads below.

### REFramework target

Repository: `onehoon/REFramework`

Branch: `master`

Commit:

```text
28c0002c6497eb8465ee14098d8367145882f581
Require exact success for XeFG Destroy reconciliation (#53)
```

This includes P5-A. Current Destroy reconciliation is lifecycle-complete only for exact result `0`.

### OptiScaler target

Repository: `onehoon/OptiScaler`

Branch: `reframework-0.9`

Commit:

```text
7b19ecbefec9dc872b11d3e3f2e59e018fa278bc
P4: finish release/0.9 swapchain ownership and generation hardening
```

### OptiScaler master reference only

Current fork master was also inspected:

```text
64874932da7ceb3475ed1e8fd8aaf837249f022a
```

Master is **not** the implementation target and is **not** a solution to P5-B. Its current XeFG final-release path also consumes the final public proxy reference before `DestroySwapchainContext()`. Do not copy that ordering into `reframework-0.9` as a supposed fix.

---

## 2. Objective

Establish a narrow cross-project contract with this invariant:

> If the current XeFG public swapchain proxy is about to lose its final OptiScaler-held COM reference, a compatible REFramework must first detach every active XeFG renderer/vtable relationship that can still touch the corresponding runtime presentation lifecycle.

The required sequence is:

```text
Opti detects confirmed final public-proxy release
    |
    | proxy is still alive
    v
REF pre-retire handoff
    -> validate current XeFG runtime identity
    -> mark detached/uncertain lifecycle
    -> reset renderer state
    -> remove Present/Resize hooks
    -> clear borrowed XeFG binding
    -> return safe-to-retire
    |
    v
Opti consumes final public proxy COM ref
    |
    v
Intel xefgSwapChainDestroy
    |
    +-- result == 0  -> P5-A reconciles and clears detached state
    |
    `-- result != 0  -> detached/uncertain state remains quarantined
```

P5-B must **not** reorder Intel `xefgSwapChainDestroy()` ahead of the final proxy release. The goal is to retire REF's borrowed relationship earlier, not to rewrite Intel's lifecycle contract.

---

## 3. Code-proven current failure window

### 3.1 OptiScaler deliberately holds the proxy alive at refcount 1 while detecting the final release

Current `OptiScaler/hooks/FG_Hooks.cpp` does the following in `FGHooks::hkFGRelease`:

```cpp
This->AddRef();

const auto releaseResult = o_FGRelease(This);
if (releaseResult == 1)
{
    // The caller's release was the final external release.
    // The temporary AddRef above is now the remaining reference.
    ...
    xefg->ReleaseSwapchainFromFinalProxyRelease(
        _hwnd,
        This,
        [This]() { o_FGRelease(This); });
}
```

The important lifetime fact is:

```text
before temporary AddRef       : N refs
This->AddRef()                : N + 1
caller-equivalent Release     : returns 1
remaining refcount            : 1
```

Therefore, when `releaseResult == 1`, OptiScaler has a precise safe window where the public XeFG proxy is definitely still alive and the final remaining reference has not yet been consumed.

### 3.2 Current 0.9 release ordering consumes that reference before Intel Destroy

Current `XeFG_Dx12::ReleaseSwapchainLocked(...)` does:

```cpp
if (_fgContext != nullptr)
    DestroyFGContext();

if (releaseFinalProxy)
    releaseFinalProxy();

if (!State::Instance().isShuttingDown && _swapChainContext != nullptr)
{
    if (!DestroySwapchainContext())
        ...;
}
```

So the current order is:

```text
DestroyFGContext
    -> final public proxy Release (1 -> 0 is possible here)
    -> xefgSwapChainDestroy
```

Once that final `Release()` returns, OptiScaler must assume the public proxy object may already be destroyed.

### 3.3 REFramework intentionally stores the active XeFG swapchain as borrowed state

Current `src/compatibility/xefg/XeFGBinding.hpp` contains:

```cpp
IDXGISwapChain3* m_swapchain{}; // borrowed; hook lifetime is bounded by the caller
```

Queue and device ownership use `ComPtr`, but the active swapchain relationship is deliberately borrowed.

### 3.4 Current REF Destroy detach assumes the borrowed object is still live enough to AddRef

Current `D3D12Hook::detach_xefg_binding_for_runtime_transition(...)` begins semantic detach and then creates a bounded keepalive:

```cpp
m_xefg_session.begin_runtime_detach(evaluation, reason);

// The active binding is borrowed. Keep the proxy alive only through the
// renderer reset and instance-hook removal below.
Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive = evaluation.binding.swapchain;
```

It then performs renderer reset and physical hook removal:

```cpp
if (g_framework != nullptr)
    g_framework->on_reset();

clear_xefg_resize_transition_hold("runtime_transition");
m_present_hook.reset();
m_swapchain_hook.reset();
...
m_xefg_session.complete_runtime_detach();
```

When normal XeFG Destroy is intercepted, `XeFGCompatibility::dispatch_destroy(...)` currently calls:

```cpp
prepare_for_xefg_runtime_transition(
    slot,
    context,
    nullptr,
    false,
    "destroy");

const auto result = original(context);
```

That means the current cross-project sequence can become:

```text
Opti:
  final public proxy Release -> possible refcount 0 / object destruction

Opti:
  xefgSwapChainDestroy(context)

REF Destroy hook:
  prepare_for_xefg_runtime_transition(...)
    -> binding still contains borrowed swapchain relationship
    -> ComPtr old_keepalive = binding.swapchain
    -> AddRef on lifetime that may already have ended
```

This is the P5-B bug. The unsafe operation is not speculative policy logic; the release-before-REF-detach ordering is directly visible in the current code.

---

## 4. Important identity nuance: do not require public-proxy pointer equality

P5-B must **not** assume that the swapchain pointer stored by REF is numerically identical to the public proxy pointer returned to OptiScaler.

Current REF discovery captures the swapchain created inside Intel's `InitFromSwapChainDesc` transaction by temporarily hooking `IDXGIFactory2::CreateSwapChainForHwnd`. `XeFGDiscovery::build_binding_candidate(...)` then binds that candidate. Separately, `dispatch_get_swapchain(...)` observes the public pointer returned by `xefgSwapChainD3D12GetSwapChainPtr` and even logs whether it is the same pointer as the internally observed candidate.

Therefore the authoritative cross-project identity for P5-B is:

1. **XeFG runtime context** — exact match required when an active REF XeFG binding exists.
2. **HWND** — match when both sides have a non-null value.
3. **REF's stored runtime slot** — use REF's own slot when executing the existing detach path.
4. **Public proxy pointer** — pass for lifetime/diagnostics, but do not require numeric equality with `XeFGBinding::swapchain()`.

This avoids a false negative when Intel exposes a public proxy that wraps or aliases a different internal presentation swapchain object.

---

# P5-B1 — REFramework pre-retire handoff export

## 5. Scope

Repository: `onehoon/REFramework`

Base: current `master`

Recommended PR title:

```text
XeFG P5-B1: add final proxy pre-retire handoff
```

Expected files:

- `src/compatibility/xefg/XeFGCompatibility.hpp`
- `src/compatibility/xefg/XeFGCompatibility.cpp`
- optionally `src/D3D12Hook.hpp/.cpp` only if a small helper is needed for clean state inspection

Avoid adding a new source file unless it materially improves ownership. The REFramework CMake target already includes `src/**.cpp`, so no build-system change should be required for a helper added to an existing source file.

## 6. Add a versioned C ABI

Use a versioned export so OptiScaler can detect capability without linking against REFramework.

Recommended ABI:

```cpp
extern "C" __declspec(dllexport)
uint32_t WINAPI REFramework_XeFG_PreRetireSwapchainV1(
    IUnknown* public_proxy,
    void* xefg_context,
    HWND hwnd) noexcept;
```

Recommended stable result values:

```cpp
enum class XeFGProxyRetireStatus : uint32_t {
    SafeNotTracked = 0,
    SafeDetached = 1,
    Blocked = 2,
};
```

Semantic contract:

| Result | Meaning | Opti action |
| --- | --- | --- |
| `SafeNotTracked` | REF has no live borrowed XeFG binding that requires detach | Continue existing final-release path |
| `SafeDetached` | Matching REF XeFG lifecycle was detached while the proxy was alive | Continue existing final-release path |
| `Blocked` | REF found a state disagreement or could not safely detach | Do not consume the final proxy reference; quarantine recreation |
| Unknown future value | Treat as unsafe | Same as `Blocked` |

Do not return a raw C++ enum across the ABI. Export `uint32_t` and keep the numeric values fixed for V1.

REFramework already uses `extern "C"` plus `__declspec(dllexport)` in `src/Main.cpp`, so this follows an existing export style.

## 7. REF pre-retire implementation

Add a public compatibility entry point such as:

```cpp
class XeFGCompatibility {
public:
    enum class ProxyRetireStatus : uint32_t {
        SafeNotTracked = 0,
        SafeDetached = 1,
        Blocked = 2,
    };

    static ProxyRetireStatus prepare_for_public_proxy_retire(
        IUnknown* public_proxy,
        void* xefg_context,
        HWND hwnd) noexcept;
};
```

The exact type location may be adjusted to local style, but the exported numeric ABI must remain stable.

### Required algorithm

Conceptual implementation:

```cpp
XeFGCompatibility::ProxyRetireStatus
XeFGCompatibility::prepare_for_public_proxy_retire(
    IUnknown* public_proxy,
    void* xefg_context,
    HWND hwnd) noexcept
{
    if (public_proxy == nullptr || xefg_context == nullptr) {
        return ProxyRetireStatus::Blocked;
    }

    // Opti still owns the final reference while this function executes.
    // Take one bounded extra reference so the public proxy cannot disappear
    // during renderer reset / hook removal if re-entrant COM activity occurs.
    Microsoft::WRL::ComPtr<IUnknown> public_proxy_keepalive{public_proxy};

    if (g_framework == nullptr) {
        return ProxyRetireStatus::SafeNotTracked;
    }

    std::scoped_lock lifecycle_lock{
        g_framework->get_hook_monitor_mutex()};

    auto* hook = D3D12Hook::current_xefg_handoff_target();
    if (hook == nullptr) {
        return ProxyRetireStatus::SafeNotTracked;
    }

    const auto binding = hook->get_xefg_lifecycle_snapshot();
    if (!binding.active) {
        // There is no borrowed active binding left to touch the retiring
        // presentation lifecycle. An already-detached/uncertain state is safe
        // here; P5-A will reconcile it when Destroy returns.
        return ProxyRetireStatus::SafeNotTracked;
    }

    // P5-B is called only for Opti's current final proxy. If REF still has an
    // active XeFG binding but it belongs to another runtime context, the two
    // state machines disagree. Do not guess and do not detach the other context.
    if (binding.runtime.slot == XeFGBinding::kInvalidRuntimeSlot
        || binding.runtime.context == nullptr
        || binding.runtime.context != xefg_context
        || (hwnd != nullptr
            && binding.runtime.hwnd != nullptr
            && binding.runtime.hwnd != hwnd)) {
        return ProxyRetireStatus::Blocked;
    }

    XeFGCandidateHandoff::discard_pending_for_runtime_transition(
        binding.runtime.slot,
        binding.runtime.context,
        binding.runtime.hwnd,
        false,
        "proxy_retire");

    const bool detached = hook->detach_xefg_binding_for_runtime_transition(
        binding.runtime.slot,
        binding.runtime.context,
        binding.runtime.hwnd,
        false,
        "proxy_retire");

    return detached
        ? ProxyRetireStatus::SafeDetached
        : ProxyRetireStatus::Blocked;
}
```

Important implementation notes:

- `public_proxy_keepalive` is only a bounded lifetime guard. **Do not compare its pointer to `binding.swapchain` as a mandatory identity check.**
- The active REF runtime identity is the matching authority.
- Use REF's stored `binding.runtime.slot` to call the existing detach path. OptiScaler does not need to know REF's runtime registry slot.
- Reuse `detach_xefg_binding_for_runtime_transition(...)`; do not duplicate renderer reset, hook removal, alias cleanup, or detached-state creation.
- Reuse `XeFGCandidateHandoff::discard_pending_for_runtime_transition(...)` so a pending candidate for the retiring runtime cannot be applied after the handoff.
- Use exact runtime matching; `allow_same_hwnd_match` must be `false` for P5-B.

### Export wrapper

Conceptual wrapper at file scope:

```cpp
extern "C" __declspec(dllexport)
uint32_t WINAPI REFramework_XeFG_PreRetireSwapchainV1(
    IUnknown* public_proxy,
    void* xefg_context,
    HWND hwnd) noexcept
{
    return static_cast<uint32_t>(
        XeFGCompatibility::prepare_for_public_proxy_retire(
            public_proxy,
            xefg_context,
            hwnd));
}
```

If an exception boundary is considered necessary, fail closed:

```cpp
try {
    ...
} catch (...) {
    return static_cast<uint32_t>(
        XeFGCompatibility::ProxyRetireStatus::Blocked);
}
```

Do not allow an exception to propagate across the C ABI.

## 8. Preserve P5-A detached-state semantics

A successful pre-retire detach must leave REF in the same semantic state that an ordinary pre-Destroy detach would create:

```text
active binding
    -> begin_runtime_detach("proxy_retire")
    -> physical hook / renderer relationship removed
    -> m_binding.clear()
    -> detached_state.active remains true
```

Do **not** clear detached state in the pre-retire export.

Later, when Opti calls Intel Destroy, the existing REF Destroy interceptor runs. Because the active binding has already been cleared, it must not try to AddRef the old borrowed swapchain again. After the vendor call:

```text
result == 0
  -> P5-A evaluate_destroy_result accepts
  -> commit_destroy_reconciliation
  -> detached state cleared

result > 0 or result < 0
  -> P5-A does not accept
  -> detached/uncertain state remains
  -> generic recovery remains suppressed/quarantined
```

This coupling between P5-B and already-merged P5-A is intentional.

## 9. REF logging

Keep logs lifecycle-specific and low noise.

Suggested successful handoff log:

```cpp
spdlog::info(
    "[XeFG][ProxyRetire] action = detached, "
    "public_proxy = 0x{:x}, binding_swapchain = 0x{:x}, "
    "context = 0x{:x}, generation = {}, runtime_slot = {}",
    reinterpret_cast<uintptr_t>(public_proxy),
    reinterpret_cast<uintptr_t>(binding.swapchain),
    reinterpret_cast<uintptr_t>(binding.runtime.context),
    binding.generation,
    runtime_slot_for_log(binding.runtime.slot));
```

Suggested blocked reasons:

```text
invalid_argument
runtime_identity_mismatch
detach_failed
```

`SafeNotTracked` should normally be debug-level only. It is a valid compatibility case, not an error.

## 10. P5-B1 forbidden changes

Do not:

- call Intel `xefgSwapChainDestroy` from the export;
- release the Opti-owned public proxy from REF;
- change generic XeFG result semantics;
- change P5-A exact-success semantics;
- make the borrowed swapchain globally owned by REF;
- replace the existing detach implementation with a second teardown implementation;
- use same-HWND matching as a substitute for exact runtime identity;
- modify Present/Resize policy;
- redesign hook-monitor recovery;
- change XeLL lifecycle;
- redesign mutex ordering;
- backport OptiScaler master architecture.

---

# P5-B2 — OptiScaler `reframework-0.9` caller

## 11. Scope

Repository: `onehoon/OptiScaler`

Base: `reframework-0.9`

Recommended PR title:

```text
P5-B2: detach REF before final XeFG proxy release
```

Primary files:

- `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`
- `OptiScaler/framegen/xefg/XeFG_Dx12.h` only if helper declarations are needed

Avoid adding a new `.cpp` for this small compatibility bridge unless necessary, because that would also require Visual Studio project/filter registration. Keeping the private helper in `XeFG_Dx12.cpp` minimizes build-system surface.

## 12. Discover the REF capability dynamically

OptiScaler must not add a link-time dependency on REFramework and must not load REFramework itself.

Do **not** depend solely on:

```cpp
GetModuleHandleW(L"dinput8.dll")
```

because a process can contain the system DirectInput module as well as the proxy module, and basename assumptions are not a strong capability check.

Instead, enumerate already-loaded modules and search for the unique versioned export.

Conceptual helper:

```cpp
#include <tlhelp32.h>

namespace {

using RefXeFGPreRetireFn =
    uint32_t (WINAPI*)(IUnknown*, void*, HWND);

enum class RefXeFGProxyRetireStatus : uint32_t {
    SafeNotTracked = 0,
    SafeDetached = 1,
    Blocked = 2,
};

enum class RefHandoffDecision {
    NotAvailable,
    Safe,
    Blocked,
};

RefXeFGPreRetireFn FindREFXeFGPreRetire() noexcept
{
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        GetCurrentProcessId());

    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    RefXeFGPreRetireFn found = nullptr;

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            auto* proc = GetProcAddress(
                entry.hModule,
                "REFramework_XeFG_PreRetireSwapchainV1");

            if (proc != nullptr)
            {
                found = reinterpret_cast<RefXeFGPreRetireFn>(proc);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

RefHandoffDecision PrepareREFForXeFGProxyRetire(
    IUnknown* publicProxy,
    void* context,
    HWND hwnd) noexcept
{
    const auto fn = FindREFXeFGPreRetire();
    if (fn == nullptr)
        return RefHandoffDecision::NotAvailable;

    const uint32_t status = fn(publicProxy, context, hwnd);

    switch (status)
    {
    case static_cast<uint32_t>(RefXeFGProxyRetireStatus::SafeNotTracked):
    case static_cast<uint32_t>(RefXeFGProxyRetireStatus::SafeDetached):
        return RefHandoffDecision::Safe;

    case static_cast<uint32_t>(RefXeFGProxyRetireStatus::Blocked):
    default:
        // Unknown future ABI values are unsafe for V1 callers.
        return RefHandoffDecision::Blocked;
    }
}

} // namespace
```

Do not cache an unavailable result forever. A negative cache can become stale if module load ordering changes. Since final proxy retirement is rare, per-retirement module enumeration is acceptable and simpler.

A positive function pointer may be cached only if module lifetime is known to be stable; for P5-B2, no cache is required.

## 13. Call the handoff at the current-lifecycle boundary

The preferred call site is inside `XeFG_Dx12::ReleaseSwapchainFromFinalProxyRelease(...)`, **after** the existing P4 stale-proxy check and **before** `ReleaseSwapchainLocked(...)` reaches `releaseFinalProxy()`.

Current P4 guard:

```cpp
if (state.currentFGSwapchain != finalProxy)
{
    LOG_DEBUG(
        "[XeFG][Lifecycle] action = stale_final_proxy_release_only, ...");
    releaseFinalProxyOnce();
    return true;
}
```

Leave that behavior unchanged.

Immediately after that current-lifecycle confirmation, add the optional REF handoff:

```cpp
const auto refHandoff = PrepareREFForXeFGProxyRetire(
    finalProxy,
    _swapChainContext,
    hwnd);

if (refHandoff == RefHandoffDecision::Blocked)
{
    _swapchainRecreationBlocked = true;

    LOG_ERROR(
        "[XeFG][Lifecycle] action = final_proxy_release_blocked, "
        "reason = ref_pre_retire_handoff_failed, "
        "proxy = {:X}, context = {:X}, hwnd = {:X}",
        (size_t) finalProxy,
        (size_t) _swapChainContext,
        (size_t) hwnd);

    // Important: return before ReleaseSwapchainLocked and before the
    // finalProxyReleased fallback. Keep the remaining COM ref alive.
    return false;
}

if (refHandoff == RefHandoffDecision::Safe)
{
    LOG_INFO(
        "[XeFG][Lifecycle] action = ref_pre_retire_handoff_complete, "
        "proxy = {:X}, context = {:X}",
        (size_t) finalProxy,
        (size_t) _swapChainContext);
}
else
{
    LOG_DEBUG(
        "[XeFG][Lifecycle] action = ref_pre_retire_handoff_skipped, "
        "reason = export_unavailable");
}

const bool releaseSucceeded =
    ReleaseSwapchainLocked(hwnd, releaseFinalProxyOnce);
```

### Why this location is important

Do not call the handoff after `releaseFinalProxyOnce()`; that is too late.

Do not call it from generic `DestroySwapchainContext()`; that can also be reached from initialization-abort/shutdown paths where the public final-proxy contract is different.

Do not call it for the P4 stale-final-proxy branch; P5-B is specifically the current lifecycle's final-proxy boundary.

Do not call it from `hkFGRelease` before the lifecycle mutex/current-proxy verification unless a later code review proves the in-function placement cannot be made safe. The existing P4 current-vs-stale check is the correct serialization boundary to preserve.

## 14. Fail closed only when the compatible export is present and refuses detach

Compatibility behavior must be asymmetric:

```text
export absent
  -> old REF / no REF / stock environment
  -> preserve existing behavior
  -> do not block release

export present + SafeNotTracked/SafeDetached
  -> proceed

export present + Blocked/unknown
  -> do not consume final proxy ref
  -> set recreation quarantine
  -> return false
```

This is important for rollout safety. P5-B2 must not make REFramework mandatory.

When blocked, the remaining Opti temporary reference intentionally survives. `hkFGRelease` already treats a failed `ReleaseSwapchainFromFinalProxyRelease` as an aborted lifecycle and returns `0`. A rare fail-closed COM leak/quarantine is safer than releasing an object while REF may still hold a borrowed/hooked relationship to it.

Do not fall through to this existing fallback after a REF `Blocked` result:

```cpp
if (!finalProxyReleased)
{
    _swapchainRecreationBlocked = true;
    releaseFinalProxyOnce();
    ...
}
```

The REF-blocked path must return **before** that fallback, otherwise P5-B would report failure and then still destroy the proxy it was trying to preserve.

## 15. Do not move the handoff under the FG Present mutex

At the recommended call site, `_swapchainLifecycleMutex` is already held, but `ReleaseSwapchainLocked()` has not yet acquired the optional XeFG `Mutex` owner `1`.

Keep the REF callback before that FG mutex acquisition.

Reason:

- the REF handoff acquires `REFramework::m_hook_monitor_mutex`;
- the existing P7 analysis already identifies a separate potential ordering inversion involving REF's hook-monitor mutex and Opti's FG mutex;
- P5-B must not worsen that by deliberately invoking REF while holding the FG mutex.

P5-B does **not** solve the broader P7 lock-order issue. It only keeps this new callback out of the known risky lock order.

If implementation review discovers a reverse dependency involving `_swapchainLifecycleMutex`, stop and isolate that as a concrete review finding rather than silently moving the callback under the FG mutex.

## 16. P5-B2 logging

Suggested logs:

Export unavailable — debug only:

```text
[XeFG][Lifecycle] action = ref_pre_retire_handoff_skipped, reason = export_unavailable
```

Compatible handoff accepted:

```text
[XeFG][Lifecycle] action = ref_pre_retire_handoff_complete, proxy = ..., context = ...
```

Compatible REF refused/failed:

```text
[XeFG][Lifecycle] action = final_proxy_release_blocked, reason = ref_pre_retire_handoff_failed, ...
```

Do not warn just because REF is absent. Absence is a supported environment.

---

## 17. Required end-to-end ordering

For a current XeFG lifecycle with new REF + new OptiScaler, runtime diagnostics must demonstrate this order:

```text
1. Opti hkFGRelease detects releaseResult == 1
2. Opti enters ReleaseSwapchainFromFinalProxyRelease
3. Opti confirms finalProxy == currentFGSwapchain
4. Opti calls REFramework_XeFG_PreRetireSwapchainV1
5. REF validates matching runtime context
6. REF begin_runtime_detach(reason=proxy_retire)
7. REF renderer reset completes
8. REF Present/Resize instance hooks are removed
9. REF borrowed binding is cleared
10. REF export returns SafeDetached
11. Opti DestroyFGContext
12. Opti releaseFinalProxy -> final COM ref may reach 0
13. Opti xefgSwapChainDestroy(context)
14. REF Destroy interceptor sees no active borrowed binding requiring AddRef
15. Intel Destroy returns
16a. result == 0  -> P5-A clears detached state
16b. result != 0  -> detached uncertainty remains quarantined
```

The key acceptance property is:

> No REF operation may perform `QueryInterface`, `AddRef`, vtable-hook removal, or renderer access through the old borrowed XeFG presentation swapchain after Opti has consumed the final public-proxy reference.

---

## 18. Compatibility matrix

| OptiScaler | REFramework | Expected behavior |
| --- | --- | --- |
| old | old | Existing behavior |
| old | P5-B1/new | Export is dormant; existing behavior |
| P5-B2/new | absent | Export not found; existing non-REF behavior |
| P5-B2/new | old | Export not found; existing compatibility behavior, P5-B protection unavailable |
| P5-B2/new | P5-B1/new | Pre-retire detach active; P5-B protection enabled |

This is why **B1 must be merged/released before B2** where practical.

---

## 19. Validation — P5-B1 / REFramework

### Source review

Confirm:

1. export name is exactly versioned and stable;
2. no pointer-equality requirement exists between `public_proxy` and REF's internally observed swapchain;
3. active REF binding with matching context uses the existing runtime-detach path;
4. active binding with mismatched runtime context fails closed;
5. inactive/no hook state returns safe without fabricating a detach;
6. successful pre-retire detach leaves `detached_state.active` intact;
7. Intel Destroy is not called by the export;
8. public proxy ownership is not transferred to REF.

### Build/audit

Run the fork's normal checks:

```text
git diff --check
python dev/audit_direct_access_clang.py
cmake --preset vs2026
cmake --build build --config Release -- /m:1 /v:minimal
```

Verify the actual DLL export:

```bat
dumpbin /exports build\bin\REFramework\dinput8.dll | findstr REFramework_XeFG_PreRetireSwapchainV1
```

Adjust the output path to the active preset if necessary.

### Dormant behavior check

Run current/old OptiScaler against P5-B1 REF and confirm no new pre-retire code executes because nothing calls the export.

---

## 20. Validation — P5-B2 / OptiScaler

The current `reframework-0.9` unsigned DLL workflow builds Release x64 with:

```text
msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
```

At minimum run:

```text
git diff --check
Release x64 build
existing formatting / CI checks for the branch
```

Source review must confirm:

1. no link-time REFramework dependency was added;
2. Opti does not `LoadLibrary` REFramework;
3. export absence is non-fatal;
4. unknown export return values fail closed;
5. the REF callback occurs before `releaseFinalProxyOnce()`;
6. REF `Blocked` returns before the existing `!finalProxyReleased` fallback;
7. stale final proxy handling remains P4 behavior;
8. generation cleanup remains unchanged;
9. Intel Destroy ordering remains final-proxy-release then Destroy;
10. P5-B does not modify XeLL ownership or low-latency architecture.

---

## 21. Runtime validation matrix

Runtime validation should focus on lifecycle transitions, not only steady-state frame generation.

Recommended target order:

### A. New REF + new OptiScaler + Intel XeFG

Exercise repeated final swapchain retirement/recreation through practical triggers such as:

- game/window resolution change that recreates the swapchain;
- borderless/fullscreen transition where the title actually recreates it;
- FG off/on lifecycle where the public XeFG swapchain is retired;
- scene/device path that is already known to recreate the XeFG swapchain;
- clean game exit as a secondary check.

Verify log ordering from section 17.

### B. REF absent

Run the same Opti build without REFramework and confirm:

- no warning/error solely because the export is missing;
- swapchain final release continues through the existing path;
- no new hard dependency exists.

### C. Old REF

Run P5-B2 Opti against a REF build without the export and confirm the same fallback behavior as B.

### D. Destroy result handling

If a positive or negative Destroy result can be reproduced or safely injected in a diagnostic build:

```text
0   -> detached state reconciles/clears through P5-A
+N  -> detached state remains uncertain/quarantined
-N  -> detached state remains uncertain/quarantined
```

Do not change P5-A semantics to make P5-B testing easier.

### E. Game smoke targets

Where practical, include at least:

- Monster Hunter Wilds / XeFG lifecycle recreation path;
- Dragon's Dogma 2 / XeFG lifecycle path;
- one native/non-XeFG REF launch to confirm the export is inert outside XeFG.

Runtime tests are evidence gates, not permission to broaden P5-B into unrelated game-specific fixes.

---

## 22. Failure-injection checks

Before merge, it is useful to temporarily exercise these cases in a diagnostic build:

### Export reports Blocked

Force the REF export to return `Blocked` and verify:

```text
Opti does not call releaseFinalProxyOnce
Opti sets recreation quarantine
Opti returns lifecycle failure
public proxy remaining ref is preserved
Intel Destroy is not entered through this final-release transaction
```

Remove the injection before commit.

### Runtime identity mismatch

Temporarily alter the supplied context and verify REF returns `Blocked` rather than detaching another binding.

### No active REF binding

Ensure `SafeNotTracked` allows Opti's normal release to proceed.

### Already detached state

If REF has no active binding because an earlier runtime transition already detached it, the export must not recreate or rebind anything. It may safely return `SafeNotTracked`; the later Destroy result remains responsible for P5-A reconciliation.

---

## 23. Review checklist

A reviewer should be able to answer **yes** to every item before P5-B is considered complete.

### Cross-project contract

- Is REF detached while Opti still owns a live final public-proxy reference?
- Is runtime context, not public/internal pointer equality, the authoritative lifecycle match?
- Does REF reuse its existing detach implementation?
- Does P5-A remain responsible for final detached-state reconciliation?

### REF

- Is the ABI versioned?
- Is it dormant until called?
- Does it avoid Intel Destroy and proxy ownership?
- Does it fail closed on active-runtime identity disagreement?
- Does it avoid same-HWND-only detach for P5-B?

### OptiScaler

- Is REFramework optional at runtime?
- Is export discovery capability-based rather than a hard DLL-name dependency?
- Does `Blocked` preserve the final reference instead of releasing anyway?
- Is the callback before final proxy release and before the optional FG mutex acquisition?
- Is stale-proxy P4 behavior preserved?
- Is generation cleanup preserved?

### Scope

- No XeLL/P6 work?
- No queue ownership/P7 work?
- No broad lock-order redesign/P7 work?
- No Opti master lifecycle transplant?
- No PR17/PR18 experimental behavior?

---

## 24. Non-goals

P5-B does not attempt to solve:

- XeLL create/destroy fail-closed semantics — P6;
- XeLL partial initialization poisoning — P6;
- D3D12 command queue raw ownership — P7;
- general REF hook-monitor mutex vs Opti FG mutex inversion — P7;
- shutdown-only XeFG context cleanup policy;
- generic COM ownership refactor;
- all possible third-party overlay hook conflicts;
- OptiScaler master parity.

Do not broaden either P5-B PR to include these.

---

## 25. Why not simply Destroy Intel context first?

A tempting alternative is:

```text
xefgSwapChainDestroy
    -> final proxy Release
```

Do not implement that in P5-B.

Reasons:

1. It changes the vendor lifecycle ordering rather than fixing REF's borrowed-lifetime boundary.
2. Current `reframework-0.9` and current Opti master both release the final proxy before Destroy, so there is no validated in-tree evidence that reversing the vendor order is safe.
3. P5-B can remove the UAF window without changing Intel ordering at all.
4. A vendor-order rewrite would have a much larger regression surface and would require separate Intel runtime validation.

The narrow fix is therefore:

```text
REF detach first
    -> existing Opti final Release
    -> existing Intel Destroy
```

---

## 26. Definition of done

P5-B is complete when all of the following are true:

1. New REFramework exports `REFramework_XeFG_PreRetireSwapchainV1`.
2. The export safely detaches a matching active XeFG runtime using existing REF lifecycle code while the public proxy is still alive.
3. `reframework-0.9` discovers the export dynamically and invokes it only for the current final XeFG proxy lifecycle.
4. REF absence or an old REF build remains supported with no new hard dependency.
5. A compatible REF `Blocked` result prevents Opti from consuming the final proxy reference and quarantines recreation.
6. The final public proxy is still released before Intel Destroy when the handoff is safe.
7. The later REF Destroy hook no longer needs to create a keepalive from a borrowed XeFG swapchain after the public proxy may have reached refcount zero.
8. P5-A exact-success semantics clear detached state only after a confirmed successful Destroy.
9. Existing P1-P4 generation/ownership hardening is preserved.
10. P6/P7 concerns remain out of scope.

Intended final lifecycle:

```text
confirmed final proxy release
    -> REF pre-retire detach while object is live
    -> Opti final proxy Release
    -> Intel Destroy
    -> P5-A exact-success reconciliation
```

That is the entire P5-B contract. Keep both PRs narrow enough that this ordering can be reviewed directly from the diff.