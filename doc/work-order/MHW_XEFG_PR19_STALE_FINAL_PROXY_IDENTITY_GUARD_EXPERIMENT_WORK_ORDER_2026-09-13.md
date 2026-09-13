# MHW XeFG PR19 Stale Final-Proxy Identity Guard Experiment Work Order

Date: 2026-09-13

## Purpose

Add one narrowly scoped behavioral experiment to the existing draft PR #19 (`diag/mhw-xefg-auto-ring-trace`) to test whether a **retired XeFG final proxy can tear down a newer swapchain/context lifecycle after waiting for `_swapchainLifecycleMutex`**.

The current PR #19 baseline has already reproduced the target MHW XeFG crash / `E_ABORT`, so a separate baseline-only build is no longer required before this experiment.

This work must add **only the stale final-proxy identity protection** plus the minimum trace support required to prove whether that branch was taken.

Do **not** add the wrapper generation guard in this work order. That is the next independent experiment only if this change does not explain the crash.

Do **not** merge PR #19 as part of this work. Keep it open and draft for runtime testing.

---

## Reviewed baseline

This work order was prepared against the current PR #19 branch:

```text
PR:      #19
branch:  diag/mhw-xefg-auto-ring-trace
head:    8542f4584f13bf65c3fd3c0255eb5c3c44086f4f
base:    fix/fg-present-thread-aware-ownedmutex
```

Relevant files reviewed:

```text
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/diagnostics/XeFGTrace.h
tools/decode_xefg_trace.py
```

Reference implementation reviewed from:

```text
branch: reframework-0.9
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
```

The `reframework-0.9` reference must be used only for the stale-final-proxy invariant. Do not copy unrelated 0.9 lifecycle behavior into master/PR19.

---

## Current PR19 behavior and the race window

`FGHooks::hkFGRelease(IUnknown* This)` currently recognizes a likely final public proxy release by temporarily `AddRef()`-ing the proxy, calling the original release, and entering XeFG teardown when the observed original release result is `1`.

The current XeFG call is effectively:

```cpp
releaseSucceeded =
    xefg->ReleaseSwapchainFromFinalProxyRelease(_hwnd, [This]() { o_FGRelease(This); });
```

However, `ReleaseSwapchainFromFinalProxyRelease()` currently does **not** receive the actual proxy identity (`This`). Instead, after taking `_swapchainLifecycleMutex`, it reads:

```cpp
auto* const finalProxy = state.currentFGSwapchain;
```

That loses the identity of the object whose final COM release actually triggered the call.

A valid race is therefore possible:

```text
Thread A
  old proxy A reaches final Release
  hkFGRelease(A)
  enters ReleaseSwapchainFromFinalProxyRelease(...)
  waits for _swapchainLifecycleMutex

Thread B / lifecycle path
  retires A
  creates/publishes proxy B
  state.currentFGSwapchain = B

Thread A
  acquires _swapchainLifecycleMutex
  reads state.currentFGSwapchain
  sees B and treats B as finalProxy
  may run current lifecycle teardown against B / the newer XeFG context
```

The required invariant is:

> A final COM release may retire the exact proxy that triggered it, but an old proxy must never destroy a newer XeFG swapchain/context lifecycle that became current while the old release was waiting for the lifecycle mutex.

---

## Critical secondary issue in the current caller

Fixing only `XeFG_Dx12::ReleaseSwapchainFromFinalProxyRelease()` is **not sufficient**.

After the call returns successfully, current PR19 `hkFGRelease()` unconditionally executes:

```cpp
state.currentFGSwapchain = nullptr;
```

If the final release was stale and a newer proxy B is already current, this line would erase the newer lifecycle alias even if the inner stale-proxy guard correctly avoided teardown.

The caller must therefore clear aliases **only if they still point to `This`**.

This is part of the same stale-final-proxy identity experiment and is required for correctness.

---

# Required implementation

## 1. Pass the actual final proxy identity into XeFG teardown

