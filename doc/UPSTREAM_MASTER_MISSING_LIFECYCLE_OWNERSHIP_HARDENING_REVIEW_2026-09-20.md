# Upstream Review Candidates: Lifecycle, COM Ownership, and XeFG Hardening

Date: 2026-09-20

## Purpose

This document summarizes fork-only hardening that is not present in the current upstream master and appears relevant to upstream master itself.

The goal is not to ask upstream to cherry-pick the fork blindly. Some of the work originated on the fork master branch and some was later adapted to the fork release/reframework-0.9 branch, whose architecture differs substantially from current master.

For each item below, the important part is the invariant or failure mode. Upstream should implement the idea in a way that fits the current master architecture.

The Intel Reflex selective DXGI identity quirk is intentionally excluded from this document.

## Reviewed snapshots

- Upstream: **optiscaler/OptiScaler master @ 93fbf1b2696112945f87d8a2cea9bdada5720d25**
- Fork master reviewed before this document commit: **onehoon/OptiScaler master @ 64874932da7ceb3475ed1e8fd8aaf837249f022a**
- Fork release line: **onehoon/OptiScaler reframework-0.9**, including merged PRs through PR38

The fork master and upstream master have diverged significantly. The release/reframework-0.9 branch has also diverged heavily from master. For that reason, commit-level cherry-picking is not recommended unless the surrounding assumptions are first verified.

## Executive summary

| Priority | Candidate | Fork source | Upstream status | Recommendation |
| --- | --- | --- | --- | --- |
| P0 | XeFG teardown must fail closed when Destroy is not confirmed successful | master PR #1, #14 | Missing | Strongly recommend |
| P0 | Remove remaining Release-until-zero swapchain ownership drains | master PR #6, #13; reframework-0.9 PR #28 | Still present in multiple paths | Strongly recommend |
| P1 | Harden DX12 wrapper and QueryInterface COM lifetimes | reframework-0.9 PR #38 | Missing | Strongly recommend |
| P1 | Own the D3D12 command queue for the XeFG lifecycle | reframework-0.9 PR #32 | Missing | Strongly recommend |
| P1 | Make OwnedMutex ownership thread-aware and hook reentrancy flags thread-local | reframework-0.9 PR #33 | Missing | Recommend |
| P2 | Add lifecycle generation / per-create context identity to reject stale destroys | reframework-0.9 PR #28 | Missing | Recommend |
| P2 | Serialize Streamline-to-XeFG frame-state mutations against Present | reframework-0.9 PR #36, #37 | Missing; master needs adapted implementation | Recommend |
| Optional integration | REFramework pre-retire XeFG lifecycle handshake | reframework-0.9 PR #29, #31, #34, #35 | Not present upstream; requires matching REF export | Recommend for supported custom REF integration |

The first four candidates are primarily ownership/lifetime correctness issues. They do not depend on custom REFramework behavior.

The last three are synchronization and stale-lifecycle hardening. They are still generally applicable, but upstream should review them against current master callback/threading behavior before implementing them.

---

# 1. XeFG teardown should fail closed when Destroy is not confirmed successful

## Fork provenance

Primary fork work:

- master PR #1: https://github.com/onehoon/OptiScaler/pull/1
- master PR #14: https://github.com/onehoon/OptiScaler/pull/14

Relevant fork files:

- OptiScaler/framegen/xefg/XeFG_Dx12.cpp
- OptiScaler/framegen/xefg/XeFG_Dx12.h
- OptiScaler/hooks/FG_Hooks.cpp
- selected release callers that must propagate failed teardown

## Current upstream behavior

Current upstream master still has a lifecycle problem in **XeFG_Dx12::DestroySwapchainContext()**.

The function temporarily clears the context before calling Intel XeFG Destroy. If Destroy does not return exact success, it restores the context pointer.

Conceptually:

~~~cpp
auto context = _swapChainContext;
_swapChainContext = nullptr;

auto result = XeFGProxy::Destroy()(context);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    _swapChainContext = context;

return true;
~~~

The critical issue is the unconditional successful return.

The surrounding **ReleaseSwapchain()** path does not receive a meaningful failure result and can continue with state cleanup:

~~~text
Destroy requested
    -> Destroy reports non-success
    -> context pointer is restored locally
    -> DestroySwapchainContext still reports success
    -> ReleaseSwapchain continues
    -> global/context state can be cleared as if teardown completed
    -> a replacement lifecycle can later be created
~~~

This loses the distinction between:

- context definitely destroyed;
- context definitely still alive;
- context state uncertain because the vendor returned a non-success status.

## Why this matters

A failed XeFG Destroy is a lifecycle boundary, not merely a diagnostic result.

If the old XeFG swapchain context was not destroyed, creating a replacement for the same window can result in overlapping lifecycle state. Even if the original trigger is an external outstanding reference, OptiScaler should not convert that vendor-reported failure into a locally successful teardown.

The invariant should be:

> A replacement XeFG lifecycle must not be published until retirement of the previous lifecycle is confirmed.

This is independent of REFramework. REF can make outstanding-reference problems easier to encounter, but the error propagation rule is general.

## Fork behavior

The fork master changed the path so that teardown completion is explicit.

The important behavior is:

~~~text
Destroy exact SUCCESS
    -> retirement is complete
    -> old context/global state may be cleared
    -> replacement is allowed

Destroy error / unconfirmed retirement
    -> release returns failure
    -> relevant old state is retained or quarantined
    -> replacement creation aborts
~~~

PR #14 refined XeFG status semantics further. Positive SDK statuses are logged as warnings, but a warning is not automatically treated as proof that a lifecycle transition or output mutation completed successfully.

