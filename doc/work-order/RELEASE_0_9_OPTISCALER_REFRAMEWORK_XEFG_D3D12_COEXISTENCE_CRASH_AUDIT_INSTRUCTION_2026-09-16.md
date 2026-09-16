# Release 0.9 — OptiScaler + REFramework XeFG / D3D12 Coexistence Crash Audit Instruction

**Status:** Independent two-repository crash-safety audit instruction — audit first, no implementation authorized  
**Date:** 2026-09-16 KST  
**Primary target:** Capcom DX12 games using forked REFramework + OptiScaler + XeFG  
**OptiScaler repository:** `onehoon/OptiScaler`  
**OptiScaler branch:** `reframework-0.9`  
**OptiScaler audited branch tip at instruction authoring:** `b9942bcfeddce67c401b21e92e6499612283c70e`  
**OptiScaler code baseline:** `036aae89e31447b21e1cab4e8d2c5c72421f9dc7` (`fix: harden DX12 wrapper COM ownership (#38)`)  
**Reason code baseline differs from branch tip:** commits after `036aae89...` through `b9942bcf...` are audit documentation only  
**REFramework repository:** `onehoon/REFramework`  
**REFramework branch:** `master`  
**REFramework audited source baseline:** `e4a1a1d1faf06f4e25923dacf765c00213c86ada` (`Fix XeFG factory lock inversion with split REF monitor scope (#56)`)  
**Important REFramework predecessor:** `79b29c073eae08e818b5f220f01f25eb6382f086` (`Fix late D3D12 callbacks after XeFG pre-retire detach (#55)`)  
**Historical REFramework runtime build seen in existing MHW logs:** `c6704372c5808ea0c49059a23afa4ee12cc39e8d` — runtime evidence only, not the source baseline

---

## 1. Mission

Perform a **complete two-repository source audit** for **game-crash-capable coexistence defects caused specifically by OptiScaler and the forked REFramework jointly participating in the same D3D12 / DXGI / XeFG swapchain lifecycle**.

The audit is deliberately narrow.

The central question is:

> **Can the current OptiScaler and forked REFramework implementations, while both are active in the XeFG D3D12 path, reach an ordering, lifetime, hook-chain, reentrancy, or teardown state that causes a game crash, fatal D3D failure, use-after-free, invalid COM call, double release, stale trampoline call, deadlock/hang, or stale queue/device/swapchain dereference?**

Do **not** audit REFramework as a general mod framework. Do **not** audit OptiScaler as a general frame-generation implementation. Audit only the boundary where the two coexist for XeFG on D3D12.

Do **not** begin by implementing a fix. First produce a complete audit report that proves or disproves every candidate interleaving. A suspicious code shape without a reachable crash-capable path is not a finding.

---

## 2. Scope boundary — strict

### 2.1 In scope

Only code that materially participates in **OptiScaler + REFramework coexistence** for the target XeFG/D3D12 path:

#### OptiScaler

At minimum inspect:

- `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`
- `OptiScaler/framegen/xefg/XeFG_Dx12.h`
- `OptiScaler/wrapped/wrapped_swapchain.cpp`
- `OptiScaler/wrapped/wrapped_swapchain.h`
- `OptiScaler/hooks/FG_Hooks.cpp`
- `OptiScaler/hooks/FG_Hooks.h`
- relevant `D3D12_Hooks` / resource-tracking code **only where it intersects the same shared device/queue/swapchain lifecycle**
- `State::currentFGSwapchain`, `currentFG`, `currentCommandQueue`, `currentD3D12Device`, shutdown flags, swapchain generation/lifecycle aliases
- the dynamic lookup and call of `REFramework_XeFG_PreRetireSwapchainV1`
- `PrepareREFForSwapchainRetire`
- `ReleaseSwapchain`
- `ReleaseSwapchainFromFinalProxyRelease`
- `ReleaseSwapchainLocked`
- XeFG swapchain context create/destroy/recreate paths
- Opti wrapped-swapchain final `Release()` path
- CreateSwapChain / CreateSwapChainForHwnd interception and XeFG proxy creation
- Present / Present1 and ResizeBuffers / ResizeBuffers1 only where they interact with REF-owned hooks or REF-observed XeFG instances

#### REFramework

At minimum inspect:

- `src/D3D12Hook.cpp`
- `src/D3D12Hook.hpp`
- `src/compatibility/xefg/XeFGBinding.*`
- `src/compatibility/xefg/XeFGPresentationSession.*`
- `src/compatibility/xefg/XeFGCompatibility.*`
- `src/compatibility/xefg/XeFGRuntimeRegistry.*`
- `src/compatibility/xefg/XeFGCandidateHandoff.*`
- `src/compatibility/xefg/XeFGDiscovery.*`
- `src/compatibility/xefg/XeFGResizeLifecycle.*`
- any hook-monitor / renderer reset code called by those paths
- `REFramework_XeFG_PreRetireSwapchainV1`
- runtime `xefgSwapChainD3D12InitFromSwapChainDesc`, `xefgSwapChainD3D12GetSwapChainPtr`, and `xefgSwapChainDestroy` interception
- external XeFG binding and binding-generation transitions
- D3D12 instance vtable hooks for Present, Present1, ResizeBuffers, ResizeBuffers1, ResizeTarget
- factory `CreateSwapChainForHwnd` hook lifecycle
- hook-monitor recovery / rehook / quarantine only where it can mutate the same hook chain while OptiScaler is active