Update the XeFG declaration in:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.h
```

From:

```cpp
bool ReleaseSwapchainFromFinalProxyRelease(HWND hwnd, std::function<void()> releaseFinalProxy);
```

To:

```cpp
bool ReleaseSwapchainFromFinalProxyRelease(HWND hwnd, IUnknown* finalProxy,
                                           std::function<void()> releaseFinalProxy);
```

Do not change the generic `IFGFeature_Dx12` interface for this experiment. This helper is XeFG-specific.

---

## 2. Pass `This` from `FGHooks::hkFGRelease()`

In:

```text
OptiScaler/hooks/FG_Hooks.cpp
```

Change the XeFG final-release call from the current shape:

```cpp
releaseSucceeded =
    xefg->ReleaseSwapchainFromFinalProxyRelease(_hwnd, [This]() { o_FGRelease(This); });
```

To:

```cpp
releaseSucceeded =
    xefg->ReleaseSwapchainFromFinalProxyRelease(_hwnd, This, [This]() { o_FGRelease(This); });
```

`This` is the authoritative identity for the COM object whose release path triggered this transaction.

Do not substitute `state.currentFGSwapchain` for this argument.

---

## 3. Preserve a safe pre-lock trace entry

In:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Update the function signature accordingly.

The existing `XeFGFinalProxyReleaseEnter` event is intentionally recorded before `_swapchainLifecycleMutex` is acquired. Keep that property.

Pre-lock trace code must read only:

- function arguments (`finalProxy`, `hwnd`)
- `this`
- atomic lifecycle markers already used by PR19

It must **not** read lifecycle-protected raw state such as:

```text
State::Instance().currentFGSwapchain
_swapChainContext
_swapchainRecreationBlocked
_hwnd
```

before acquiring `_swapchainLifecycleMutex`.

Now that `finalProxy` is an explicit stable argument, it is recommended to place it in the existing entry event's `swapchain` field:

```cpp
XeFGTrace::Record(
    XeFGTrace::EventType::XeFGFinalProxyReleaseEnter,
    reinterpret_cast<uint64_t>(finalProxy),
    reinterpret_cast<uint64_t>(this),
    0,
    0,
    _swapchainReleaseInProgress.load(std::memory_order_acquire),
    _swapchainReleaseOwnerThread.load(std::memory_order_acquire));
