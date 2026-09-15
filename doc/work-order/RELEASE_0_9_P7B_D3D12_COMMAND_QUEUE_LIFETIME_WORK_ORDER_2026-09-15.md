# Release 0.9 P7-B Work Order — Own D3D12 Command Queues Across the XeFG Lifecycle

Date: 2026-09-15

## Status

Implementation work order.

P7-A is complete. P7-B is the next lifecycle-hardening step for the `reframework-0.9` compatibility branch.

This work order is intentionally designed from the **current `reframework-0.9` source only**.

> Do not copy, backport, or treat `master` as the implementation source of truth for this work.

`master` may have different queue, XeLL, hook, or frame-generation architecture and is not assumed to be correct for this branch. P7-B must preserve the current release/0.9 architecture and the P1-P7-A lifecycle boundaries already established here.

The goal is:

> Any D3D12 command queue used after a create call returns must have explicit COM ownership for exactly the lifecycle that may still use it, and an old lifecycle must be drained with its old queue before a replacement queue is published.

---

## 1. Verified target baseline

Repository: `onehoon/OptiScaler`

Branch: `reframework-0.9`

Baseline reviewed:

```text
d78cb589c889da5716e7a85eaf1760bf7e93d4be
P7-A: detach REF before XeFG lifecycle retirement (#31)
```

Relevant files reviewed at this baseline:

```text
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/framegen/IFGFeature_Dx12.h
OptiScaler/framegen/IFGFeature_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/State.h
OptiScaler/Util.cpp
OptiScaler/Util.h
```

No REFramework source change is expected for P7-B.

---

## 2. Code-proven problem A — `FG_Hooks` stores a queue after dropping the only reference it acquired

Both `FGHooks::CreateSwapChain(...)` and `CreateSwapChainForHwnd(...)` currently do:

```cpp
ID3D12CommandQueue* cq = nullptr;
if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    return E_INVALIDARG;

currentCommandQueue = cq;
cq->Release();
```

`currentCommandQueue` is only a raw pointer:

```cpp
static ID3D12CommandQueue* currentCommandQueue = nullptr;
```

Later, `WaitForGPUIdle()` dereferences it:

```cpp
currentCommandQueue->Signal(resizeFence, resizeFenceValue);
```

Therefore OptiScaler does not own a reference corresponding to the later `Signal()` use.

The game may normally keep the queue alive, but that is an external lifetime assumption, not an OptiScaler lifecycle invariant.

---

## 3. Code-proven problem B — replacement overwrites the queue before the old lifecycle is drained

The same create functions assign `currentCommandQueue = cq` **before** checking `previousFGSwapchain` and before this call:

```cpp
WaitForGPUIdle();
```

Therefore the current sequence is conceptually:

```text
receive candidate queue B
    -> overwrite currentCommandQueue with B
    -> discover old swapchain/lifecycle A
    -> WaitForGPUIdle()
       -> Signal(queue B)
    -> retire lifecycle A
```

If lifecycle A was initialized against queue A and a recreation arrives with a different queue B, the drain is performed on the candidate/new queue rather than the queue associated with the old lifecycle.

This is a correctness problem independent of whether either raw pointer happens to remain alive.

Required invariant:

```text
old lifecycle A
    -> drain with queue A
    -> retire A
    -> initialize candidate lifecycle B with queue B
    -> only after B commits, publish queue B
```

---

## 4. Code-proven problem C — XeFG stores the vendor/init queue as a borrowed raw pointer

`IFGFeature_Dx12` currently contains:

```cpp
ID3D12CommandQueue* _gameCommandQueue = nullptr;
```

The release/0.9 XeFG create paths resolve a queue and, after successful XeFG swapchain creation, do:

```cpp
_gameCommandQueue = realQueue;
```

`XeFG_Dx12::SetCommandQueue(...)` likewise does:

```cpp
_gameCommandQueue = queue;
```

No AddRef is acquired by either assignment.

This is especially important because the release/0.9 `Util::CheckForRealObject(...)` helper does not transfer ownership. It internally performs a `QueryInterface`, writes the returned object to the output pointer, and then immediately calls:

```cpp
(*ppRealObject)->Release();
```

before returning `true`.

So a `realQueue` returned through `CheckForRealObject` is intentionally a borrowed pointer.

---

## 5. The borrowed XeFG queue is used later

The queue is not only needed during `xefgSwapChainD3D12InitFromSwapChainDesc`.

Current XeFG code uses `_gameCommandQueue` after creation, including delayed paths such as:

```cpp
_gameCommandQueue->ExecuteCommandLists(...);
```

from `Deactivate()` and `Present()`.