### 2.2 Explicitly out of scope

Do not spend audit time on:

- REFramework's normal mod features, Lua, SDK, UI, scripting, input, camera, gameplay, or renderer features except a renderer reset directly required to prove a shared XeFG lifetime path;
- generic REFramework quality or architecture;
- generic OptiScaler quality or architecture;
- D3D11;
- Vulkan;
- FSR-FG / DLSS-FG output paths except a truly shared primitive that directly participates in the target XeFG coexistence crash path;
- performance tuning;
- style, naming, cleanup, refactoring, abstraction quality, maintainability, or code organization;
- memory/resource leaks that cannot produce a crash in the audited lifetime;
- a raw pointer merely because it is raw;
- a missing lock merely because it is missing;
- SDLX-002/003/006/007 unless REFramework coexistence materially changes the reachable crash path;
- game-specific rendering correctness;
- anti-tamper behavior except where the supplied runtime evidence directly establishes that a shared Opti/REF hook/lifecycle event triggers it.

**This is not a refactoring audit.** Any recommendation whose main justification is cleaner ownership, simpler architecture, fewer raw pointers, or easier maintenance is outside scope unless the current implementation has a demonstrated crash-capable interleaving.

---

## 3. Evidence standard and finding gate

A primary finding is allowed only when **all three** are established:

1. **Reachability** — the path is source-confirmed or precisely conditional with explicit preconditions;
2. **Broken shared invariant** — identify exactly which Opti/REF hook, COM object, generation, lock, trampoline, or lifecycle invariant becomes invalid;
3. **Crash-capable consequence** — show the exact invalid operation that can produce at least one of:
   - access violation / use-after-free;
   - double `Release`, refcount underflow, or final-release reuse;
   - call through a stale or invalid COM interface;
   - call through a stale trampoline/original/vtable target;
   - stale queue/device/swapchain dereference after final release;
   - concrete deadlock or permanent hook/lifecycle hang capable of freezing/crashing the game;
   - invalid XeFG/DXGI/D3D12 lifecycle call capable of fatal D3D or device removal.

Use only these primary classifications:

- `CONFIRMED CRASH-CAPABLE DEFECT`
- `CONDITIONAL CRASH-CAPABLE DEFECT`
- `SAFE BY PROVEN INVARIANT`
- `NOT REACHABLE`
- `INSUFFICIENT EVIDENCE`

Only the first two belong in the **Primary Crash Findings** section.

Do **not** promote the following to a crash finding without a concrete invalid operation:

- "architecture is fragile";
- "ownership is unclear";
- "the pointer could theoretically become stale";
- "two modules hook the same API";
- "same device but different queue";
- "a mutex is nonrecursive";
- "there is a race window";
- "the code should use `ComPtr`";
- "this would be easier to reason about after refactoring".

If the crash consequence cannot be proven, classify it as `INSUFFICIENT EVIDENCE` or omit it from the primary findings.

---

## 4. Known current safeguards — verify them, do not rediscover old bugs as current bugs

The audit must explicitly account for the current fixes below.

### 4.1 OptiScaler PR34 — final-release reentrancy/deadlock hardening

The current wrapper final-release path was changed to prevent final release from recursively deadlocking on the wrapper-local lifecycle path. Verify the current implementation rather than reasoning from an older release.

For every new finding touching final release, state whether PR34 already blocks the proposed interleaving.

### 4.2 OptiScaler PR35 — shutdown pre-retire behavior

During process shutdown, current OptiScaler intentionally avoids the cross-module REF pre-retire call while still retiring/releasing XeFG locally.

Any shutdown finding must prove that a cross-component call still occurs after this safeguard, or prove a different stale-object path. Do not propose re-adding REF pre-retire during process detach without source proof.

### 4.3 OptiScaler PR36 / PR37 — Streamline/XeFG CPU transaction

The Streamline input transaction and Present-side owner-2 serialization were added, and PR37 removed the inner `EvaluateState` release that could prematurely unlock the caller's transaction.

This audit is not primarily about frame-resource CPU ordering. Consider these changes only when their mutex ownership intersects a REF callback or a shared swapchain lifecycle operation.

### 4.4 OptiScaler PR38 — D3D12 wrapper COM ownership

Current D3D12 wrapper code holds local queried queue/device/real-object references through use, and retains `_real1.._real4` interface references through wrapper lifetime before releasing them during final teardown.

Do not report the pre-PR38 release-before-use patterns as current defects.

### 4.5 REFramework PR55 — late D3D12 callbacks after XeFG pre-retire