For lifecycle-changing APIs, exact success is required before committing state.

This matters especially for:

- context creation;
- latency-reduction binding;
- swapchain initialization;
- GetSwapChainPtr;
- Activate / Deactivate state commits;
- Destroy.

The exact warning policy does not have to be copied literally, but upstream should avoid using a generic non-negative-success interpretation for state transitions where completion must be proven.

## Upstream implementation recommendation

Current upstream architecture should keep its own object model but enforce these rules:

1. Make **DestroySwapchainContext()** return meaningful success/failure.
2. Do not clear the authoritative context identity on unconfirmed Destroy.
3. Propagate failure through **ReleaseSwapchain()**.
4. Abort same-window replacement creation after a failed/uncertain retirement.
5. Ensure final-proxy release paths cannot silently convert a failed retirement into a clean state.
6. Log the context identity, vendor result, and whether recreation was quarantined.
7. Require exact success before committing lifecycle state where the SDK result does not guarantee completion.

## Risk if unchanged

Potential consequences include:

- creating a second XeFG lifecycle while the first one remains alive;
- stale proxy/context state;
- later device removed / E_ABORT style failures far from the original failed teardown;
- shutdown or resize failures that are difficult to attribute because the first invalid transition was treated as successful.

## Suggested validation

- repeated same-HWND swapchain recreation;
- resize/fullscreen transitions;
- XeFG enable/disable cycles;
- injected or naturally occurring Destroy failure;
- clean process shutdown;
- Intel and non-Intel GPUs when XeFG is used as the output replacement;
- REF-free testing in addition to any REFramework testing.

---

# 2. Remove all remaining Release-until-zero swapchain ownership drains

## Fork provenance

Relevant fork work:

- master PR #6: https://github.com/onehoon/OptiScaler/pull/6
- master PR #13: https://github.com/onehoon/OptiScaler/pull/13
- reframework-0.9 PR #28: https://github.com/onehoon/OptiScaler/pull/28

Upstream already removed one related pattern in commit:

- 349176128f5ce87fb81a4c636569c60c44557fec — Disable XeFG backbuffer release hack

That was an important fix, but the same ownership anti-pattern still exists elsewhere.

## Current upstream behavior

### A. XeFG currentRealSwapchain force drain

Current upstream **XeFG_Dx12::CreateSwapchain()** and **CreateSwapchain1()** still contain a path equivalent to:

~~~cpp
if (State::Instance().currentRealSwapchain != nullptr)
{
    UINT release = 0;
    do
    {
        release = State::Instance().currentRealSwapchain->Release();
    } while (release > 0);
}
~~~

The comment indicates this exists because XeFG may not release the swapchain sufficiently for same-HWND recreation.

The problem is that **State::currentRealSwapchain** is tracking state, not proof that OptiScaler owns every outstanding reference represented by the current COM count.

### B. FFX API wrapped-swapchain force drain

Current upstream **inputs/FG/FfxApi_Dx12_FG.cpp** still performs:

~~~cpp
auto refCount = State::Instance().currentWrappedSwapchain->Release();

while (refCount > 0 && refCount < 0xffffff00)
{
    refCount = State::Instance().currentWrappedSwapchain->Release();
}
~~~

This appears in replacement and destroy-related paths.

### C. Legacy FSR3 wrapped-swapchain force drain

Current upstream **inputs/FG/FSR3_Dx12_FG.cpp** contains the same pattern.

## Why this matters

COM reference count is not an ownership map.

A high reference count does not identify which component owns the references. Repeatedly calling Release until the count reaches a threshold can consume references owned by:

- the game;
- Streamline or another interposer;
- a wrapper;
- an overlay;
- XeFG/vendor code;
- another OptiScaler subsystem.

The correct rule is:

> A component releases exactly the references it owns. Tracking aliases do not grant Release authority.

This is the same principle that motivated upstream commit 34917612 for backbuffers. The remaining swapchain force-drain paths should be audited using the same principle.

## Fork behavior

Fork master PR #6/#13 removed XeFG-side swapchain ownership drains and documented the relevant State swapchain fields as non-owning tracking aliases.

The reframework-0.9 PR #28 extended the same ownership principle to the FFX API and legacy FSR3 paths:

~~~text
tracked wrapper needs replacement
    -> detach tracking alias by identity
    -> do not repeatedly Release the object
    -> let the real COM owner perform its own balanced release
~~~

The 0.9 implementation should not be copied directly into master because the surrounding wrapper/generation architecture differs. The invariant, however, is directly applicable.

## Upstream implementation recommendation

Perform a targeted ownership audit of all swapchain state fields and all loops that repeatedly call Release.

Recommended rules:

1. Document whether each State swapchain pointer is owned or borrowed.
2. Remove Release-until-zero loops from borrowed/tracking aliases.
3. Clear aliases by pointer identity, not by draining references.
4. Keep the wrapper's owned underlying-real reference balanced exactly once.
5. Keep final XeFG public-proxy cleanup separate from wrapper-owned real-swapchain cleanup.
6. On vendor Destroy failure, fail closed rather than trying to force external references away.
7. Audit FFX API and legacy FSR3 replacement/destroy callbacks, not only native XeFG paths.

## Risk if unchanged

- over-release of references owned by another component;
- use-after-release behavior that depends on timing;
- reentrant Release callbacks into hook/wrapper code;
- inconsistent behavior with overlays and REFramework;
- teardown that appears to work until another owner touches the prematurely released object.

## Suggested validation

- FFX API input to XeFG;
- legacy FSR3 input to XeFG;
- native XeFG creation/recreation;
- multiple swapchain replacements on the same HWND;
- external overlay/interposer present;
- full shutdown with no lingering process.