`IFGFeature_Dx12::GetCommandQueue()` also returns the same raw pointer to callers.

Therefore its required lifetime extends beyond the create call.

---

## 6. Current teardown does not define queue retirement

After P7-A, `XeFG_Dx12::ReleaseSwapchainLocked(...)` correctly serializes the swapchain lifecycle and performs:

```text
REF pre-retire handoff already completed by caller
    -> FG mutex
    -> DestroyFGContext / Deactivate
    -> optional final proxy Release
    -> XeFG/XeLL teardown
    -> quarantine checks
    -> ReleaseObjects
    -> success
```

But `_gameCommandQueue` has no ownership state and is not explicitly retired there.

The queue is required through at least `Deactivate()` / command-list submission and must remain alive through all teardown work that can still use it.

If teardown fails or the lifecycle is quarantined, the old queue must not be dropped merely because a retirement attempt started.

---

## 7. Preserve/reuse path exposes the same identity problem

When `FGPreserveSwapChain` reuses the existing XeFG lifecycle, `XeFG_Dx12::CreateSwapchain*()` returns from the ResizeBuffers path before assigning a new `_gameCommandQueue`.

That is consistent with preserving the existing lifecycle.

However `FG_Hooks.cpp` currently overwrites its separate `currentCommandQueue` with the newly supplied candidate queue before it knows the old lifecycle will be reused.

This can create two different queue identities for one preserved lifecycle:

```text
XeFG lifecycle queue = old queue A
FGHooks idle queue   = newly supplied queue B
```

P7-B must not allow this split ownership/identity state.

---

# P7-B invariants

## 8. Required ownership rules

The implementation must satisfy all of the following.

### 8.1 XeFG owns the queue it may use asynchronously/later

Once a new XeFG swapchain lifecycle is successfully initialized and committed, XeFG must hold a COM reference to the D3D12 command queue associated with that lifecycle.

Do not rely on the game, REF, a wrapper, or a leaked QueryInterface reference to keep it alive.

### 8.2 Candidate queue is local until lifecycle commit

A queue passed to a new `CreateSwapChain*` call is a **candidate**.

Do not replace the currently published lifecycle queue before:

- the old lifecycle has been drained and retired when required; and
- the new XeFG lifecycle has successfully completed its critical initialization.

### 8.3 Drain the old lifecycle with the old lifecycle queue

If a current XeFG lifecycle exists, `WaitForGPUIdle` before replacement must signal the queue owned by that current lifecycle, not the incoming candidate queue.

### 8.4 Preserve/reuse keeps the old queue

If `FGPreserveSwapChain` reuses the existing lifecycle, do not publish or bind the incoming candidate queue as though a new lifecycle were committed.

### 8.5 Failed/quarantined teardown retains the old queue

If P5/P6/P7-A teardown returns false, XeFG Destroy fails, XeLL teardown fails, or recreation is quarantined, retain the queue ownership associated with the uncertain old lifecycle.

Do not create a queue UAF by dropping the owner on a failed retirement.

### 8.6 Successful retirement releases ownership only after the last possible queue use

The old queue may be released only after:

```text
Deactivate / pending command-list submission
    -> vendor/lifecycle teardown
    -> ReleaseObjects
    -> retirement confirmed successful
```

### 8.7 Stale final-proxy release cannot clear a newer queue

The P4/P5 stale final-proxy branch must remain release-only for that stale proxy. It must not reset the queue owned by a newer active lifecycle.

### 8.8 No global base-class ownership rewrite

Do **not** simply change `IFGFeature_Dx12::_gameCommandQueue` to `ComPtr` in this PR.

That would silently change FSRFG and other shared backend semantics.

P7-B should add explicit XeFG ownership while retaining the current base raw pointer as a non-owning alias for compatibility.

---

# P7-B implementation

## 9. Add a XeFG-owned queue reference

Recommended private member in `XeFG_Dx12`:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> _ownedGameCommandQueue;
```

The existing inherited raw member remains:

```cpp
_gameCommandQueue
```

but for XeFG it becomes only an alias to the owned object:

```cpp
_gameCommandQueue = _ownedGameCommandQueue.Get();
```

A small helper is recommended to keep assignment semantics centralized, for example:

```cpp
void XeFG_Dx12::CommitGameCommandQueue(ID3D12CommandQueue* queue)
{
    _ownedGameCommandQueue = queue; // AddRef
    _gameCommandQueue = _ownedGameCommandQueue.Get();
}
```

The exact name may differ.

Do not use `Attach()` for an ordinary borrowed queue pointer. The owner must acquire its own reference.

---

## 10. Keep the resolved queue alive during XeFG initialization

Both `CreateSwapchain(...)` and `CreateSwapchain1(...)` currently obtain a borrowed `realQueue`.

After resolving the real queue, create a local owning candidate before invoking Intel XeFG:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> queueCandidate;
queueCandidate = realQueue; // acquire P7-B's own reference
```