Commit `79b29c073eae08e818b5f220f01f25eb6382f086` added late-callback forwarding for retired/different XeFG instances. Present, Present1, ResizeBuffers, ResizeBuffers1, and ResizeTarget callbacks can resolve the **current restored vtable slot** and forward there instead of blindly using a detached instance-hook original.

The audit must test the limits of this invariant:

- which callbacks use late forwarding;
- which shared callbacks do not;
- whether object readability checks are sufficient for the actual lifetime window;
- whether a callback can arrive after object destruction rather than merely after REF detach;
- whether forwarding can accidentally target Opti's wrapper/detour recursively;
- whether the current-vtable target can be stale because the object itself is already invalid.

Do not report the already-fixed stale instance-hook path unless it remains reachable through a different callback/order.

### 4.6 REFramework PR56 — XeFG factory lock inversion

Commit `e4a1a1d1faf06f4e25923dacf765c00213c86ada` changed active/in-flight XeFG `CreateSwapChainForHwnd` handling to:

- acquire a do-not-hook guard;
- enter the REF hook-monitor lifecycle lock;
- unhook/reset the old D3D12 path;
- record an in-flight factory transition;
- release the hook-monitor lock before calling downstream `CreateSwapChainForHwnd`;
- reacquire it after downstream return;
- suppress intermediate/background rehook during the split window;
- rehook once when the final in-flight transition exits.

Any factory deadlock finding must prove a lock cycle or stale state **after** this split-scope design.

---

## 5. Establish the actual cross-module architecture first

Before searching for bugs, draw the current combined ownership/callback graph. Do not infer it from names.

At minimum resolve these logical objects:

```text
Game / Capcom engine
    |
    +-- DXGI factory / CreateSwapChainForHwnd
    |
    +-- real application D3D12 command queue/device
    |
    +-- application-visible swapchain

OptiScaler
    |
    +-- WrappedIDXGISwapChain4
    +-- underlying real DXGI swapchain interfaces (_real, _real1.._real4)
    +-- XeFG public/proxy swapchain returned from Intel XeFG
    +-- XeFG _swapChainContext
    +-- XeFG _fgContext
    +-- State::currentFGSwapchain
    +-- State::currentCommandQueue/currentD3D12Device
    +-- XeFG owned initialization/game queue
    +-- FGHooks swapchain hooks

Intel XeFG runtime
    |
    +-- InitFromSwapChainDesc
    +-- GetSwapChainPtr
    +-- internal/presentation swapchain behavior
    +-- Destroy

REFramework
    |
    +-- XeFGRuntimeRegistry slot + FunctionHook originals
    +-- XeFGBinding runtime identity {slot, context, hwnd}
    +-- borrowed XeFG swapchain identity
    +-- owned queue/device ComPtrs
    +-- binding generation
    +-- instance VtableHook
    +-- phase-1/factory hooks
    +-- hook-monitor / rehook / quarantine state
```

For every edge, label:

- owned reference vs borrowed pointer;
- who can perform final release;
- which module can replace/clear the pointer;
- which lock, if any, is held;
- whether a callback crosses module boundaries;
- whether the edge is generation-bound.

---

## 6. Required shared-hook inventory

Build a table for every D3D12/DXGI function that both projects can touch in the target path.

At minimum resolve:

- `IDXGISwapChain::Present`
- `IDXGISwapChain1::Present1`
- `IDXGISwapChain::ResizeBuffers`
- `IDXGISwapChain3::ResizeBuffers1`
- `IDXGISwapChain::ResizeTarget`
- relevant `IUnknown::Release` / wrapper final release
- `IDXGIFactory*::CreateSwapChain*`
- `IDXGIFactory2/4::CreateSwapChainForHwnd`
- XeFG runtime `InitFromSwapChainDesc`
- XeFG runtime `GetSwapChainPtr`
- XeFG runtime `Destroy`
- D3D12 queue/device hooks only where both sides can mutate or rely on the same object lifetime.

Required columns:

| API / slot | Opti hook site and mechanism | REF hook site and mechanism | Original/trampoline storage | Install order | Reinstall/unhook path | Current invocation chain | Late-callback behavior | Crash-capable stale-target path? |
|---|---|---|---|---|---|---|---|---|

Do not assume hook order from expected module load order. Establish it from source and, where necessary, available logs.

For every shared vtable method answer:

1. What address is in the vtable before Opti hooks it?
2. What address is in the vtable before REF hooks it?
3. What does each side save as "original"?
4. What happens if the other side restores or replaces that slot later?
5. Can a saved original point to the other module's hook after that hook has been detached?
6. Does current REF late forwarding eliminate the stale path?
7. Can Opti hold an equivalent stale function pointer or vtable snapshot?
8. Can either side restore a vtable entry over the other side's still-active hook?

A stale hook finding is valid only if you identify the exact saved target, the mutation that invalidates it, and the later reachable call through it.

---

## 7. Required shared-object lifetime audit