---

# 3. Harden DX12 wrapper and QueryInterface COM lifetimes

## Fork provenance

- reframework-0.9 PR #38: https://github.com/onehoon/OptiScaler/pull/38

Relevant files:

- OptiScaler/Util.cpp
- OptiScaler/Util.h
- OptiScaler/wrapped/wrapped_swapchain.cpp
- OptiScaler/wrapped/wrapped_swapchain.h

## Current upstream behavior

Current upstream master still has several patterns where a QueryInterface-acquired reference is immediately released while the returned raw pointer continues to be used.

### A. Command queue in LocalPresent

Conceptually:

~~~cpp
ID3D12CommandQueue* cq = nullptr;

if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
{
    cq->Release();

    ID3D12CommandQueue* realQueue = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cq, (IUnknown**) &realQueue))
        cq = realQueue;

    ...

    cq->GetDevice(...);
}
~~~

The QI reference has already been released before later operations use the pointer.

### B. D3D12 device in LocalPresent

The same function obtains a D3D12 device, releases the QI reference, and then publishes/uses the pointer:

~~~cpp
if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
{
    device12->Release();

    State::Instance().currentD3D12Device = device12;
    D3D12Hooks::HookDevice(device12);
}
~~~

### C. Wrapper interface pointers

The wrapped swapchain constructor queries IDXGISwapChain1/2/3/4 interfaces and then immediately releases each QI reference while retaining the raw interface pointer in the wrapper.

This can work as long as another reference keeps the COM object alive and the implementation's interface pointers remain valid, but the lifetime contract is implicit and fragile. Reentrancy and teardown make that assumption harder to reason about.

## Why this matters

A QueryInterface success gives the caller an owned COM reference. Releasing it before the last local use converts a clear ownership contract into an implicit borrowed-pointer contract.

The issue is not that every such sequence is guaranteed to crash. The issue is that correctness depends on another reference remaining alive across all later uses.

For lifecycle-heavy code such as swapchain replacement, Present, and wrapper final Release, explicit ownership substantially reduces ambiguity.

The invariant should be:

> Every QI-acquired pointer remains owned until its last use, unless it is deliberately transferred to another owner.

## Fork behavior

PR #38 introduced an explicit owned-query helper and WRL RAII for the DX12 LocalPresent path.

Conceptually:

~~~text
QI command queue
    -> ComPtr owns it through all queue uses

unwrap Streamline object
    -> owned ComPtr for returned real object

GetDevice
    -> ComPtr owns device through HookDevice and local operations
~~~

It also retained the wrapper's QI references for **_real1 ... _real4** for the wrapper lifetime and released them in a single idempotent cleanup path before the final underlying real-swapchain release.

The fork intentionally did not redefine every historical borrowed pointer in the project. It fixed the specific DX12 wrapper paths where lifetime was being relied on after an immediate Release.

## Upstream implementation recommendation

1. Keep QI references alive through their final local use with ComPtr or equivalent RAII.
2. If **CheckForRealObject()** intentionally returns a borrowed alias, make that contract explicit.
3. Consider adding a separate helper for callers that require an owned real-object reference.
4. Keep the wrapper's queried IDXGISwapChain interface references alive for the wrapper lifetime, or otherwise prove that the base real-swapchain ownership guarantees all retained interface pointers.
5. Release retained interface references exactly once during wrapper finalization.

This should be implemented against current master rather than copied mechanically from the 0.9 branch.

## Risk if unchanged

Mostly timing-dependent lifetime fragility:

- stale queue/device pointer if another owner drops the final reference;
- use after a reentrant teardown;
- wrapper interface pointer assumptions that are difficult to audit;
- future refactors breaking previously implicit ownership.

## Suggested validation

- DX12 wrapped swapchain Present;
- Streamline-wrapped command queue;
- resize/recreation while wrapper is active;
- final wrapper Release;
- DXVK early-return paths;
- clean shutdown and device-removal handling.

---

# 4. Own the D3D12 command queue for the XeFG lifecycle

## Fork provenance

- reframework-0.9 PR #32: https://github.com/onehoon/OptiScaler/pull/32

Relevant files:

- OptiScaler/framegen/xefg/XeFG_Dx12.cpp
- OptiScaler/framegen/xefg/XeFG_Dx12.h
- OptiScaler/hooks/FG_Hooks.cpp
- release callers that retire queue-generation state

## Current upstream behavior

Current upstream XeFG still stores the game command queue as a raw pointer:

~~~cpp
void XeFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue)
{
    _gameCommandQueue = queue;
}
~~~

The queue is then used beyond the immediate call that supplied it.

Current master also has command-queue state used for:

- swapchain initialization;
- resize synchronization;
- Present-time work;
- GPU-idle waits;
- HUD capture retirement through currentFG->GetCommandQueue() in newer HUDfix code.

The more lifecycle work depends on GetCommandQueue(), the more important it becomes that the returned pointer has an explicit lifetime guarantee.

## Why this matters

A long-lived raw COM pointer is safe only if some other owner is guaranteed to keep the object alive for the entire period in which OptiScaler may use it.

Swapchain recreation makes that assumption particularly risky:

~~~text
lifecycle A uses queue A
new creation arrives with queue B
old lifecycle A still needs draining/retirement
global/raw queue pointer is changed to B
old retirement accidentally signals or waits on B
~~~

Even if queue A and B often happen to be the same object, the lifecycle should not rely on that.

The invariant should be:

> Each active XeFG lifecycle owns the command queue it uses, and retirement of that lifecycle uses the same owned queue.

## Fork behavior

PR #32 introduced an owned queue for XeFG.