Then use:

```cpp
queueCandidate.Get()
```

for `xefgSwapChainD3D12InitFromSwapChainDesc(...)`.

This local candidate must remain alive through all initialization and cleanup paths in that call.

Do not commit it to `_ownedGameCommandQueue` merely because Intel Init returned.

The recommended commit point is after the critical XeFG init and `D3D12GetSwapChainPtr` have both completed with exact success and a valid public swapchain has been obtained.

Conceptually:

```cpp
result = XeFGProxy::D3D12InitFromSwapChainDesc()(
    _swapChainContext,
    ...,
    queueCandidate.Get(),
    ...);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    return AbortSwapchainInitialization(...);

// GetSwapChainPtr exact-success validation...

_ownedGameCommandQueue = std::move(queueCandidate);
_gameCommandQueue = _ownedGameCommandQueue.Get();
```

The existing XeFG exact-success lifecycle policy remains unchanged.

---

## 11. Make `SetCommandQueue` ownership-safe for XeFG

Current code:

```cpp
void XeFG_Dx12::SetCommandQueue(
    FG_ResourceType type,
    ID3D12CommandQueue* queue)
{
    _gameCommandQueue = queue;
}
```

must no longer leave a borrowed queue behind.

Use the same owning helper/state:

```cpp
void XeFG_Dx12::SetCommandQueue(
    FG_ResourceType type,
    ID3D12CommandQueue* queue)
{
    CommitGameCommandQueue(queue);
}
```

A null queue must clear both the owner and raw alias.

Do not add a second independent queue owner for `SetCommandQueue`.

Do not redesign the meaning of `FG_ResourceType` in this PR.

---

## 12. Retire the XeFG queue only on confirmed successful lifecycle retirement

In `ReleaseSwapchainLocked(...)`, do not reset the queue near function entry.

The old queue is still required by `DestroyFGContext()` / `Deactivate()` and possibly pending command-list submission.

Required relative order:

```text
PrepareREFForSwapchainRetire   [P7-A caller]
    -> FG mutex
    -> DestroyFGContext / Deactivate
       -> old owned queue remains valid
    -> optional final proxy Release
    -> DestroySwapchainContext
    -> fail-closed quarantine checks
    -> ReleaseObjects
    -> clear raw alias
    -> Reset owned queue
    -> return success
```

Recommended tail after all existing failure exits:

```cpp
ReleaseObjects();

_gameCommandQueue = nullptr;
_ownedGameCommandQueue.Reset();
```

If any existing failure path returns before the successful retirement tail, the queue must remain owned.

Do not clear it from the stale-final-proxy branch.

---

## 13. Fix `FG_Hooks` candidate-vs-current queue ordering

Do not use this pattern anymore:

```cpp
ID3D12CommandQueue* cq = nullptr;
pDevice->QueryInterface(IID_PPV_ARGS(&cq));
currentCommandQueue = cq;
cq->Release();
```

Use a local owning candidate for the duration of the create call:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> candidateQueue;
if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&candidateQueue))))
{
    LOG_ERROR("FG Feature requires D3D12 Command Queue!");
    return E_INVALIDARG;
}
```

Pass `candidateQueue.Get()` to `fg->CreateSwapchain*()`.

The incoming queue must not overwrite the currently published queue before old-lifecycle drain/release decisions are complete.

---

## 14. `WaitForGPUIdle` must use the current lifecycle queue

For XeFG, the authoritative queue for an already-created lifecycle should be the queue owned by the current FG feature:

```cpp
auto* fg = State::Instance().currentFG;
auto* lifecycleQueue = fg != nullptr ? fg->GetCommandQueue() : nullptr;
```

For the XeFG path, call the idle wait with that old lifecycle queue **before** `fg->CreateSwapchain*()` can retire it.

A recommended API shape is:

```cpp
static void WaitForGPUIdle(ID3D12CommandQueue* queue)
{
    if (queue == nullptr || resizeFence == nullptr || resizeFenceEvent == nullptr)
        return;

    ++resizeFenceValue;
    queue->Signal(resizeFence, resizeFenceValue);
    ...
}
```

Then replacement becomes conceptually:

```cpp
if (!preserve && previousFGSwapchain != nullptr)
{
    auto* oldQueue = fg != nullptr ? fg->GetCommandQueue() : nullptr;
    WaitForGPUIdle(oldQueue);
}