```

Do not add normal `LOG_*` traffic for this diagnostic change.

---

## 4. Validate the explicit proxy only after taking the lifecycle mutex

Required shape:

```cpp
bool XeFG_Dx12::ReleaseSwapchainFromFinalProxyRelease(
    HWND hwnd,
    IUnknown* finalProxy,
    std::function<void()> releaseFinalProxy)
{
    XeFGTrace::Record(...safe pre-lock fields only...);

    std::unique_lock lifecycleLock(_swapchainLifecycleMutex);

    if (finalProxy == nullptr || !releaseFinalProxy)
    {
        LOG_ERROR("[XeFG][Lifecycle] action = release_swapchain_aborted, "
                  "reason = missing_final_proxy_release");
        return false;
    }

    auto& state = State::Instance();
    bool finalProxyReleased = false;

    ...
}
```

The important point is that `finalProxy` was captured by the caller before the mutex wait and remains the identity being retired.

---

## 5. Keep `releaseFinalProxyOnce()` identity-specific

The callback wrapper must clear public aliases only if they still refer to the exact stale/current proxy being released.

Required shape:

```cpp
auto releaseFinalProxyOnce = [&]()
{
    if (finalProxyReleased)
        return;

    if (state.currentSwapchain == finalProxy)
        state.currentSwapchain = nullptr;

    if (state.currentFGSwapchain == finalProxy)
        state.currentFGSwapchain = nullptr;

    XeFGTrace::Record(
        XeFGTrace::EventType::XeFGFinalProxyReleaseBefore,
        reinterpret_cast<uint64_t>(finalProxy),
        reinterpret_cast<uint64_t>(this),
        reinterpret_cast<uint64_t>(_swapChainContext));

    releaseFinalProxy();

    XeFGTrace::Record(
        XeFGTrace::EventType::XeFGFinalProxyReleaseAfter,
        reinterpret_cast<uint64_t>(finalProxy),
        reinterpret_cast<uint64_t>(this),
        reinterpret_cast<uint64_t>(_swapChainContext));

    finalProxyReleased = true;
};
```

`_swapChainContext` is safe to observe here because `_swapchainLifecycleMutex` is already held.

The callback must still execute at most once.

---

## 6. Add the stale final-proxy identity guard before lifecycle teardown

After `_swapchainLifecycleMutex` is held and after `releaseFinalProxyOnce` is defined, compare the authoritative `finalProxy` argument with the current published FG proxy.

Required logic:

```cpp
// The lifecycle associated with this proxy may already have been retired
// while this final COM release was waiting for _swapchainLifecycleMutex.
// Never let an old proxy tear down a newer XeFG lifecycle.
if (state.currentFGSwapchain != finalProxy)
{
    XeFGTrace::Record(
        XeFGTrace::EventType::XeFGStaleFinalProxyReleaseOnly,
        reinterpret_cast<uint64_t>(finalProxy),
        reinterpret_cast<uint64_t>(state.currentFGSwapchain),
        reinterpret_cast<uint64_t>(_swapChainContext));

    releaseFinalProxyOnce();
    return true;
}
```

### Required semantics of the stale branch

When this branch is taken, it must:

1. consume the final release callback exactly once;
2. clear only aliases still equal to `finalProxy`;
3. return success to the caller;
4. **not** call `ReleaseSwapchainLocked()`;
5. **not** call `DestroyFGContext()`;
6. **not** call `DestroySwapchainContext()`;
7. **not** call `XeFGProxy::Destroy()`;
8. **not** set `_swapchainRecreationBlocked` merely because the proxy is stale;
9. leave the current/newer `state.currentFGSwapchain` intact.

Do not add a new `LOG_DEBUG`/`LOG_INFO` on this branch. The ring-buffer event is sufficient and avoids perturbing the timing-sensitive reproduction.

---

## 7. Fix the caller's unconditional alias clear

In current PR19 `FGHooks::hkFGRelease()`, after `ReleaseSwapchainFromFinalProxyRelease()` succeeds, this code currently clears the current FG proxy unconditionally.

Replace the unconditional clear with identity-conditional alias cleanup.

Preferred shape:

```cpp
LOG_DEBUG("FG Swapchain released, clearing public proxy aliases");

if (state.currentSwapchain == This)
    state.currentSwapchain = nullptr;

if (state.currentFGSwapchain == This)
    state.currentFGSwapchain = nullptr;