Build a lifetime/ownership matrix for at least:

- application real swapchain;
- Opti `WrappedIDXGISwapChain4`;
- Opti-held `_real`, `_real1`, `_real2`, `_real3`, `_real4`;
- Intel XeFG public/proxy swapchain;
- Intel internal swapchain if separately observable;
- `State::currentFGSwapchain`;
- XeFG `_swapChainContext`;
- XeFG `_fgContext`;
- REF `XeFGBinding::m_swapchain` — currently documented as **borrowed**;
- REF binding queue/device — currently `ComPtr` owned;
- REF `D3D12Hook::m_swap_chain`, `m_command_queue`, `m_device` aliases;
- REF instance `VtableHook` target;
- Opti and REF queue/device aliases used during resize/recreate.

Required columns:

| Object | Creator | Initial ref owner | Borrowed/owned in Opti | Borrowed/owned in REF | Publication site | Generation identity | Retire trigger | Final Release owner | Last legal callback/use | Possible stale dereference |
|---|---|---|---|---|---|---|---|---|---|---|

COM accounting must be explicit where a crash finding depends on it. Show the relevant `QI/AddRef/Release` sequence. Do not infer ownership from a field name.

---

## 8. Audit the Opti -> REF pre-retire handshake end to end

The current Opti source dynamically discovers:

```text
REFramework_XeFG_PreRetireSwapchainV1
```

and calls it from the XeFG retire path using:

```text
(public proxy, XeFG swapchain context, hwnd)
```

The current REF export calls `XeFGCompatibility::prepare_for_public_proxy_retire`, which:

- temporarily keeps the public proxy alive with a `ComPtr`;
- takes REF's hook-monitor mutex;
- validates runtime slot/context/hwnd identity against the active binding;
- discards pending candidate state;
- asks `D3D12Hook` to detach the matching XeFG binding;
- resets REF renderer state / removes the instance relationship before returning safe-detached.

Trace the entire call stack **in both directions** and answer:

1. Which Opti locks are held when `REFramework_XeFG_PreRetireSwapchainV1` is called?
2. Which REF locks are acquired before returning?
3. Can REF's detach/reset path call DXGI, XeFG, or an Opti-owned wrapper while an Opti lifecycle lock is held?
4. Can `g_framework->on_reset()` release COM objects or execute callbacks that re-enter Opti?
5. Is the temporary REF `ComPtr` enough to keep the same physical proxy alive until every late callback is safe?
6. Is Opti still the owner of the final public-proxy reference throughout pre-retire?
7. Can the REF keepalive make the "final" Opti release non-final and change callback order in a crash-capable way?
8. Can a late callback arrive after REF returns but before Opti destroys XeFG context/proxy?
9. Can a late callback arrive after Opti actually destroys/releases the object? If yes, does PR55 have a valid object on which to read the restored vtable?
10. On `Blocked`, does Opti preserve all objects required for a safe retry, or can it leave a half-retired object graph?
11. On `SafeNotTracked`, prove that REF truly retains no callback/hook relationship capable of using the object.
12. On `SafeDetached`, prove that REF no longer owns an instance hook that assumes the borrowed swapchain is alive.

This handshake is a primary audit boundary. Do not stop after seeing that both sides have locks or keepalives; prove the complete lifetime through return and final release.

---

## 9. Final proxy / wrapper release audit

Trace separately:

```text
normal explicit XeFG ReleaseSwapchain
final public-proxy release
Opti WrappedIDXGISwapChain4 final Release
process shutdown / DLL detach
failed initialization cleanup
failed XeFG Destroy
```

For the final-release path construct a sequence diagram showing:

```text
Game/owner
  -> Opti wrapper Release
     -> terminal/final-release decision
     -> Opti state snapshot
     -> REF pre-retire? (when allowed)
        -> REF binding detach/reset/vtable restore
        -> late callbacks if any
     -> XeFG context release/destroy
     -> retained interface refs release
     -> underlying real/proxy Release
     -> wrapper delete
```

The diagram must reflect **actual current code order**, including PR34/35/38.

Crash questions:

- Can REF retain `m_swapchain` after the owning Opti final release begins?
- Can REF's VtableHook destructor/restore access the object after Opti or Intel has freed it?
- Can Opti final release re-enter REF while REF is already inside Present/Resize for the same proxy?
- Can that reentry wait on the same hook-monitor mutex from the same thread or another thread while REF waits on Opti's mutex?
- Can `releaseFinalProxy` execute while REF still has an in-flight callback?
- Can an Opti callback run after `delete this` because REF saved an entry pointing into the wrapper?
- Can the underlying `_real*` interface release order invalidate a method target still needed by REF late forwarding?

Only report a defect when the exact callback/refcount/lock sequence reaches an invalid operation.

---

## 10. CreateSwapChain / factory transition audit

Current REF PR56 intentionally releases the hook-monitor mutex around downstream `CreateSwapChainForHwnd` for active/in-flight XeFG transitions and serializes final rehook using `g_xefg_factory_transition`.