scResult = fg->CreateSwapchain(..., candidateQueue.Get(), ...);
```

Do not pass `candidateQueue` to the pre-retire idle wait for the old lifecycle.

For non-XeFG backends, preserve existing behavior unless a minimal holder conversion is required for compilation/safety. Do not refactor FSRFG internals in P7-B.

---

## 15. Keep the FGHooks resize-sync queue/fence pair coherent

`FG_Hooks.cpp` also keeps a resize fence and event across calls.

If a local `currentCommandQueue` holder remains for resize/telemetry fallback, it must become an owning holder, preferably:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> currentCommandQueue;
```

and it must represent the **successfully published current lifecycle**, not the incoming candidate.

Required publication rule:

```text
candidate queue arrives
    -> old lifecycle drain uses old queue
    -> create/recreate succeeds
    -> if this is a new lifecycle, publish candidate queue + rebuild fence
```

If `reusedExistingLifecycle == true`, do not replace the published queue/fence with the candidate. The old lifecycle and its queue remain authoritative.

It is acceptable to encapsulate the holder/fence/event/value/generation into one small internal struct in `FG_Hooks.cpp` if that makes the invariant clearer. Do not create a new global subsystem.

Recommended diagnostic metadata:

```cpp
uint64_t generation;
```

matching `State::currentFGSwapchainGeneration`.

This makes stale cleanup auditable and prevents an old retirement from clearing synchronization state for a newer lifecycle.

---

## 16. Clear resize-sync state only when its lifecycle is actually retired

On a confirmed successful final/current lifecycle release, the FGHooks-side queue/fence state may be cleared only if it still belongs to the generation being retired.

The existing `hkFGRelease` already captures:

```cpp
const auto generationBeforeRelease =
    state.currentFGSwapchainGeneration.load(...);
```

Reuse that identity rather than inventing pointer-only cleanup.

Conceptual helper:

```cpp
void ResetResizeSyncIfGeneration(uint64_t generation)
{
    if (resizeSync.generation != generation)
        return;

    resizeSync.queue.Reset();
    resizeSync.fence.Reset();
    CloseHandle(...);
    resizeSync.value = 0;
    resizeSync.generation = 0;
}
```

If implementation keeps the existing raw `resizeFence`, perform the equivalent explicit Release/Close cleanup.

A stale final proxy must never clear a newer generation's queue/fence state.

---

## 17. Creation failure handling

There are two distinct failure cases.

### Failure before old lifecycle retirement

Keep old queue/fence ownership unchanged.

### Failure after old lifecycle retired but before new lifecycle commits

The candidate queue is local and must be released automatically.

Do not publish it.

If FGHooks-side synchronization state still refers to the now-retired old generation, clear that state once the existing lifecycle state proves the old generation is no longer current.

Do not guess that every failed create retired the old lifecycle.

Use the existing current swapchain/generation state to distinguish these cases.

---

## 18. Keep telemetry queue use internally consistent

Current `FGPresent(...)` checks one queue:

```cpp
if (willPresent && currentCommandQueue != nullptr)
```

but calls:

```cpp
UpscalerTimeDx12::ReadUpscalingTime(
    State::Instance().currentCommandQueue);
```

If P7-B converts or encapsulates the FGHooks queue holder, make the guard and argument refer to the same owned/current queue source.

For example:

```cpp
auto* queue = currentCommandQueue.Get();
if (willPresent && queue != nullptr)
    UpscalerTimeDx12::ReadUpscalingTime(queue);
```

Do not redesign the wider `State::currentCommandQueue` architecture in this PR.

---

# Required non-changes

## 19. Do not import master architecture

Explicitly out of scope:

- copying the current master FGHooks queue model;
- copying master XeFG queue/lifecycle code;
- restructuring release/0.9 around master naming or ownership conventions;
- changing XeLL integration to master-style ownership;
- treating a master ref leak or longer-lived raw pointer as a valid ownership model.

The review baseline and implementation decisions for P7-B are release/0.9 only.

## 20. Do not change P7-A lock ordering

P7-A's rule remains:

```text
REF pre-retire handoff
    -> FG mutex
    -> vendor/lifecycle teardown
```

Queue ownership changes must not move REF handoff inside the FG mutex.

## 21. Do not change XeFG/XeLL result policy

P6 semantics remain unchanged.

For XeFG:

```text
0  = exact success
>0 = warning/non-error classification, but no critical lifecycle commit
<0 = error
```

Do not use P7-B to alter result-code handling.

XeLL remains exact-success gated per the pinned release/0.9 SDK.

## 22. Do not change REF

No REFramework change is expected.