The important flow is:

~~~text
incoming queue
    -> resolve real queue if needed
    -> hold as local ComPtr candidate
    -> use candidate for XeFG initialization
    -> only after successful initialization, commit it as the lifecycle-owned queue
~~~

For replacement:

~~~text
old lifecycle A
    -> use queue A to drain/retire A
    -> do not publish queue B early
    -> retire A
    -> create/commit lifecycle B with queue B
~~~

The release branch also tied resize synchronization state to the active FG generation so that retiring an old generation cannot clear a newer generation's queue/fence state.

The generation mechanism is discussed separately below. Upstream does not have to use the exact same implementation to gain the main queue-ownership benefit.

## Upstream implementation recommendation

At minimum:

1. Store an owned ComPtr for the active XeFG game command queue.
2. Keep the raw **_gameCommandQueue** only as an alias if required by existing base-class interfaces.
3. Do not replace the owned queue until the new XeFG lifecycle has successfully initialized.
4. Use the old lifecycle's queue for GPU-idle waits during its retirement.
5. Reset queue ownership only when that lifecycle has actually retired.
6. Audit global/static queue aliases in FGHooks and State for the same lifetime assumptions.

If upstream adopts lifecycle generations, bind queue/fence state to the generation as well.

## Risk if unchanged

- use of a destroyed command queue;
- waiting/signaling the wrong queue during recreation;
- old/new lifecycle cross-contamination;
- hard-to-reproduce resize, Present, or shutdown failures.

## Suggested validation

- repeated swapchain recreation with the same queue;
- recreation where a different queue is supplied;
- failed creation after old lifecycle retirement;
- preserve-swapchain mode;
- resize GPU-idle waits;
- delayed HUD capture removal;
- full shutdown.

---

# 5. Make OwnedMutex ownership thread-aware and reentrancy guards thread-local

## Fork provenance

- reframework-0.9 PR #33: https://github.com/onehoon/OptiScaler/pull/33

This work incorporates the useful part of earlier fork synchronization experiments but was implemented and merged as the release/0.9 P7-C hardening.

Relevant files:

- OptiScaler/OwnedMutex.h
- OptiScaler/hooks/FG_Hooks.h
- OptiScaler/hooks/FG_Hooks.cpp
- OptiScaler/wrapped/wrapped_swapchain.cpp
- FSRFG / XeFG owner-check callers

## Current upstream behavior

Current upstream **OwnedMutex** records only a logical owner tag:

~~~cpp
std::atomic<uint32_t> owner { 0 };

void lock(uint32_t ownerTag)
{
    mtx.lock();
    owner.store(ownerTag);
}
~~~

Callers then use the tag to decide whether the current path is reentrant.

For example, Present contains logic equivalent to:

~~~cpp
if (... && fg->Mutex.getOwner() != 2)
{
    fg->Mutex.lock(2);
    mutexUsed = true;
}
~~~

The problem is that owner tag 2 means an operation class. It does not identify which OS thread owns the mutex.

A second thread observing owner == 2 cannot safely conclude that it is the reentrant owner.

In addition, upstream FGHooks keeps these flags process-global:

- _skipResize
- _skipResize1
- _skipPresent
- _skipPresent1

These flags represent nested hook state. If one thread sets a skip flag and another thread enters the same hook, the second thread can consume state that belongs to the first.

## Why this matters

There are two different concepts:

1. global observation that an operation of a given type is active;
2. same-thread reentrancy where the current thread is already inside that operation.

A single numeric owner tag cannot represent both.

For a non-recursive mutex, false same-thread detection can bypass required serialization. Conversely, releasing a mutex from a thread that did not acquire it is invalid.

The invariant should be:

> Same-thread reentrancy checks require both the logical owner tag and the actual owning thread identity.

For hook skip flags:

> State that means skip the nested call on this call stack belongs to the current thread, not the process.

## Fork behavior

PR #33 added:

~~~text
OwnedMutex:
    owner tag
    + owner OS thread ID
    + isOwnedByCurrentThread(tag)
~~~

Unlock now requires both tag and thread identity to match.

The four Present/Resize hook skip flags became **thread_local**.

The fork did not blindly convert every owner check. Some checks intentionally observe a globally active operation and were left global until their callback/thread contract could be proven.

That distinction is important for upstream too.

## Upstream implementation recommendation

1. Add owning-thread identity to OwnedMutex.
2. Add **isOwnedByCurrentThread(ownerTag)**.
3. Require the owning thread when unlocking.
4. Convert same-call-flow reentrancy tests to the thread-aware helper.
5. Make FGHooks Present/Resize skip flags thread-local.
6. Do not mechanically convert checks that intentionally ask whether any thread currently owns a lifecycle operation.
7. Audit wrapper ResizeBuffers / ResizeBuffers1 and SetFullscreenState for the same distinction.

## Relationship to recent upstream Present/Resize work

This proposal is conceptually separate from recent upstream shared-mutex changes around native/DX11-with-DX12 Present/Resize synchronization.

Thread-aware OwnedMutex fixes the meaning of existing ownership/reentrancy checks. It does not require upstream to adopt the fork's overall synchronization architecture.

## Risk if unchanged

- cross-thread false reentrancy;
- serialization bypass;
- another thread consuming a hook skip flag;
- invalid unlock assumptions;
- logging-sensitive timing bugs where adding diagnostics changes thread scheduling enough to hide the issue.

## Suggested validation

- Present and Resize on different threads;
- nested vendor callbacks on the same thread;
- SetFullscreenState during active FG;
- repeated ResizeBuffers / ResizeBuffers1;
- logging on/off comparison;
- FSRFG and XeFG output paths.

---