Trace the actual combined path when the downstream call enters Opti / XeFG / DXGI and can re-enter REF runtime hooks.

Required questions:

1. While REF's monitor lock is released, which REF global/session fields are temporarily detached or inconsistent?
2. Which of those fields can another thread read or mutate?
3. Does the do-not-hook guard fully suppress background D3D12 hook installation during the split window?
4. Are nested/concurrent factory calls correctly counted by `in_flight`?
5. Can an exception/early return leave `in_flight` or `rehook_required` inconsistent?
6. Can Opti cause a nested factory transition that exits in a different order from REF's assumptions?
7. Can Opti publish a new XeFG proxy/binding candidate while REF still considers the old generation active/detached?
8. Can the final rehook attach to a swapchain that Opti has already retired or replaced?
9. Can REF's old saved original/trampoline point into an Opti hook that was replaced during the split window?
10. Can Opti's saved original point into the REF hook that REF just removed before the downstream call?

If PR56 proves the relevant cycle impossible, classify it `SAFE BY PROVEN INVARIANT` and state the invariant.

---

## 11. Present / Present1 coexistence audit

Trace both ordinary and interpolated-frame presentation through both modules.

At minimum distinguish:

- application-visible Present/Present1;
- Opti wrapper `LocalPresent`;
- Opti `FGHooks::FGPresent`;
- Intel XeFG proxy/internal Present behavior;
- REF phase-1 pointer hook;
- REF XeFG instance `VtableHook`;
- REF `present_common`;
- REF PR55 late forwarding.

Required crash checks:

- same-thread reentrancy from Opti -> XeFG -> DXGI -> REF -> Opti;
- different-thread callback while the other side is retiring the binding;
- REF instance replacement between Present entry and original-call lookup;
- Opti wrapper deletion while REF is inside `present_common`;
- REF `on_reset()` while an Opti Present transaction still references renderer/swapchain state;
- restored vtable forwarding back into the same REF callback, producing recursion;
- restored vtable forwarding to an Opti function whose owner object/module is no longer valid;
- calling an old Present target after hook uninstall/reinstall.

Produce at least one normal Present sequence and one worst-case retire-vs-Present sequence.

---

## 12. ResizeBuffers / ResizeBuffers1 / ResizeTarget coexistence audit

Audit resize only for crash-capable cross-component lifetime/hook effects.

Trace:

```text
application resize
 -> Opti wrapper / FGHooks
 -> Intel proxy/internal resize behavior
 -> REF instance resize hook
 -> REF renderer reset / resize hold
 -> original/restored target
 -> return / re-enable / present
```

Required questions:

- Can Opti's owner-6678 resize lock overlap REF hook-monitor lock in the opposite order elsewhere?
- Can REF reset/remove its instance hook while Opti is inside the corresponding proxy method?
- Can REF late forwarding execute on an object already released by Opti resize/recreation?
- Can resize trigger a new proxy/generation on one side while the other side still calls methods on the old one?
- Can `ppPresentQueue` or device updates indirectly cause use of a released queue/device in the other module? Ignore mere queue-identity differences unless a stale dereference is proven.
- Does REF's resize hold suppress render callbacks through every path that can expose partially recreated Opti resources?
- Does a failed ResizeBuffers/ResizeBuffers1 leave either module believing the old proxy is alive while the other has already retired it?

Do not promote the existing wrapper `WaitForGPUIdle` no-op defect here unless REFramework coexistence creates the crash path being reported; that defect belongs to a separate generic Opti hardening track.

---

## 13. XeFG runtime hook coexistence audit

REF's `XeFGRuntimeRegistry` hooks Intel exports with `FunctionHook` and stores original targets per runtime slot:

- `xefgSwapChainD3D12InitFromSwapChainDesc`
- `xefgSwapChainD3D12GetSwapChainPtr`
- `xefgSwapChainDestroy`

Opti calls those XeFG APIs through its proxy loader.

Audit:

1. module-load ordering between Opti's XeFG proxy resolution and REF's runtime hook installation;
2. whether Opti can cache a pre-hook function pointer and bypass REF for part of a lifecycle while later calls go through REF;
3. whether REF unhook/reinstall can invalidate a function pointer cached by Opti;
4. whether runtime slot replacement/unload can leave REF's `FunctionHook::get_original()` or Opti's proxy table pointing into an unloaded module;
5. whether `dispatch_destroy` or pre-init transition can detach an active REF binding while Opti still expects it to exist;
6. whether Intel `Destroy` failure/warning paths leave Opti and REF with contradictory context-liveness beliefs;
7. whether multiple XeFG runtime modules/slots can bind the wrong Opti context/proxy.

Again, only promote to a crash finding when a concrete invalid call/deref is reachable.

---

## 14. Cross-component generation audit

Create a generation/state matrix covering, at minimum:

```text
Opti wrapper instance identity
Opti State::currentFGSwapchain
Opti XeFG _swapChainContext
Opti XeFG _fgContext
Opti owned initialization/game queue
REF runtime slot
REF runtime context
REF binding generation
REF borrowed swapchain
REF binding queue/device
REF instance VtableHook target
REF detached-uncertain state
REF resize-hold state
REF factory-transition in_flight/rehook_required
```

For each lifecycle event show **before / mutation / after**:

1. first XeFG context creation;
2. GetSwapChainPtr/public proxy acquisition;
3. REF external/candidate binding;
4. normal Present;
5. resize without recreation;
6. XeFG deactivate/reactivate;
7. swapchain/proxy recreation;
8. failed initialization;
9. failed/warning Destroy;
10. explicit release;
11. final proxy release;
12. process shutdown.

Findings about stale generations must name the exact old object and the later operation that dereferences it.

---

## 15. Combined lock and callback graph — mandatory

Produce one combined lock graph containing every lock that can participate in a cross-module call.

At minimum include:

### OptiScaler

- `IFGFeature_Dx12::Mutex` / owner IDs relevant to Present, resize, release;
- XeFG `_swapchainLifecycleMutex`;
- wrapped swapchain `_localMutex` if enabled/relevant;
- any release guard or lifecycle atomic used by final proxy release.

### REFramework

- `g_framework->get_hook_monitor_mutex()`;
- `XeFGCompatibility` state mutex where applicable;
- `XeFGRuntimeRegistry::m_mutex`;
- `XeFGCandidateHandoff` pending-candidate mutex;
- Streamline hook mutex if it can appear in the same target path;
- factory-transition state and do-not-hook guard interactions.

For every cross-module edge record:

```text
held lock(s)
  -> cross-module call
     -> lock(s) acquired by callee
        -> callback / DXGI / XeFG call back into first module
```

A deadlock finding must show an actual cycle such as:

```text
Thread A: Opti lock X -> REF lock Y
Thread B: REF lock Y -> Opti lock X
```

or a same-thread nonrecursive reentry that demonstrably reacquires a held lock.

Do not label "possible lock inversion" without identifying both edges in current source.

---

## 16. Hook-monitor / rehook / quarantine interaction

The existing E_ABORT evidence shows REF HookMonitor timeout/quarantine **after** Present activity stopped; do not assume the monitor caused that historical failure.

For current source, independently audit whether hook-monitor actions themselves can create a new crash-capable coexistence path.

Answer:

- Can background recovery unhook or rehook D3D12 while Opti is inside Present, Resize, CreateSwapChain, or Release?
- Which runtime-transition/factory-transition flags suppress recovery?
- Can quarantine clear an instance relationship while an Opti callback is in flight?
- Does the monitor take the same mutex as pre-retire and factory transition?
- Can PR56's do-not-hook guard be released too early relative to Opti's downstream factory work?
- Can an older late callback execute after a rehook and accidentally forward into the new generation?
- Can a monitor timeout operate on a binding generation whose proxy has already been finally released?

If current session/generation checks prevent this, document the invariant and classify safe.

---

## 17. Shutdown audit

Treat normal process shutdown separately from normal runtime retirement.

Current Opti PR35 intentionally skips REF pre-retire when `State::isShuttingDown` is true.

Trace:

```text
DLL/process detach begins
 -> Opti isShuttingDown publication
 -> remaining Present/Resize/Release callbacks
 -> XeFG swapchain/context teardown
 -> REF hook-monitor / D3D12 hook destruction
 -> runtime hook destruction/module unload
 -> wrapper/COM final releases
```

Required questions:

- Can REF call into Opti or Intel after Opti teardown has begun?
- Can Opti call the REF export after REF module detach has begun despite PR35?
- Can `FindREFXeFGPreRetire` race module unload and call a function pointer from an unloaded module?
- Is module enumeration + `GetProcAddress` followed immediately by call protected against concurrent unload by any process invariant?
- Can REF `FunctionHook` destructors restore Intel export bytes after the Intel XeFG DLL has unloaded?
- Can late Present/Resize callback forwarding inspect a swapchain object after its COM destruction during shutdown?
- Can final-release callbacks race D3D12Hook destruction / `g_d3d12_hook` clearing?

Do not call a normal process-exit leak a crash defect. Only report reachable invalid execution during shutdown.

---

## 18. Required lifecycle timing diagrams

The report must include **all** of the following diagrams. Do not stop after the first suspicious path.

1. **Startup / initial hook path**  
   D3D12 discovery -> Opti swapchain wrapper -> XeFG runtime load -> REF runtime hooks -> XeFG InitFromSwapChainDesc -> GetSwapChainPtr -> REF candidate/external binding -> first Present.

2. **Normal Present / Present1**  
   Include both Opti and REF hook layers and the actual original/restored target.

3. **ResizeBuffers / ResizeBuffers1**  
   Show Opti resize ownership, Intel proxy behavior, REF resize hook/hold, renderer reset, return.

4. **FG deactivate/reactivate without process shutdown**  
   Show context/proxy/binding state before and after.