```

Do **not** clear a newer pointer merely because teardown for `This` returned success.

This caller-side change is mandatory for the stale-proxy experiment to be valid.

Do not add generation state in this work order.

---

# Trace changes

## 8. Append exactly one new event for stale final-proxy detection

Current PR19 `XeFGTrace::EventType` ends with:

```cpp
XeFGDestroySwapchainContextExit,
```

Append one event after the existing values:

```cpp
XeFGStaleFinalProxyReleaseOnly,
```

Do not insert it in the middle of the enum and do not renumber any existing event IDs.

At the reviewed PR19 head, the existing decoder maps events through ID `108`, so this event should become `109` if no newer events were appended before implementation.

If PR19 has moved, inspect the enum and append at the next available ID rather than assuming `109` blindly.

### Suggested payload

For `XeFGStaleFinalProxyReleaseOnly`:

```text
swapchain          = finalProxy that triggered final Release
object_or_context  = state.currentFGSwapchain after lifecycle mutex acquisition
aux_pointer        = current _swapChainContext
thread             = automatic trace thread ID
```

No record-layout change is required.

Do not bump the trace file version solely for this event append.

---

## 9. Update the decoder

In:

```text
tools/decode_xefg_trace.py
```

Append the exact new event ID/name to `EVENT_NAMES`.

Example for the reviewed head:

```python
109: "XeFGStaleFinalProxyReleaseOnly",
```

Extend `--self-test` so the new event is decoded by name at least once.

Do not change the binary record layout, CSV/TSV fields, or primary-failure semantics.

---

# Explicit non-goals

This experiment must **not** include any of the following:

- wrapper generation / swapchain generation tracking;
- `nextFGSwapchainGeneration` or `currentFGSwapchainGeneration`;
- changes to `WrappedIDXGISwapChain4::Release()`;
- changes to `FGPreserveSwapChain` behavior;
- resize lifecycle redesign;
- `_skipResize`, `_skipResize1`, `_skipPresent`, or `_skipPresent1` changes;
- additional thread-local guard experiments;
- `OwnedMutex` redesign;
- XeLL lifetime changes;
- command queue ownership changes;
- Streamline redesign;
- REF changes;
- timeout/retry/recovery behavior;
- extra synchronous file I/O;
- new per-frame `LOG_*` diagnostics;
- unrelated cleanup/refactoring.

The point of this build is to isolate **one hypothesis**.

---

# Why wrapper generation is intentionally excluded

`reframework-0.9` also contains a stronger generation-based wrapper guard. That may still be valuable for master later, but adding it now would combine two lifecycle protections and make the MHW result ambiguous.

The test sequence must remain:

```text
A. existing PR19 baseline
   -> crash / E_ABORT already reproduced

B. PR19 + stale final-proxy identity guard only
   -> test

C. only if B is insufficient:
   PR19 + stale final-proxy guard + wrapper generation guard
   -> separate follow-up work order / commit