# 6. Add lifecycle generation and per-create context identity to reject stale destroys

## Fork provenance

- reframework-0.9 PR #28: https://github.com/onehoon/OptiScaler/pull/28

Relevant files:

- OptiScaler/State.h
- OptiScaler/hooks/FG_Hooks.cpp
- OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
- OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp
- OptiScaler/wrapped/wrapped_swapchain.cpp
- OptiScaler/wrapped/wrapped_swapchain.h

PR #28 also removed additional COM force-drain paths. Those are covered separately in Candidate 2. This section is only about stale lifecycle identity.

## Current upstream behavior

### A. Legacy FSR3 input uses a fixed fake context identity

Current upstream **FSR3_Dx12_FG.cpp** still contains:

~~~cpp
const UINT fgContext = 0x1337;
~~~

Context create returns this fixed marker and destroy checks the same value.

The marker identifies a context type, but it does not identify a particular creation instance.

### B. FFX API input uses fixed fake context identities

Current upstream **FfxApi_Dx12_FG.h** defines:

~~~cpp
const size_t scContext = 0x13375CC;
const size_t fgContext = 0x133757C;
~~~

Create returns these fixed values and destroy checks them to decide whether to release the active swapchain or FG context.

Again, this distinguishes context kind but not lifecycle instance.

### C. Wrapped swapchain teardown has no generation identity

A wrapper that reaches final Release can decide to retire the current FG lifecycle based on current global state and HWND, but there is no monotonic identity proving that this wrapper belongs to the currently published FG swapchain generation.

## Failure scenario

A stale callback can arrive after a newer lifecycle has already been published:

~~~text
create lifecycle A
    -> fake context marker = fixed value

retire/recreate
create lifecycle B
    -> same fake context marker

delayed destroy for A arrives
    -> marker still matches
    -> callback can act on current lifecycle B
~~~

The same general problem applies to a stale wrapped swapchain whose final Release occurs after a newer swapchain for the same HWND became current.

This is an ABA-style identity problem: pointer/window/type equality is not enough to prove lifecycle identity across recreation.

## Why this matters

Frame-generation APIs can defer or reorder destroy/release callbacks relative to application-side recreation.

A stale callback should only retire the object/lifecycle it was created for.

The invariant should be:

> Every independently creatable lifecycle must have an identity that is not reused by the next lifecycle.

## Fork behavior

PR #28 introduced two related mechanisms.

### Per-create FFX/FSR3 context tokens

Instead of fixed fake context constants being the complete identity, each create receives a unique token.

Destroy behavior becomes:

~~~text
destroy token == currently active token
    -> retire current lifecycle

destroy token != active token
    -> stale destroy
    -> retire/forget only the stale token
    -> do not touch the newer active lifecycle
~~~

### FG swapchain generation

The fork added a monotonically increasing FG swapchain generation.

A wrapped swapchain records the generation associated with it.

During final Release, it may retire the current FG lifecycle only when:

- it was the current tracked wrapper;
- its recorded generation is non-zero;
- its generation equals the current published generation;
- the FG/window identity still matches.

A stale wrapper final Release therefore cannot tear down a newer lifecycle on the same HWND.

## Upstream implementation recommendation

The exact fork token registry does not have to be copied. Upstream needs the invariant.

Possible implementation:

1. Give each FFX/legacy-FSR3 synthetic context creation a unique opaque token.
2. Track the currently active token per context kind.
3. A destroy for a stale token must not destroy the current FG object.
4. Give each published FG swapchain lifecycle a monotonically increasing generation or equivalent unique ID.
5. Associate wrappers/callback state with the generation observed at creation.
6. Require generation match before stale wrapper/final-release paths are allowed to retire the current lifecycle.
7. Keep lifecycle generation metadata separate from COM ownership. A generation proves identity, not ownership.

## Risk if unchanged

- delayed destroy for lifecycle A destroys lifecycle B;
- stale wrapper Release tears down a newer same-HWND swapchain;
- sporadic recreation failures that depend on callback ordering;
- bugs that disappear when logging changes timing.

## Suggested validation

- rapid create/destroy/create sequences;
- deliberately delayed destroy callback for the previous context;
- same HWND reused for a new FG lifecycle;
- stale wrapper final Release after the new lifecycle is active;
- failed recreation followed by retry;
- FFX API and legacy FSR3 input paths.


---

# 7. Serialize Streamline-to-XeFG frame-state mutations against Present

## Fork provenance

- reframework-0.9 PR #36: https://github.com/onehoon/OptiScaler/pull/36
- reframework-0.9 PR #37: https://github.com/onehoon/OptiScaler/pull/37

Relevant files:

- OptiScaler/OwnedMutex.h
- OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
- OptiScaler/framegen/xefg/XeFG_Dx12.cpp

## Problem being addressed

The Streamline DX12 input path mutates the same XeFG frame-generation object that the Present path consumes.

Current upstream master allows these operations to run without one common transaction boundary:

- **Sl_Inputs_Dx12::setConstants()**
  - advances frame tracking;
  - calls **XeFG_Dx12::EvaluateState()**;
  - updates camera state, jitter, motion-vector scale, reset state, and frame-time state.
- **Sl_Inputs_Dx12::reportResource()**
  - advances frame tracking;
  - selects a frame index;
  - publishes depth, motion-vector, HUDless, and UI resources into the FG object.
- **Sl_Inputs_Dx12::markPresent()**
  - mutates frame-boundary state and the FG frame count.
- **Sl_Inputs_Dx12::evaluateState()**
  - reads frame state and can request FG state changes.

At the same time, **FGHooks::FGPresent()** takes the FG mutex with logical owner 2 and calls **fg->Present()**.