REF already owns its captured presentation queue independently while a REF binding is active and drops that binding during P7-A handoff. P7-B is about OptiScaler retaining its own queue for its own later use.

---

# Suggested PR shape

## 23. Files

Expected primary files:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
```

`IFGFeature_Dx12.h` should not need an ownership-type change.

Avoid unrelated files.

Recommended PR title:

```text
P7-B: own XeFG D3D12 command queues across lifecycle
```

---

# Validation

## 24. Static/build validation

Required:

```text
Release x64 build
clang-format check
git diff --check
```

Audit all assignments and dereferences of:

```text
_gameCommandQueue
currentCommandQueue
resizeFence
resizeFenceEvent
```

For XeFG, there must be no path that stores a long-lived command queue without an owned COM reference.

## 25. Reference/lifetime validation

Add temporary debug logging if useful for testing:

```text
[XeFG][QueueLifecycle] action = candidate_acquired
[XeFG][QueueLifecycle] action = committed
[XeFG][QueueLifecycle] action = retained_on_failed_retire
[XeFG][QueueLifecycle] action = retired
[FG][QueueLifecycle] action = wait_old_generation
[FG][QueueLifecycle] action = publish_generation
[FG][QueueLifecycle] action = clear_generation
```

Include pointer and generation where available.

Logging may remain if concise and useful, but do not build a permanent verbose tracing subsystem.

## 26. Required scenario tests

### A. Normal create / present / release

```text
candidate queue acquired
new XeFG lifecycle commits
queue remains valid through Present/Deactivate
successful retirement releases queue owner
```

### B. Replacement with the same queue

Old lifecycle must drain and retire cleanly, then the new lifecycle may commit the same COM identity as a new owned reference.

### C. Replacement with a different queue

This is the critical P7-B test:

```text
old lifecycle = queue A
incoming create = queue B
WaitForGPUIdle must Signal A
old lifecycle retires
new Intel Init uses B
only successful new lifecycle publishes B
```

### D. Preserve/reuse path

Incoming candidate B must not replace the queue owned by preserved lifecycle A.

### E. REF active

P7-A handoff must still complete before FG mutex teardown. Queue ownership must not add a new REF lock dependency.

### F. XeFG Destroy failure / quarantine

Old queue ownership must remain intact when retirement returns false.

No candidate queue may be published.

### G. XeLL teardown failure

Same rule: retain old lifecycle queue while lifecycle state is uncertain/quarantined.

### H. New lifecycle initialization failure

Candidate queue must be released and not published.

If the old lifecycle was already successfully retired, FGHooks resize-sync metadata must not continue claiming that old generation is current.

### I. Final proxy release

Successful final release clears the queue/fence state for that generation only.

### J. Stale proxy release

A stale proxy from generation N must not clear queue ownership or resize-sync state for generation N+1.

### K. Repeated resize / ResizeBuffers1

No invalid queue access, stale queue Signal, or queue/fence identity drift.

---

## 27. Runtime matrix

Minimum manual runtime matrix when available:

```text
MHW + REF + XeFG
DD2 + REF + XeFG
RE9/other current REF test title + XeFG
```

Exercise:

```text
launch
FG enable/disable
window mode change
ResizeBuffers / ResizeBuffers1
swapchain recreation
rapid menu/settings transitions
clean game exit
```

A synthetic queue-A -> queue-B recreation test is strongly recommended because most games normally reuse one queue and can hide the exact bug P7-B is meant to close.

---

# Acceptance criteria

P7-B is complete only when all of the following are true:

- XeFG has explicit COM ownership of every `_gameCommandQueue` it may use after create returns.
- A resolved real queue is held through Intel initialization by a local owning candidate.
- Candidate queue is committed only after critical new-lifecycle initialization succeeds.
- Old lifecycle drain uses old lifecycle queue, not incoming candidate queue.
- Preserve/reuse does not rebind queue ownership to a candidate.
- Failed/quarantined teardown retains the old queue owner.
- Successful retirement clears XeFG queue ownership only after the last queue use.
- Stale proxy release cannot clear a newer lifecycle queue.
- FGHooks queue/fence state is not published before lifecycle commit.
- FGHooks queue/fence cleanup is generation-safe if generation metadata is used.
- The Present telemetry guard and queue argument are internally consistent if touched.
- No REFramework change is introduced.
- No master code/architecture is imported.
- P7-A ordering remains intact.
- P6 XeLL and XeFG result semantics remain intact.
- Release x64 build and formatting checks pass.

Once these conditions are met, P7-B closes the release/0.9 D3D12 command-queue lifetime gap without broadening the patch into unrelated master or backend architecture changes.