5. **Swapchain/proxy recreation and binding rollover**  
   Include REF binding generation and Opti public proxy/context identity.

6. **Final public-proxy release**  
   Include Opti pre-retire cross-call, REF keepalive, detach/vtable restore, XeFG context/proxy release, wrapper final release.

7. **Normal process shutdown**  
   Explicitly show why REF pre-retire is or is not called.

8. **Late callback after REF detach**  
   Present or Resize callback that entered before/during detach and resumes afterward; show PR55 forwarding.

9. **Concurrent/nested CreateSwapChainForHwnd**  
   Show PR56 monitor unlock, in-flight count, downstream Opti/XeFG work, and final one-shot rehook.

10. **Hook-monitor timeout/recovery during active XeFG**  
    Show generation/runtime-transition guards and whether mutation can overlap a live Opti call.

Each diagram must identify threads where concurrency matters.

---

## 19. Crash-mode proof templates

Use these proof standards.

### 19.1 Use-after-free

Required evidence:

```text
object O has final owning reference released
 -> no valid keepalive remains
 -> pointer P remains reachable in other module
 -> later path dereferences/calls P
```

State the exact COM object/interface and ref owner.

### 19.2 Double release / refcount corruption

Required evidence:

```text
owned reference count source
 -> first Release consumes ownership
 -> same logical ownership is consumed again
 -> object may already be destroyed
```

Do not infer double release merely because both modules call `Release` on different owned references.

### 19.3 Stale hook/trampoline

Required evidence:

```text
module A saves target T
 -> module B later detaches/restores/replaces T
 -> T is no longer valid for the object/module lifetime
 -> module A later calls saved T
```

Account for REF PR55 late-vtable forwarding before declaring this defect.

### 19.4 Deadlock

Required evidence:

Show exact locks, exact two paths, and exact wait cycle. Include PR56's split monitor scope.

### 19.5 Fatal D3D / invalid lifecycle

Required evidence:

Show an actual API call on a destroyed/retired context, swapchain, queue, or device, or a contractually illegal lifecycle order. Do not equate an error return with a crash unless the subsequent path makes it fatal.

---

## 20. Existing runtime evidence — use carefully

Historical MHW evidence may be used as supporting context, but **must not be presented as validation of the current Opti/REF source tips**.

Relevant historical observations include:

- runtime REFramework build `c6704372...`;
- successful repeated ResizeBuffers1 / Present sessions;
- distinct same-device XeFG initialization and presentation queues;
- an E_ABORT 4004 crash capture with no paired OptiScaler.log;
- a separate shutdown/proxy-retire crash capture in which REF lifecycle detach / pre-retire and Opti final release occurred close together.

Rules:

- historical stacks can prioritize source paths but do not prove the current code still contains the defect;
- HookMonitor timeout after Present cessation is a downstream observation unless current source proves otherwise;
- do not merge separate sessions into one timeline;
- do not claim PR34-PR38 or REF PR55/56 runtime success from older binaries.

If no current runtime pair exercises a candidate, classify it source-only/conditional and state the exact missing evidence.

---

## 21. Mandatory per-finding PR/fix history assessment

For every `CONFIRMED` or `CONDITIONAL CRASH-CAPABLE DEFECT`, include:

| Existing fix | Relevance to finding | Fully blocks it? | Residual reachable path |
|---|---|---|---|
| Opti PR34 | final-release reentrancy | yes/no | exact path |
| Opti PR35 | shutdown pre-retire | yes/no | exact path |
| Opti PR36 | Streamline/Present transaction | yes/no | exact path |
| Opti PR37 | EvaluateState owner-2 lifetime | yes/no | exact path |
| Opti PR38 | D3D12 COM ownership | yes/no | exact path |
| REF PR55 | late callback forwarding | yes/no | exact path |
| REF PR56 | factory lock inversion | yes/no | exact path |

A finding that is fully blocked by a current fix is not a current primary finding; move it to **Resolved Historical Risk / Safe by Current Invariant**.

---

## 22. Mandatory completion questions

The audit is incomplete until every question below has a sourced answer or an explicit `INSUFFICIENT EVIDENCE` with the exact missing evidence.