Therefore the producer side can be updating frame identity, resources, or constants while the consumer side is entering XeFG Present unless the game/vendor callback ordering happens to serialize them.

The issue is not specific to Monster Hunter World and does not require E_ABORT 4004 to be proven as the root cause. MHW was the workload that motivated the investigation, but the underlying invariant is general:

> XeFG must not consume a partially updated frame transaction while Streamline callbacks are still publishing that frame's state.

## What PR #36 did on release/reframework-0.9

PR #36 introduced a small RAII transaction guard around the Streamline-to-XeFG mutation entry points.

Conceptually:

~~~text
Streamline frame callback
    -> if output is XeFG
    -> if current thread does not already own the FG transaction mutex
    -> acquire FG mutex as frame transaction owner
    -> perform frame/resource/constants mutation
    -> release at outer scope
~~~

It was applied to:

- setConstants();
- reportResource();
- evaluateState();
- markPresent().

Same-thread nested calls were allowed to reuse the outer transaction instead of recursively locking the non-recursive mutex.

This depends on the thread-aware OwnedMutex behavior described in Candidate 5.

## What PR #37 corrected

PR #37 removed an ownership violation from **XeFG_Dx12::EvaluateState()**.

Older code contained logic conceptually equivalent to:

~~~cpp
if (Mutex.isOwnedByCurrentThread(2))
    Mutex.unlockThis(2);
~~~

EvaluateState did not acquire that outer owner-2 transaction, so it must not release it.

The owner of a transaction must also be the scope responsible for releasing it.

The invariant is:

> Callee code must not release a frame transaction acquired by its caller.

## Important master-specific difference

The release/reframework-0.9 implementation **must not be copied literally into current upstream master**.

Current upstream master has:

~~~cpp
void XeFG_Dx12::EvaluateState(...)
{
    OwnedLockGuard lock(Mutex, 555);
    ...
}
~~~

The 0.9 branch did not have this internal owner-555 lock when PR #36/#37 landed.

If upstream simply adds the PR #36 outer owner-2 transaction around **setConstants()**, then:

~~~text
setConstants()
    -> acquire Mutex owner 2
    -> XeFG_Dx12::EvaluateState()
        -> attempts to acquire the same non-recursive Mutex as owner 555
        -> self-deadlock
~~~

Therefore the upstream implementation needs to adapt the locking model.

## Recommended master-native implementation

Candidate 5, thread-aware OwnedMutex, should be implemented first or together with this work.

Then choose one explicit ownership model.

A practical model is:

1. Streamline XeFG entry points establish the outer frame transaction.
2. **XeFG_Dx12::EvaluateState()** acquires its internal owner-555 lock only when the current thread does not already own the FG mutex.
3. If the current thread already owns the outer frame transaction, EvaluateState executes within that transaction without recursively locking.
4. EvaluateState never releases owner 2; the outer RAII transaction releases it.
5. Non-Streamline callers of EvaluateState keep the existing internal serialization because they do not arrive with the outer transaction.
6. Present continues to use the same FG mutex, so it cannot overlap a cross-thread Streamline frame transaction.
7. Same-thread nested vendor callbacks remain reentrant-safe through the thread-aware ownership check.

Conceptually:

~~~text
Streamline path:
    transaction owner 2
        -> EvaluateState notices same-thread ownership
        -> no nested lock
        -> SetResource / SetFrameCount / constants updates
    transaction scope releases owner 2

Other input path:
    no outer transaction
        -> EvaluateState acquires owner 555 itself
        -> existing standalone serialization preserved

Present on another thread:
    waits for owner 2 transaction to complete
    -> consumes a coherent frame state
~~~

An alternative is a dedicated frame-transaction mutex shared by the Streamline producer and Present consumer, but adding another lock should be justified carefully because lock ordering with the existing FG lifecycle mutex then becomes another concern.

Reusing the existing FG mutex with explicit same-thread ownership appears closer to the current architecture.

## Why this is worth upstream review even without proven E_ABORT causality

This proposal should not be presented as:

> "PR36/37 fixes Intel MHW E_ABORT 4004."

That causal claim is not established.

It can be presented as:

> "Streamline callbacks publish a multi-step XeFG frame state while Present consumes the same object. Current master has no common transaction boundary between those producer mutations and XeFG Present. The fork serialized those mutations and made transaction ownership caller-scoped."

That is an independently reviewable concurrency property.

Even if it does not prove the cause of a specific Intel error, avoiding partially observed FG frame state is a defensible synchronization improvement.

## Scope recommendation

Keep the first upstream version narrow:

- XeFG output only;
- Streamline DX12 input only;
- only frame-state mutation entry points;
- no FSRFG/DLSSG behavior change;
- no broad recursive-mutex conversion;
- no assumption that every logical owner check means same-thread reentrancy.

This reduces regression risk.

## Risks / things to audit

Before landing, upstream should inspect:

- every function called under the outer transaction for nested FG mutex acquisition;
- lock ordering with _frameBoundaryMutex;
- lifecycle/resize locks that may call back into Streamline;
- same-thread vendor callbacks from Present;
- shutdown paths;
- whether Streamline SL1 requires the same treatment separately.

A debug assertion/log can help identify unexpected nested ownership instead of silently bypassing it.

## Suggested validation

- Streamline input -> XeFG output;
- Present on a different thread from resource/constants callbacks;
- same-thread nested callbacks;
- repeated FG enable/disable;
- resize/recreation while Streamline callbacks are active;
- Intel MHW E_ABORT 4004 reproduction workload as a regression test, without claiming it as proof of root cause;
- other Streamline titles to catch over-serialization or deadlock regressions;
- logging enabled/disabled comparison.