```

---

# Expected code touch set

Keep the behavioral commit limited to approximately:

```text
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/diagnostics/XeFGTrace.h
tools/decode_xefg_trace.py
```

Do not modify project files unless the implementation unexpectedly introduces a new source file. It should not need one.

---

# Runtime interpretation matrix

## Result 1: stale event occurs and crash disappears / drops sharply

Example:

```text
XeFGFinalProxyReleaseEnter
XeFGStaleFinalProxyReleaseOnly
XeFGFinalProxyReleaseBefore
XeFGFinalProxyReleaseAfter
... newer lifecycle continues ...
```

Interpretation:

- strong evidence that an old final proxy was reaching teardown after a newer proxy had become current;
- stale final-proxy teardown is likely a real contributor to the crash;
- promote this guard as a candidate permanent master fix;
- repeat the same test enough times to rule out a timing fluke before merging.

## Result 2: stale event occurs but crash still occurs

Interpretation:

- the race exists in practice;
- this guard is useful but is not sufficient to explain the E_ABORT;
- preserve the trace evidence;
- next candidate can be the wrapper generation guard as a separate experiment.

## Result 3: stale event never occurs and crash still occurs

Interpretation:

- this particular stale-final-proxy path is unlikely to be the current E_ABORT trigger;
- do not infer that generation protection is unnecessary;
- continue with existing PR19 failure trace analysis or the next isolated guard experiment.

## Result 4: stale event occurs, newer proxy is preserved, but teardown later fails elsewhere

Interpretation:

- verify that no subsequent caller path cleared `state.currentFGSwapchain` unconditionally;
- verify `ReleaseSwapchainLocked()` was not entered for the stale event;
- inspect the next `PrimaryFailureTrigger` and surrounding lifecycle events.

---

# Validation requirements

## Static / build validation

Required before runtime testing:

```text
1. Release x64 OptiScaler.vcxproj build passes.
2. python tools/decode_xefg_trace.py --self-test passes.
3. python -m py_compile tools/decode_xefg_trace.py passes.
4. git diff --check passes.
5. No existing EventType values were renumbered.
6. No trace record/header layout changed.
```

## Code-review checks

Verify all of the following manually:

```text
[ ] hkFGRelease passes `This` as explicit finalProxy.
[ ] ReleaseSwapchainFromFinalProxyRelease no longer derives finalProxy from currentFGSwapchain.
[ ] currentFGSwapchain is compared with finalProxy only after _swapchainLifecycleMutex is held.
[ ] stale path releases only finalProxy and returns before ReleaseSwapchainLocked.
[ ] stale path cannot call XeFG Destroy.
[ ] release callback cannot run more than once.
[ ] caller no longer unconditionally clears currentFGSwapchain after a successful stale release.
[ ] newer currentSwapchain/currentFGSwapchain aliases survive an old proxy release.
[ ] no generation guard was added.
[ ] no additional normal logging was introduced for the new stale branch.
```

---

# Runtime test target

Primary test:

```text
Game: Monster Hunter Wilds
Path: same Intel + XeFG + REFramework scenario already used to reproduce PR19 E_ABORT/crash
Build: PR19 with this single behavioral experiment commit
```

Use the same game settings, OptiScaler configuration, REFramework build, XeFG DLL set, and reproduction sequence as the already-crashing PR19 baseline whenever possible.

Capture after every crash or meaningful non-crash run:

```text
OptiScaler_XeFGTrace.bin
OptiScaler_XeFGTrace.prev.bin if relevant
OptiScaler.log
REFramework log / crash evidence if generated
```

Decode at minimum:

```bash
python tools/decode_xefg_trace.py OptiScaler_XeFGTrace.bin --format text
```

Search for:

```text
XeFGFinalProxyReleaseEnter
XeFGStaleFinalProxyReleaseOnly
XeFGFinalProxyReleaseBefore
XeFGFinalProxyReleaseAfter
XeFGReleaseLockedEnter
XeFGDestroyFGContextEnter
XeFGDestroySwapchainContextEnter
XeFGDestroyBegin
XeFGDestroyResult
PrimaryFailureTrigger
```

For any `XeFGStaleFinalProxyReleaseOnly` event, verify that the recorded `swapchain` and `object_or_context` pointers differ.

Also verify that the stale event is **not** followed as part of that stale transaction by `XeFGReleaseLockedEnter` / destroy events that target the newer lifecycle.

---

# Commit / PR instructions

Implement this as **one additional commit on PR #19**.

Suggested commit title:

```text
experiment: guard stale XeFG final proxy release
```

Do not create a replacement PR for PR #19.
Do not merge PR #19 after implementation.
Do not modify PR #18.

Update the PR #19 description so its former diagnostic-only non-goal is qualified. Add a short note similar to:

```text
Temporary behavioral experiment: PR19 now includes a stale final-proxy identity guard to test whether a retired XeFG proxy can tear down a newer swapchain lifecycle while waiting for the lifecycle mutex. Wrapper generation protection remains intentionally excluded.
```

Keep the rest of the existing PR19 diagnostic description intact.

---

# Stop conditions

Stop and report instead of broadening the patch if implementation appears to require any of the following:

- adding generation tracking;
- rewriting the wrapper COM lifetime model;
- changing preserve-swapchain semantics;
- changing resize synchronization;
- changing current XeFG SDK Destroy handling;
- changing REFramework;
- restructuring FGHooks beyond the identity-safe caller cleanup described above.

Those are separate experiments and must not be folded into this commit.

---

## Final objective

This is not a general XeFG lifecycle refactor.

It is a controlled A/B experiment answering one question:

> **Can a final Release from an old XeFG proxy wait behind the lifecycle mutex, observe a newly published proxy/context, and then incorrectly tear down or clear that newer lifecycle?**

The implementation is successful if it makes that scenario impossible while preserving all existing PR19 tracing, and the resulting runtime trace makes it unambiguous whether the stale-proxy guard actually fired during the MHW crash reproduction.