1. Can Opti final wrapper/proxy release call REF pre-retire while REF is inside Present/Resize for the same proxy?
2. If yes, what prevents or permits UAF/deadlock?
3. Can REF pre-retire call DXGI/XeFG/Opti code while Opti `_swapchainLifecycleMutex`, FG mutex, or wrapper local mutex is held?
4. Can either project retain and later dereference the public proxy after the other project causes its final release?
5. Does REF's temporary `ComPtr` keepalive cover every callback interval that matters?
6. Can a callback begin before detach and resume after the Opti object has been destroyed?
7. Does PR55 always forward such a callback to a valid current vtable target, or can the object itself already be dead?
8. Can Opti or REF save an original/trampoline that becomes invalid when the other side unhooks/reinstalls?
9. Can either side restore a vtable slot over the other side's still-active hook?
10. Can CreateSwapChainForHwnd nested/concurrent transitions escape PR56's `in_flight` accounting?
11. Can a failed downstream factory call leave REF unhooked while Opti holds a live proxy that will later call back?
12. Can a new Opti XeFG generation be published before REF fully retires the old generation?
13. Can REF bind a new generation while an old Opti final release later detaches it by hwnd/context ambiguity?
14. Can ordinary ResizeBuffers1 create a stale old-generation proxy/hook relationship capable of a later call?
15. Can hook-monitor rehook/quarantine mutate the hook chain while Opti is in Present/Resize/Release?
16. Do runtime-transition and factory-transition suppression cover every such monitor path?
17. Can shutdown call `REFramework_XeFG_PreRetireSwapchainV1` after REF has begun unload?
18. Can REF runtime hooks call an Intel XeFG original after the vendor module has begun unload?
19. Can Opti's dynamic REF export lookup race REF module unload in a crash-capable way?
20. Are queue/device differences purely observational, or can either project dereference a released queue/device due to the other's lifecycle transition?
21. Are PR34/35 safeguards symmetric across explicit release, final proxy release, failed destroy, resize/recreation, and shutdown?
22. Are REF PR55/56 safeguards symmetric across Present, Present1, ResizeBuffers, ResizeBuffers1, ResizeTarget, factory recreation, runtime destroy, and binding replacement?
23. Is any cross-component callback executed while both sides believe they own final teardown authority for the same proxy/context?
24. Is there any current path where a borrowed REF swapchain outlives the caller-bounded hook lifetime documented in `XeFGBinding`?
25. Can a `SafeNotTracked` or `SafeDetached` result be returned while a REF callback still contains an unprotected raw pointer that will later be used?

---

## 23. Required final report

Create the final report at:

```text
doc/work-order/RELEASE_0_9_OPTISCALER_REFRAMEWORK_XEFG_D3D12_COEXISTENCE_CRASH_AUDIT_REPORT_2026-09-16.md
```

Required structure:

1. **Executive verdict**
   - count of confirmed crash-capable defects;
   - count of conditional crash-capable defects;
   - whether current coexistence can be considered source-safe for tested lifecycle paths;
   - no broad "architecture quality" verdict.

2. **Pinned revision matrix**
   - exact final audited Opti tip;
   - exact final audited REF tip;
   - note any commits that landed during audit and whether they touch scope.

3. **Combined architecture / callback diagram**

4. **Shared hook-chain matrix**

5. **Shared object ownership / lifetime matrix**

6. **Combined lock-order and callback graph**

7. **Generation / lifecycle matrix**

8. **Required timing diagrams** from section 18

9. **Primary Crash Findings**
   - only confirmed/conditional crash-capable issues;
   - exact source and exact interleaving;
   - crash mechanism;
   - confidence and runtime evidence;
   - relationship to PR34-38 and REF PR55/56.

10. **Safe by Current Invariant / Dismissed Candidates**
    - suspicious paths that were proven safe;
    - old issues already fixed;
    - non-crash refactoring concerns excluded from action.

11. **Insufficient Evidence**
    - exact missing source/runtime evidence;
    - minimal diagnostic needed, if any.

12. **Remediation ordering**
    - only for actual crash-capable findings;
    - smallest causal fix first;
    - no implementation in this audit.

13. **Completion gate table** answering every section-22 question.

---

## 24. Stop conditions and anti-shortcut rules

The auditor must **not stop** after finding the first suspicious lock, raw pointer, stale-looking hook, or old crash stack.

The audit is complete only after:

- both repositories have been traced through every required lifecycle;
- every shared hook has an install/uninstall/original-target analysis;
- every relevant swapchain/context/queue/device has an ownership/lifetime row;
- PR34-38 and REF PR55/56 have been applied to every candidate before classification;
- all required timing diagrams are produced;
- every completion question is answered;
- each primary finding has a concrete crash mechanism.

Do not make these shortcuts:

- do not treat "two hooks on one method" as a bug;
- do not treat "borrowed pointer" as UAF without proving lifetime violation;
- do not treat "different queues" as a crash without proving stale/invalid use;
- do not treat a timeout as the initiating fault without chronology;
- do not treat a historical crash stack as proof against current source;
- do not recommend one global lock;
- do not add sleeps/yields/logging delays as fixes;
- do not propose a broad ownership rewrite;
- do not modify either repository during the audit;
- do not open a PR with implementation changes from this instruction.

If a vendor/internal boundary prevents proof, finish all source-side reasoning first, identify the single opaque edge, and specify the **minimum no-behavior-change diagnostic** required to resolve it.

---

## 25. Expected disposition

It is acceptable for the audit to conclude that **no current crash-capable coexistence defect is proven**.

The purpose is not to generate work. The purpose is to distinguish:

```text
real current crash path
vs.
already-fixed historical path
vs.
fragile but safe coexistence
vs.
unproven suspicion
```

A short list of strongly proven findings is preferred over a long list of refactoring concerns.