---

# 8. Optional REFramework pre-retire XeFG lifecycle handshake

## Fork provenance

- reframework-0.9 PR #29: https://github.com/onehoon/OptiScaler/pull/29
- reframework-0.9 PR #31: https://github.com/onehoon/OptiScaler/pull/31
- reframework-0.9 PR #34: https://github.com/onehoon/OptiScaler/pull/34
- reframework-0.9 PR #35: https://github.com/onehoon/OptiScaler/pull/35

Relevant OptiScaler files:

- OptiScaler/framegen/xefg/XeFG_Dx12.cpp
- OptiScaler/wrapped/wrapped_swapchain.cpp
- OptiScaler/wrapped/wrapped_swapchain.h

Matching custom REFramework side:

- exports **REFramework_XeFG_PreRetireSwapchainV1**
- tracks the Intel XeFG presentation/swapchain lifecycle and can detach before OptiScaler retires it

## Scope and prerequisite

This candidate is different from Candidates 1-7.

It is not a standalone OptiScaler correctness change. It is an **optional cross-project lifecycle integration** for environments where a matching REFramework build explicitly tracks the XeFG presentation lifecycle.

The handshake is only useful when the matching REF export is present.

When the export is unavailable, OptiScaler should continue using its normal standalone retirement path.

This makes the integration optional rather than a hard dependency on REFramework.

## Why the handshake is needed

In the custom OptiScaler + custom REFramework XeFG environment, both components have lifecycle responsibilities around the same Intel presentation objects.

The ownership model is intentionally asymmetric:

~~~text
OptiScaler
    -> owns the public XeFG lifecycle and game-side swapchain/queue transition

REFramework
    -> observes/binds to Intel XeFG presentation lifecycle state
    -> may hold tracking references/callback state that must be detached before retirement
~~~

Without an explicit handoff, OptiScaler can begin destroying the XeFG context or releasing the final public proxy while REF still believes that lifecycle is active.

That creates ordering ambiguity:

~~~text
OptiScaler retires XeFG proxy/context
    while
REF still tracks/binds that same lifecycle
~~~

Depending on timing, this can surface as:

- reentrant final Release;
- stale REF tracking state;
- late callback access during retirement;
- shutdown AVs;
- recreation failures because one side still considers the old lifecycle active.

The desired invariant is:

> If REFramework is actively tracking the XeFG lifecycle, OptiScaler must obtain an explicit safe-to-retire result from REF before live retirement proceeds.

## ABI used by the fork

The custom REFramework exposes:

~~~text
REFramework_XeFG_PreRetireSwapchainV1
~~~

OptiScaler dynamically discovers the export at runtime.

Conceptually, the call is:

~~~text
PrepareREFForXeFGProxyRetire(publicProxy, swapchainContext, hwnd)
~~~

REF returns one of three meaningful outcomes:

~~~text
SafeNotTracked
    -> REF is not tracking this lifecycle
    -> OptiScaler may continue

SafeDetached
    -> REF was tracking it and successfully detached
    -> OptiScaler may continue

Blocked
    -> REF cannot safely detach / retirement is not safe
    -> OptiScaler must not continue live retirement
~~~

Unknown future return values are treated as blocked by the V1 caller.

If module enumeration succeeds but the export does not exist, the result is **NotAvailable**, which means normal standalone behavior continues.

## PR #29: final public proxy handoff

PR #29 added the initial handoff at the most important ownership boundary:

~~~text
final public XeFG proxy Release
    -> ask REF to detach
    -> only then continue XeFG teardown / final proxy release
~~~

This addresses the case where REF may still be tracking the presentation proxy at the moment the last public OptiScaler reference is being consumed.

## PR #31: extend handoff to all live-retirement paths

PR #31 generalized the handshake so it is not limited to final public proxy release.

The pre-retire check is also performed before:

- same-HWND recreation;
- explicit ReleaseSwapchain();
- final proxy release.

This matters because the old lifecycle can be retired for reasons other than final wrapper Release.

The important rule is:

> Every live XeFG lifecycle retirement path that can invalidate REF-tracked presentation state should pass through the same pre-retire gate.

## PR #34: protect final Release against REF reentrancy

The handshake itself can cause reentrancy.

REF may need to perform AddRef/Release operations while detaching its presentation tracking.

If this occurs during the wrapper's final Release path, the wrapper can re-enter finalization while its first finalization is still in progress.

PR #34 added explicit final-release ownership state so that a reentrant final Release is consumed safely instead of running the retirement path twice.

Conceptually:

~~~text
wrapper final Release begins
    -> mark final release in progress
    -> call REF pre-retire handshake
        -> REF detach may cause AddRef/Release
            -> reentrant final Release sees finalization already active
            -> does not run teardown again
    -> original owner finishes retirement
    -> real swapchain released once
    -> wrapper destroyed once
~~~

This is why PR #34 is part of the handshake package rather than an unrelated wrapper fix.

## PR #35: process-shutdown exception

The live-retirement ordering rule is not automatically correct during process shutdown.

During process teardown:

- module destruction order is no longer normal runtime order;
- REF may itself be shutting down;
- calling into external module code can be unsafe;
- vendor teardown paths may already be in partially dismantled state.

The fork therefore skips the external REF pre-retire handshake when:

~~~text
State::isShuttingDown == true
~~~

Live runtime retirement still uses the handshake.

This separation is important:

~~~text
live retirement:
    OptiScaler -> REF pre-retire -> XeFG teardown

process shutdown:
    avoid external REF handoff
    -> follow shutdown-specific ownership policy
~~~

Upstream should preserve this distinction if it adopts the integration.

## Why this is now relevant for upstream review

This should be considered an upstream candidate if the project intends to support or recommend the matching custom REFramework build for XeFG users.

The integration is intentionally:

- dynamically discovered;
- optional;
- versioned by export name;
- inactive when the matching REF export is absent.

Therefore upstream can support the integration without introducing a hard build-time or runtime dependency on REFramework.

This is useful when OptiScaler and REFramework are both participating in the Intel XeFG presentation lifecycle, because it makes the retirement ordering explicit instead of relying on incidental COM reference timing.

## Upstream implementation recommendation

If upstream wants to support the custom REF integration:

1. Keep the handshake optional and runtime-discovered.
2. Use a versioned export/ABI.
3. Treat **SafeNotTracked** and **SafeDetached** as safe to continue.
4. Treat **Blocked** and unknown V1 return values as fail-closed for live retirement.
5. Use the same pre-retire gate for every live lifecycle retirement path that invalidates REF-tracked XeFG presentation state.
6. Protect final wrapper Release from reentrant finalization.
7. Skip the external handshake during process shutdown.
8. Keep the integration separate from core OptiScaler ownership rules; Candidates 1-7 must remain correct without REF installed.

## What should not be assumed

The handshake does not mean:

- REF owns the XeFG lifecycle;
- OptiScaler may skip its own teardown correctness;
- a REF return value replaces COM ownership;
- every REFramework build supports this ABI.

The contract is only:

> REF confirms whether its own tracking state is safe for OptiScaler to retire the current XeFG lifecycle.

## Suggested validation

Test all combinations:

- matching custom REF installed;
- no REF installed;
- REF installed without the V1 export;
- REF returns SafeNotTracked;
- REF returns SafeDetached;
- REF returns Blocked;
- same-HWND recreation;
- explicit ReleaseSwapchain;
- wrapper final Release;
- reentrant AddRef/Release during detach;
- process shutdown;
- Intel and non-Intel GPUs using XeFG output.

## Packaging recommendation

This should be reviewed as one coordinated integration package:

~~~text
PR29
    -> initial pre-retire ABI at final proxy boundary

PR31
    -> all live-retirement paths use the gate

PR34
    -> reentrant final-release protection

PR35
    -> process-shutdown exception
~~~

Taking only one piece can leave another retirement path unprotected or can introduce reentrancy/shutdown problems.


---

# Suggested upstream review order

The candidates are related, but they do not all need to land in one PR.

A low-risk review order would be:

1. **XeFG fail-closed teardown**
2. **Remove all remaining Release-until-zero ownership drains**
3. **DX12 wrapper/QI COM lifetime hardening**
4. **XeFG command queue lifetime ownership**
5. **Thread-aware OwnedMutex + thread-local hook guards**
6. **Lifecycle generation / per-create context tokens**
7. **Streamline-to-XeFG frame transaction serialization (master-adapted)**
8. **Optional REFramework pre-retire XeFG lifecycle handshake**

Reasons for this order:

- Items 1 and 2 correct explicit ownership/lifecycle violations already visible in current master.
- Items 3 and 4 make COM lifetimes explicit before adding more lifecycle identity state.
- Item 5 changes synchronization semantics and is a prerequisite for a safe master-native version of Item 7.
- Item 6 is broader lifecycle hardening and is easiest to reason about after ownership rules are already clear.
- Item 7 should follow Item 5 because current master EvaluateState has its own owner-555 lock and cannot accept the 0.9 patch literally.
- Item 8 is optional integration work and should be reviewed as one PR29/31/34/35 package only if upstream intends to support the matching custom REFramework lifecycle ABI.

The fork release/reframework-0.9 implementation order should not be treated as a required upstream order because current master has additional HUDfix, resource tracking, DX11-with-DX12, low-latency, and Streamline changes.

---

# Changes intentionally not proposed here

## Intel Reflex selective DXGI identity quirk

Excluded intentionally.

## XeLL release/0.9 fail-closed implementation

The release branch contains additional XeLL context quarantine and fakenvapi publication ownership work.

Current upstream master has evolved its low-latency/XeLL architecture substantially, so the 0.9 implementation is not an appropriate direct proposal. The invariant can be revisited separately against current master.

---

# Reference links

Fork master work:

- PR #1 — XeFG destroy failure lifecycle  
  https://github.com/onehoon/OptiScaler/pull/1
- PR #6 — remaining XeFG swapchain COM ownership drains  
  https://github.com/onehoon/OptiScaler/pull/6
- PR #13 — ownership-boundary refinement  
  https://github.com/onehoon/OptiScaler/pull/13
- PR #14 — XeFG exact-success / warning policy  
  https://github.com/onehoon/OptiScaler/pull/14

Fork reframework-0.9 work:

- PR #28 — swapchain generation, FFX/legacy FSR3 context identity, remaining force-drain cleanup  
  https://github.com/onehoon/OptiScaler/pull/28
- PR #32 — D3D12 command-queue lifetime across XeFG recreation  
  https://github.com/onehoon/OptiScaler/pull/32
- PR #33 — thread-aware FG mutex ownership and reentrancy guards  
  https://github.com/onehoon/OptiScaler/pull/33
- PR #38 — DX12 wrapper COM ownership hardening  
  https://github.com/onehoon/OptiScaler/pull/38

Relevant upstream change already landed:

- 34917612 — Disable XeFG backbuffer release hack  
  https://github.com/optiscaler/OptiScaler/commit/349176128f5ce87fb81a4c636569c60c44557fec

That upstream change addresses one instance of the ownership issue. Candidate 2 above is the follow-through audit for the equivalent patterns that remain elsewhere.
