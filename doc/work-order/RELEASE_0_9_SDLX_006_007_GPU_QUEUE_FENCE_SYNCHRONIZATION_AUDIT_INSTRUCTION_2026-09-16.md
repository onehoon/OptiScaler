# Release 0.9 — SDLX-006 / SDLX-007 GPU Queue, Fence, and Resize-Queue Synchronization Audit Instruction

**Status:** Independent source-audit instruction — audit first, no implementation authorized  
**Target repository:** `onehoon/OptiScaler`  
**Target branch:** `reframework-0.9`  
**Pinned code audit base:** `036aae89e31447b21e1cab4e8d2c5c72421f9dc7`  
**Pinned base subject:** `fix: harden DX12 wrapper COM ownership (#38)`  
**Date:** 2026-09-16  
**Primary findings to re-audit:** `SDLX-006`, `SDLX-007`  
**Primary target scenario:** Capcom DX12 + REFramework + Streamline/DLSSG input -> OptiScaler XeFG output, especially Monster Hunter Wilds on Intel  

> The commit that adds this document is documentation-only. The code baseline for the audit is the pinned `036aae89...` revision unless the branch acquires later code changes. If the branch advances during the audit, the auditor must inspect every post-baseline code commit that touches any relevant path and explicitly state whether each finding still applies at the final audited tip.

---

## 1. Mission

Perform a **complete end-to-end D3D12 GPU-ordering audit** of the path by which Streamline/DLSSG resources become XeFG inputs and are later consumed by XeFG / presentation.

The audit must answer one central question with source and API-contract evidence:

> **For every XeFG input resource used for frame N, what establishes the GPU happens-before relationship between the commands that produce that resource and the queue/work that consumes it for XeFG frame generation/presentation?**

A second question must be answered with equal rigor:

> **When swapchain creation, `ResizeBuffers1`, recreation, resource submission, or vendor callbacks expose more than one D3D12 command queue, which queue does each layer actually own/use, and can those queue identities diverge across a swapchain generation?**

This is a **re-audit**, not confirmation of the earlier SDLX-006/007 wording. The prior audit classified both as credible source candidates, but the current code now includes PR36/PR37 CPU-side serialization and PR38 DX12 COM-lifetime hardening. Independently determine whether SDLX-006 and SDLX-007 remain defects, are safe by a proven D3D12/vendor contract, are merely fragile, or require runtime evidence.

Do **not** implement a fix during this audit. Do not add waits, fences, sleeps, queue substitutions, or logging behavior changes. Produce the report first.

---

## 2. Required final deliverable

Create a detailed report at:

```text
doc/work-order/RELEASE_0_9_SDLX_006_007_GPU_QUEUE_FENCE_SYNCHRONIZATION_AUDIT_REPORT_2026-09-16.md
```

The report must be committed to `reframework-0.9` only after the full checklist in this instruction is complete.

The auditor is **not done** merely after finding one suspicious missing `Wait()` or one pair of different queue pointers. Continue until every producer, submission, fence, queue-publication, XeFG-consumption, resize, and generation transition listed below has been traced to a terminal API boundary or explicitly marked **UNPROVEN** with the exact missing evidence.

If a local runtime artifact is unavailable, do **not** stop the source audit. Finish the complete source/contract audit, mark runtime-only questions as `NOT_EXERCISED`, and specify exactly what a future capture must record.

---

## 3. Background and why this needs a fresh audit

The previous Streamline/XeFG contract audit recorded `SDLX-006` and `SDLX-007` as follows:

- `SDLX-006`: CPU-side resource readiness did not establish an explicit GPU queue/fence dependency in the inspected path.
- `SDLX-007`: a queue received through `ResizeBuffers1` could update wrapper/global queue aliases without obviously updating XeFG's owned queue.

Since that audit:

- PR36 added a Streamline/XeFG CPU transaction around Streamline input callbacks using the FG owner-2 mutex.
- PR37 fixed `XeFG_Dx12::EvaluateState()` so it no longer releases that outer transaction unexpectedly.
- PR38 hardened the DX12 wrapper COM lifetime for queried command queues/devices and wrapped DXGI capability interfaces.

Those changes are important, but none automatically proves GPU execution ordering.

A CPU mutex can establish CPU callback order while still allowing this GPU topology:

```text
CPU order
--------
Streamline callback
  -> resource tagged / tracked
  -> CPU says resource is ready
  -> Present / XeFG path begins

GPU order if queues differ and no dependency exists
-----------------------------------------------
Queue A: [commands producing Depth/MV/HUD-less .........]

Queue B:                 [XeFG consumes those resources]

                 no proven A -> B GPU edge
```

The MHW E_ABORT evidence also recorded a real runtime topology in which an XeFG initialization queue and a presentation queue were **distinct command-queue identities on the same D3D12 device**. This alone does not prove a bug: same-device multi-queue operation is valid when the required ordering exists. The audit must determine the ordering contract, not merely detect multiple queues.

The failure is timing-sensitive and has historically been easier to reproduce with heavy OptiScaler file logging disabled. That makes an unproven GPU/CPU synchronization edge worth resolving carefully, but logging sensitivity is not proof of SDLX-006/007.

---

## 4. Hard audit rules

### 4.1 Do not stop at a local code smell

The following statements are **not acceptable final conclusions by themselves**:

```text
"SetResourceReady happens before ExecuteCommandLists, therefore bug."
"The queues are different, therefore bug."
"There is no ID3D12CommandQueue::Wait in this function, therefore bug."
"The DLSSG completion fence is ignored, therefore bug."
"The command list is passed to XeFG, therefore synchronization is safe."
"The Intel SDK probably synchronizes internally."
"DXGI guarantees this."
```

Each must be completed with the actual API contract and the complete call path.

### 4.2 CPU ordering is not GPU ordering

Explicitly distinguish:

- CPU mutual exclusion;
- CPU callback sequence;
- command-list recording order;
- command-list submission order;
- same-queue GPU execution order;
- cross-queue GPU dependency;
- GPU completion;
- resource lifetime/validity;
- vendor/API internal synchronization guaranteed by documented contract.

`ExecuteCommandLists()` returning means commands were submitted to a queue; it does **not** by itself mean their GPU work completed.

### 4.3 Never infer fence direction from a field name

The Streamline `DLSSGState` fields:

```text
inputsProcessingCompletionFence
lastPresentInputsProcessingCompletionFenceValue
```

must be audited from the exact Streamline header/documentation version used by this branch.

Determine precisely whether they mean, for example:

- game producer work is complete and DLSSG may consume inputs;
- DLSSG has finished consuming inputs and the game may reuse them;
- presentation processing completed;
- or another contract.

The direction matters. A fence that tells the application when **DLSSG is finished reading** an input is not automatically a producer-completion fence that XeFG should wait on before reading that input.

Do not recommend consuming this fence in XeFG until its direction and semantics are proven.

### 4.4 Do not assume the code's variable names describe vendor semantics

Names such as:

```text
_gameCommandQueue
currentCommandQueue
presentation queue
initialization queue
```

may describe the developer's interpretation rather than the API's formal role.

For every queue, record both:

1. the source variable/name; and
2. the role guaranteed by the API call that supplied it.

### 4.5 No implementation during audit

Do not:

- add `Signal` / `Wait` calls;
- force one global queue;
- replace XeFG's queue after resize;
- add `WaitForGPUIdle` calls;
- add `Flush` behavior;
- add sleeps/yields;
- use logging as a synchronization mechanism;
- alter resource validity;
- change PR36/37 mutex behavior;
- modify REFramework.

Minimal diagnostic instrumentation may be **designed in the report**, but do not implement it unless separately instructed after review.

---

## 5. Scope

### 5.1 Primary OptiScaler files / paths

At minimum inspect the current-tip versions of:

```text
OptiScaler/hooks/Streamline_Hooks.cpp
OptiScaler/hooks/Streamline_Hooks.h
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.h

OptiScaler/resource_tracking/ResTrack_dx12.cpp
OptiScaler/resource_tracking/ResTrack_dx12.h

OptiScaler/framegen/IFGFeature.cpp
OptiScaler/framegen/IFGFeature.h
OptiScaler/framegen/IFGFeature_Dx12.cpp
OptiScaler/framegen/IFGFeature_Dx12.h

OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.h
OptiScaler/proxies/XeFG_Proxy.*

OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/hooks/FG_Hooks.h
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/wrapped/wrapped_swapchain.h

OptiScaler/State.*
OptiScaler/Util.*
```

Also inspect every actual caller/callee discovered while tracing these paths. Do not restrict the audit to the file list above.

### 5.2 Companion REFramework source — read-only

If the matching `onehoon/REFramework` source is available, inspect it read-only to determine:

- what REF labels as XeFG initialization queue;
- what REF labels as presentation queue;
- how external binding queue identity is captured;
- whether the queue is a game queue, OptiScaler queue, Intel/vendor queue, or callback-supplied queue;
- whether REF introduces any `Signal`/`Wait` or queue handoff relevant to this path.

Do **not** modify REF during this audit.

### 5.3 Capcom DX12 focus

The target Capcom games in this investigation are DX12. Do not broaden the audit into D3D11 behavior.

Shared code may be inspected when it materially controls DX12 queue/fence behavior, but D3D11 fixes and tests are out of scope.

### 5.4 Explicitly out of scope unless directly required to prove 006/007

Do not turn this into a new general audit of:

- SDLX-002 / SDLX-003 map locking/lifetime;
- Reflex device caching;
- PR38 COM ownership already addressed in the DX12 wrapper;
- generic FSR-FG behavior;
- broad shutdown/lifecycle refactors;
- Intel/NVIDIA private implementation reverse engineering.

If one of those areas directly breaks the queue/fence proof, record the dependency narrowly and return to SDLX-006/007.

---

## 6. Current-source observations that must be independently verified

These observations are supplied to prevent the audit from wasting time rediscovering only the first layer. **Verify them against the actual audited tip; do not blindly copy them into the report.**

### 6.1 PR36/37 serialize Streamline CPU input callbacks with active XeFG Present

`Sl_Inputs_Dx12` currently uses `ScopedStreamlineFGTransaction` for XeFG when `FGUseMutexForSwapchain` is enabled. It serializes covered Streamline input paths against the owner-2 Present transaction.

This is a CPU-side ordering mechanism. Determine whether any relevant GPU-ready transition or queue publication occurs **outside** this transaction.

In particular, `ResTrack_Dx12::hkExecuteCommandLists()` is a separate later callback and does not obviously participate in the Streamline transaction.

### 6.2 Streamline resource tagging carries a D3D12 command-list identity into OptiScaler

`StreamlineHooks` forwards command buffers from:

```text
hkslSetTag
hkslSetTagForFrame
hkslEvaluateFeature
```

into:

```text
Sl_Inputs_Dx12::reportResource(..., ID3D12GraphicsCommandList* cmdBuffer, frameId)
```

`reportResource()` stores that pointer in `Dx12Resource::cmdList` for the relevant resource.

Trace the exact source of this command list and determine what its presence contractually means. Is it merely the command list associated with the tag call, or does the API guarantee that it contains / orders all producer work necessary for the resource?

### 6.3 XeFG `SetResource()` tags the resource before later queue-submission observation

For the relevant validity/resource cases, `XeFG_Dx12::SetResource()` builds `xefg_swapchain_d3d12_resource_data_t` and calls:

```text
XeFGProxy::D3D12TagFrameResource(..., fResource->cmdList, frameId, &resourceParam)
```

It can then call `SetResourceReady(type, fIndex)` for some paths.

Do not assume that `D3D12TagFrameResource` executes the command list, waits for it, or creates a queue dependency. Read the exact XeFG SDK contract for this call and for the command-list parameter.

### 6.4 Resource tracking marks readiness before calling the original `ExecuteCommandLists`

At the pinned baseline, `ResTrack_Dx12::hkExecuteCommandLists()` performs the following high-level sequence when a tracked command list is found:

```text
match tracked cmdList on queue `This`
    -> fg->SetResourceReady(type)
    -> erase tracking entry
    -> original ExecuteCommandLists(This, ...)
    -> fg->SetCommandQueue(type, This)
    -> return
```

Verify exact current-tip ordering and all early-return branches.

The audit must distinguish three events:

```text
CPU-ready flag set
queue submission performed
GPU producer work completed
```

They are not interchangeable.

Also determine whether `SetResourceReady()` / `IsResourceReady()` themselves have a proven CPU synchronization relationship with `XeFG_Dx12::Dispatch()`.

### 6.5 XeFG owns one committed command-queue reference, and resource type is currently ignored by queue commit

Current XeFG has an owned queue:

```text
Microsoft::WRL::ComPtr<ID3D12CommandQueue> _ownedGameCommandQueue
```

`CommitGameCommandQueue(queue)` stores it and updates `_gameCommandQueue`.

Current `SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue)` forwards directly to `CommitGameCommandQueue(queue)`; `type` is not used to maintain per-resource queue ownership.

Determine the consequence if Depth, Velocity, HUD-less, UI, or distortion command lists are submitted on different queues. Do not assume this happens in MHW; prove reachability from source/runtime if claiming it.

### 6.6 XeFG swapchain creation also commits the initialization queue

The XeFG create path resolves the command queue supplied to swapchain creation, passes that queue into the XeFG `D3D12InitFromSwapChainDesc` path, and commits it into XeFG's owned queue after successful creation.

Trace the exact queue supplied by:

```text
FGHooks::CreateSwapChain / CreateSwapChainForHwnd
    -> XeFG_Dx12::CreateSwapchain / CreateSwapchain1
    -> D3D12InitFromSwapChainDesc
    -> CommitGameCommandQueue
```

Determine whether this queue is later expected to remain stable for the lifetime of the XeFG swapchain or whether the API permits / requires replacement.

### 6.7 There are multiple queue caches/aliases — do not conflate them

At minimum distinguish:

```text
State::currentCommandQueue
State::currentD3D12Device

FGHooks file-static currentCommandQueue
FGHooks currentCommandQueueGeneration
FGHooks resizeFence / resizeFenceValue / resizeFenceEvent

IFGFeature_Dx12::_gameCommandQueue
XeFG_Dx12::_ownedGameCommandQueue

WrappedIDXGISwapChain4::_device
wrapped LocalPresent queueForUse / queriedQueue / realQueueOwner

ResizeBuffers1 ppPresentQueue[]

REFramework external-binding queue identity
REFramework/Intel presentation queue identity, if separately observable
```

The same pointer value may appear under several names. Different pointer values may also be wrappers/proxies for the same underlying queue. Canonicalize identity where possible.

### 6.8 Wrapped `ResizeBuffers1` publishes the first present-queue entry into global/wrapper aliases

At the pinned baseline, the wrapped `ResizeBuffers1` path has logic equivalent to:

```text
if (*ppPresentQueue != nullptr && XeFG active)
    State::currentCommandQueue = (ID3D12CommandQueue*)*ppPresentQueue;
    _device = State::currentCommandQueue;
```

It does not obviously call `XeFG_Dx12::CommitGameCommandQueue()` at that point.

This must be re-audited carefully. `IDXGISwapChain3::ResizeBuffers1` exposes `ppPresentQueue` as an array. Determine:

- required/optional pointer rules;
- array length rules;
- whether all entries must be the same queue;
- relationship with `pCreationNodeMask`;
- what `BufferCount == 0` means for the array contract;
- whether reading only the first entry is sufficient for the target topology;
- whether the wrapper path can execute with `ppPresentQueue == nullptr` and, if so, whether dereference is safe. The null-dereference question is secondary to SDLX-007 but must be recorded if source-confirmed.

### 6.9 FGHooks maintains a separate resize-sync queue/fence lifecycle

Current `FG_Hooks.cpp` contains its own queue/fence state, including concepts equivalent to:

```text
currentCommandQueue
currentCommandQueueGeneration
resizeFence
resizeFenceValue
resizeFenceEvent
PublishResizeSync(...)
ResetResizeSyncIfGeneration(...)
RetireQueueGeneration(...)
WaitForGPUIdle(...)
```

This is separate from the wrapped-swapchain `WaitForGPUIdle()` implementation.

Trace both helpers independently. Do not assume fixing or validating one proves the other.

Determine when the FGHooks queue/generation is published, when it is reset, and whether `ResizeBuffers1` uses a queue belonging to the same swapchain generation being resized.

### 6.10 The existing E_ABORT runtime evidence observed distinct same-device queue identities

The prior E_ABORT REF log recorded an XeFG initialization queue and presentation queue that were different pointers but belonged to the same D3D12 device.

That observation is a **topology fact**, not a root-cause result.

The new audit must determine whether the two labels represent:

- game producer queue vs vendor presentation queue;
- wrapper/proxy vs real queue identity;
- two true D3D12 queues;
- old vs new swapchain generation;
- or another relationship.

---

## 7. Mandatory end-to-end trace A — Streamline fence origin and semantics

Start at the public Streamline/DLSSG interface. Do not begin at XeFG.

### 7.1 Locate exact struct definitions and version handling

Find the exact definitions used by this branch for:

```text
sl::DLSSGState
inputsProcessingCompletionFence
lastPresentInputsProcessingCompletionFenceValue
```

Record:

- header path;
- struct version(s);
- SDK / Streamline version if identifiable;
- field type;
- ownership/lifetime rules for the fence pointer;
- exact semantic wording from the header/docs;
- which party signals it;
- which party is expected to wait;
- when the value changes;
- whether it relates to the current frame, last present, or another frame.

### 7.2 Trace `hkslDLSSGGetState()` completely

Resolve:

```text
caller
 -> hkslDLSSGGetState
 -> underlying/original DLSSG state query
 -> local/new state
 -> outward state copy
 -> every later OptiScaler read of the two fence fields
```

Search the **entire repository**, not only Streamline files, for the field names and any aliases.

If OptiScaler only forwards the values back to the application and never stores/consumes them, state that precisely.

Then answer the critical contract question:

> Would consuming this fence inside OptiScaler XeFG be semantically correct for producer -> XeFG ordering, or is it a DLSSG-consumer-completion fence whose direction is opposite?

If external documentation is needed, cite the exact public NVIDIA/Streamline source/version. If the contract cannot be proven, classify it `UNPROVEN`; do not infer from the name.

---

## 8. Mandatory end-to-end trace B — one resource from game production to XeFG consumption

Choose at least these resource types:

```text
Depth
Velocity / MotionVectors
HUDLessColor
```

Include UI/Distortion if their path differs materially.

For each type, follow **one full frame N** through every stage.

### 8.1 Streamline entry

Identify which entry can supply it in the target path:

```text
hkslSetTag
hkslSetTagForFrame
hkslEvaluateFeature
```

Record:

- frame token / frame ID;
- resource pointer;
- resource state;
- lifecycle (`eOnlyValidNow`, `eValidUntilPresent`, `eValidUntilEvaluate`, etc.);
- command-list pointer;
- thread if knowable;
- locks held.

### 8.2 `Sl_Inputs_Dx12::reportResource`

Record how the above becomes `Dx12Resource`:

```text
resource
cmdList
state
validity
frameIndex
width/height
resource type
```

Determine when the owner-2 transaction begins/ends relative to resource tagging.

### 8.3 `XeFG_Dx12::SetResource`

Trace:

- resource-map mutation;
- optional copy / flip / barrier work;
- command-list mutations;
- `D3D12TagFrameResource` timing;
- `SetResourceReady` timing;
- registration of command-list tracking in `ResTrack_Dx12`;
- which operations are merely recorded into a command list versus executed immediately.

If `SetResource()` calls or indirectly triggers a resource-tracking registration helper such as `SetResourceCmdList`, trace that exact chain and index selection.

### 8.4 Command-list close / submit path

Find when the command list is closed and how it reaches:

```text
ResTrack_Dx12::hkExecuteCommandLists(queue = This, ...)
```

Prove whether the exact command-list pointer tracked at tag time is the pointer observed at submission time, including wrapper/real-object replacement hooks.

Trace any command-list pointer rewriting logic.

### 8.5 Submission and readiness

For each resource type, record a sequence number like:

```text
R1 = reportResource entered
R2 = D3D12TagFrameResource called
R3 = SetResourceReady called from SetResource, if applicable
R4 = tracked command list matched in hkExecuteCommandLists
R5 = SetResourceReady called from hkExecuteCommandLists
R6 = original ExecuteCommandLists called on queue Qproducer
R7 = SetCommandQueue(type, Qproducer)
R8 = XeFG Dispatch reads IsResourceReady
R9 = XeFG/vendor work consumes resource
R10 = Present submits/executes
```

Do not collapse duplicate readiness paths. Determine which validity modes can mark readiness before queue submission and which wait for the Execute hook.

---

## 9. Mandatory end-to-end trace C — every queue identity

Produce a queue-identity table with at least these columns:

| Queue label | Source variable/API | How acquired | COM ownership | Device identity | Generation | Writers/replacements | Consumers | Can differ from producer? |
|---|---|---|---|---|---|---|---|---|

Populate every relevant queue, including:

```text
Qfactory/create: queue passed to FGHooks CreateSwapChain/CreateSwapChainForHwnd
Qxefg-init: queue passed to XeFG D3D12InitFromSwapChainDesc
Qxefg-owned: XeFG_Dx12::_ownedGameCommandQueue / _gameCommandQueue
Qresource(type): queue `This` observed by hkExecuteCommandLists for each tracked type
Qstate: State::currentCommandQueue
Qfg-resize: FGHooks file-static currentCommandQueue
Qresize[i]: each ResizeBuffers1 ppPresentQueue[i]
Qwrapped-device: WrappedIDXGISwapChain4::_device when it represents a queue
Qlocal-present: LocalPresent queueForUse / queried/real queue
Qref-binding: queue recorded by REFramework external XeFG binding
Qvendor-present: any distinct queue observed/exposed by XeFG/REF instrumentation
```

### 9.1 Canonicalize wrapper/proxy identities

For every pointer comparison, determine whether it is:

```text
same COM interface pointer
same COM identity via IUnknown
Streamline proxy vs underlying real queue
truly distinct ID3D12CommandQueue objects
```

When feasible, compare owning `ID3D12Device` identity too.

Do not call two queues "different" solely because wrapper and underlying pointers differ.

### 9.2 Determine queue type and node

For each actual D3D12 queue, retrieve/trace `D3D12_COMMAND_QUEUE_DESC` where possible:

```text
Type
Priority
Flags
NodeMask
```

The target path is expected to use appropriate D3D12 queues, but the audit must not assume this.

### 9.3 Determine whether resource types may be produced on different queues

Because XeFG stores one global owned game queue and `SetCommandQueue(type, queue)` does not preserve the type, answer:

1. Can Depth and Velocity be submitted on different queues?
2. Can HUDLess/UI be submitted on another queue?
3. If yes, which queue wins `_gameCommandQueue` and when?
4. Does XeFG actually use `_gameCommandQueue` to consume all those resources, or only for OptiScaler-owned helper command lists?
5. Is per-resource queue identity required by the XeFG API contract?

This is a mandatory question even if MHW is later shown to use one producer queue.

---

## 10. Mandatory end-to-end trace D — all fences, signals, waits, and completion mechanisms

Search the complete relevant codebase for:

```text
CreateFence
ID3D12Fence
Signal(
Wait(
SetEventOnCompletion
GetCompletedValue
WaitForSingleObject
ExecuteCommandLists
Flush / GPU idle helpers
D3D12TagFrameResource
```

Do not report only search hits. Classify each as:

```text
producer -> consumer synchronization
resize-only synchronization
CPU wait on GPU
GPU queue wait on another queue/fence
vendor-internal/unobservable
unrelated
```

Produce a fence table:

| Fence | Creator | Signal queue | Signal point/value | Waiter | Wait point/value | CPU or GPU wait | Generation/lifetime | Orders frame resources? |
|---|---|---|---|---|---|---|---|---|

### 10.1 Explicit cross-queue proof

If `Qproducer != Qconsumer`, identify the exact edge. A valid proof may be, depending on contract:

```text
Qproducer->Signal(F, N)
Qconsumer->Wait(F, N)
```

or a vendor API that explicitly guarantees an equivalent dependency from the submitted command list/queue.

If no such edge exists, state that. If the XeFG SDK guarantees an implicit edge, quote/cite the exact public contract and explain how OptiScaler satisfies its preconditions.

### 10.2 Same-queue proof

If the target resource producer and XeFG consumer always use the same queue, prove:

- queue identity cannot change between production and consumption;
- submission order on that queue is the required order;
- no vendor-internal queue consumes the resource before the supplied queue reaches the producer work;
- resize/recreation cannot silently replace the queue mid-frame.

"Same device" is not equivalent to "same queue".

---

## 11. Mandatory end-to-end trace E — what does XeFG actually consume, and on which queue?

Do not stop at `XeFG_Dx12::SetResource()`.

Trace:

```text
D3D12TagFrameResource
TagFrameConstants / SetPresentId as applicable
XeFG_Dx12::Present
XeFG_Dx12::Dispatch
OptiScaler helper command lists
_gameCommandQueue->ExecuteCommandLists, if used
XeFG swapchain Present / Present1 path
vendor callbacks back into DXGI / ResizeBuffers1
```

Answer separately:

1. Which queue executes OptiScaler-owned UI/SC helper command lists?
2. Which queue is supplied to XeFG initialization?
3. Does XeFG expose/document the queue on which its internal frame-generation work executes?
4. Does `D3D12TagFrameResource` accept a command list specifically so XeFG can establish correct ordering, or for resource-state/barrier bookkeeping only?
5. Does XeFG require the application to ensure the resource is ready before the tag call, before Present, or before another API boundary?
6. Can a command list be tagged before it has been submitted? Is that explicitly supported?
7. If `cmdList == nullptr` or the code substitutes a sentinel `(ID3D12GraphicsCommandList*)1` for older SDK behavior, what synchronization guarantee remains?
8. How do `XEFG_SWAPCHAIN_RV_ONLY_NOW` versus `XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT` change the requirement?

Use the exact XeFG SDK headers/docs corresponding to the binary/header version in this repository. If the vendor contract is silent, mark the result `UNPROVEN` rather than assuming safety or unsafety.

---

## 12. Mandatory end-to-end trace F — `ResizeBuffers1` and queue-generation transitions (SDLX-007)

Treat resize as a complete generation transition, not a single function call.

### 12.1 Trace all `ResizeBuffers1` layers in actual call order

The codebase has more than one relevant layer, including:

```text
WrappedIDXGISwapChain4::ResizeBuffers1
FGHooks::hkResizeBuffers1
underlying real IDXGISwapChain3::ResizeBuffers1
XeFG/vendor internal ResizeBuffers1 callbacks
```

Prove the actual call order for:

- game-originated ResizeBuffers1;
- XeFG/vendor-originated nested ResizeBuffers1;
- wrapper/proxy call;
- skip-flag branches (`_skipResize1`, `_skipResize`, etc.).

Do not assume both wrapper and FG hooks execute in every path.

### 12.2 Audit `ppPresentQueue` as an array

For each resize path record:

```text
BufferCount
pCreationNodeMask[i]
ppPresentQueue[i]
canonical queue identity
queue device identity
```

Determine whether the first entry alone is sufficient and whether queue entries can change from the previous generation.

### 12.3 Track queue publication to every owner/cache

For a successful resize, show what happens to:

```text
WrappedIDXGISwapChain4::_device
State::currentCommandQueue
FGHooks::currentCommandQueue
FGHooks::currentCommandQueueGeneration
XeFG_Dx12::_ownedGameCommandQueue
IFGFeature_Dx12::_gameCommandQueue
REFramework external binding generation/queue
```

For each, mark:

```text
updated
unchanged intentionally
updated later by resource submission
retired/reset
unknown
```

### 12.4 Generation correctness

Trace:

```text
nextFGSwapchainGeneration
currentFGSwapchainGeneration
BindFGGeneration
PublishResizeSync
RetireQueueGeneration
ResetResizeSyncIfGeneration
wrapper final release
new swapchain creation/reuse
```

Answer:

1. Can a queue from generation G remain stored while resources/present belong to G+1?
2. Can `WaitForGPUIdle()` signal a queue that no longer owns the backbuffers being resized?
3. Can a resource submission after resize overwrite XeFG's queue with an old-generation queue?
4. Can an old command list be recognized after frame/generation rollover and republish its queue?
5. Does preserved-swapchain mode change these answers?

---

## 13. Mandatory hypothesis matrix

Resolve **every** hypothesis below. Add more if discovered.

Use one of:

```text
CONFIRMED DEFECT
SAFE BY PROVEN INVARIANT
SAFE BY DOCUMENTED API CONTRACT
SAFE BUT FRAGILE
RUNTIME-DEPENDENT CANDIDATE
INSUFFICIENT EVIDENCE
NOT REACHABLE IN TARGET MHW PATH
```

### H1 — CPU-ready-before-submit permits XeFG consumption before producer submission

`SetResourceReady` can become true before original `ExecuteCommandLists()` is called.

Determine whether XeFG Dispatch can observe that ready bit concurrently and proceed before the producer command list is even submitted.

### H2 — CPU-ready-after-submit still does not imply GPU completion

Even if the ordering is changed conceptually to after `ExecuteCommandLists`, determine whether a different consumer queue would still need a GPU dependency.

### H3 — the Streamline DLSSG completion fence is the missing producer dependency

Do not accept this hypothesis without proving fence direction/semantics. It may be the opposite dependency.

### H4 — passing the producer command list into `D3D12TagFrameResource` is sufficient

Prove or disprove from XeFG's documented API contract.

### H5 — all MHW resources are produced and consumed on one queue, so cross-queue waits are unnecessary

Prove actual queue identity for Depth, Velocity, HUD-less and the XeFG consumer path. Do not infer from device identity.

### H6 — resource types can arrive on multiple queues but global `_gameCommandQueue` safely represents them

Explain how a single queue is correct if producer queues differ, or classify as a defect/candidate.

### H7 — `ResizeBuffers1` changes present queue but XeFG's owned queue remains old

Prove actual transition and consequence. Do not classify solely from the wrapper assignment; follow later resource submissions and XeFG queue usage.

### H8 — the queue difference in the E_ABORT REF log is wrapper/proxy identity only

Canonicalize COM identity and device/queue objects where available.

### H9 — FGHooks resize fence establishes the needed frame-resource ordering

Determine exactly what that fence orders. A resize-idle fence is not automatically a per-frame producer -> XeFG dependency.

### H10 — vendor Present path internally waits for game work

Accept only with an explicit XeFG/Streamline/DXGI contract or runtime evidence strong enough to support the claim.

### H11 — `eOnlyValidNow` / command-list-associated resources have a stricter queue requirement than `eValidUntilPresent`

Resolve from both Streamline lifecycle semantics and XeFG validity semantics.

### H12 — a queue-generation change can occur without a resource/frame-generation reset

Trace resize/recreate flags and generation state to prove whether old/new resource and queue state can mix.

---

## 14. Required timing / ownership diagrams

The final report must contain at least these diagrams, using actual function names and queue labels from the audited source.

### 14.1 Normal frame, one producer queue

Example shape:

```text
Game/Streamline CPU
  reportResource(frame N, cmdList C)
    -> XeFG SetResource
      -> TagFrameResource(C)

Game submission CPU
  hkExecuteCommandLists(QA, C)
    -> readiness event(s)
    -> original ExecuteCommandLists(QA, C)
    -> queue publication

GPU QA
  ... producer work for N ...

Present CPU
  FGHooks::FGPresent
    -> XeFG::Present/Dispatch

XeFG GPU consumer
  [show exact queue / proven API dependency]
```

### 14.2 Distinct producer and consumer queues

Show the required/observed fence edge or explicitly show that none is proven.

### 14.3 Two producer queues for different resource types

For example:

```text
Depth -> QA
Velocity -> QB
HUDLess -> QA or QC
XeFG owned queue -> Q?
```

Show how the single `_gameCommandQueue` evolves.

### 14.4 Resize queue replacement

Show generation G -> G+1, every queue cache update, fence retirement, and first post-resize resource submission.

### 14.5 Candidate bad interleaving

Only if source permits it, show the shortest concrete interleaving that violates an API ordering contract. Every step must be reachable; do not invent vendor behavior.

---

## 15. Repository-wide search requirements

At minimum perform searches equivalent to:

```text
rg -n "inputsProcessingCompletionFence|lastPresentInputsProcessingCompletionFenceValue"
rg -n "ExecuteCommandLists|SetResourceReady|IsResourceReady|SetCommandQueue|CommitGameCommandQueue"
rg -n "CreateFence|SetEventOnCompletion|GetCompletedValue|WaitForSingleObject"
rg -n "\.Signal\(|->Signal\(|\.Wait\(|->Wait\("
rg -n "D3D12TagFrameResource|TagFrameConstants|SetPresentId"
rg -n "currentCommandQueue|_gameCommandQueue|_ownedGameCommandQueue|ppPresentQueue"
rg -n "ResizeBuffers1|PublishResizeSync|RetireQueueGeneration|ResetResizeSyncIfGeneration"
rg -n "currentFGSwapchainGeneration|nextFGSwapchainGeneration|BindFGGeneration"
```

Search vendor/SDK headers checked into or consumed by the repository for the exact definitions/contracts as well.

Do not stop because a search returns no direct call. Follow typedefs/function pointers/proxy wrappers and dynamically loaded functions until the terminal API is identified.

---

## 16. API-contract verification requirements

The report must separate **source observation** from **external API contract**.

Verify, where relevant:

### D3D12

- ordering of command lists submitted to one command queue;
- semantics of `ID3D12CommandQueue::Signal`;
- semantics of `ID3D12CommandQueue::Wait`;
- whether CPU return from `ExecuteCommandLists` implies only submission or completion;
- multi-node / `ResizeBuffers1` present-queue rules.

### DXGI

- `IDXGISwapChain3::ResizeBuffers1` rules for `pCreationNodeMask` and `ppPresentQueue`;
- array length and nullability;
- D3D12 present-queue expectations.

### Streamline / DLSSG

- resource tag command-buffer meaning;
- resource lifecycle meaning;
- `DLSSGState` completion-fence meaning and direction;
- frame-token association.

### Intel XeFG

- queue passed to `D3D12InitFromSwapChainDesc`;
- command-list argument to `D3D12TagFrameResource`;
- resource validity requirements;
- required resource readiness / synchronization responsibility;
- any documented blocking/synchronization mode;
- whether the SDK internally creates/uses another queue and what ordering the application must establish.

If the exact version-specific contract cannot be found, state that explicitly. Do not silently substitute a newer SDK contract unless compatibility is proven.

---

## 17. Runtime evidence — use if available, but source audit must finish without it

Potential existing evidence locations include:

```text
C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004
C:\GoogleDrive\ref-xefg\Release-09\mhw
C:\GoogleDrive\ref-xefg\Release-09\mhw\Intel
C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더 (3)
```

If available, preserve session boundaries. Do not combine logs from different builds into one timeline.

Record for each runtime artifact:

```text
OptiScaler commit/build identity
REFramework commit/build identity
GPU + driver
logging on/off
queue pointers
queue device identities
swapchain generation
resize events
Present sequence
D3D return/error
```

Historical logs from older builds may establish topology but **cannot validate the current pinned commit**.

### 17.1 Existing log labels must not be overinterpreted

If REF logs say:

```text
initialization queue
presentation queue
external binding queue
```

trace the instrumentation source that assigned those labels before using them as architectural facts.

---

## 18. If source alone cannot close the proof: required diagnostic design

If the final source/API audit is still `INSUFFICIENT EVIDENCE`, design a **no-behavior-change diagnostic patch** as the next step. Do not implement it during this audit.

The diagnostic proposal must log/correlate at least:

```text
monotonic CPU event sequence number
thread ID
frame ID / Opti frame count / ring index
resource type
resource pointer
command-list pointer
resource validity/state
producer queue pointer
canonical real queue identity if wrapped
queue device identity
queue desc Type/NodeMask
XeFG owned queue identity at the same moment
State::currentCommandQueue
FGHooks currentCommandQueue + generation
ResizeBuffers1 ppPresentQueue[i]
current FG swapchain generation
DLSSG completion fence pointer/value, if contractually relevant
TagFrameResource call/result
SetResourceReady transition
original ExecuteCommandLists entry/return
SetCommandQueue transition
XeFG Present/Dispatch entry
D3D/XeFG error result
```

The instrumentation must be designed so a single frame can be reconstructed without relying on synchronous file logging delays. Prefer compact event IDs / buffered logging if later implemented.

Do not use the diagnostic logger itself as a synchronization fix.

---

## 19. Required report structure

The final report must use this structure or an equivalently complete one.

### 19.1 Executive verdict

State separately:

```text
SDLX-006 verdict
SDLX-007 verdict
MHW E_ABORT causal confidence
whether a code change is justified before new runtime A/B
```

Do not claim E_ABORT root cause without direct evidence.

### 19.2 Revision matrix

Record:

- pinned base;
- final audited OptiScaler tip;
- every post-pinned relevant commit;
- REFramework revision(s) inspected;
- vendor header/API versions used for contract conclusions;
- runtime build revisions.

### 19.3 Queue identity matrix

Required table from section 9.

### 19.4 Fence/dependency matrix

Required table from section 10.

### 19.5 Resource-path matrix

At minimum Depth / Velocity / HUD-less from Streamline tag to GPU consumer.

### 19.6 Resize/generation matrix

Before/after queue identity and all cache owners for generation G -> G+1.

### 19.7 Findings

Assign new subfinding IDs if necessary, for example:

```text
SDLX-006-A
SDLX-006-B
SDLX-007-A
```

Each finding must contain:

```text
Status
Severity / impact
Confidence
Exact source path + symbol
Preconditions
Reachable sequence
Violated or unproven contract
Consequence
Current MHW reachability
Runtime evidence
Counter-evidence / safety invariant
Recommended next step
```

### 19.8 What was disproven

Explicitly list attractive hypotheses that did **not** survive the audit. This is required so later work does not repeatedly revisit them.

### 19.9 Minimal next experiment

If a defect remains only a runtime candidate, define the smallest diagnostic/A-B experiment that can decide it.

If the source proves a defect independent of MHW, describe the smallest correction boundary, but do not implement it.

---

## 20. Severity and evidence language

Use these evidence labels consistently:

```text
SOURCE-CONFIRMED
API-CONTRACT-CONFIRMED
RUNTIME-OBSERVED
RUNTIME-CANDIDATE
THEORETICAL
NOT_EXERCISED
INSUFFICIENT EVIDENCE
NOT REACHABLE
```

A source-confirmed missing explicit `Wait()` is **not automatically a defect** if the vendor contract proves an implicit dependency.

A source-confirmed queue difference is **not automatically a defect** if the correct cross-queue synchronization is proven.

Conversely, a successful resize/present result does not prove synchronization correctness if the race is timing-dependent.

---

## 21. Completion gate — the auditor must answer every item

The audit is incomplete until the report answers all of the following.

### Streamline contract

- [ ] What exactly do the DLSSG completion fence/value mean?
- [ ] Who signals it and who waits?
- [ ] Is it relevant to game-producer -> XeFG-consumer ordering?
- [ ] What does the command buffer passed with a resource tag guarantee?

### Resource production

- [ ] For Depth, Velocity, HUD-less: which command list carries production/transition work?
- [ ] How is that command list registered for tracking?
- [ ] On which queue is it submitted?
- [ ] When does OptiScaler mark the resource ready relative to submission?

### XeFG consumption

- [ ] What does `D3D12TagFrameResource` guarantee about the supplied command list?
- [ ] What queue does XeFG consume/execute on, as far as public contract permits proving?
- [ ] Does OptiScaler provide the required same-queue or cross-queue GPU ordering?
- [ ] What happens when the command-list pointer is null/sentinel?

### Queue ownership

- [ ] What queue initializes the XeFG swapchain?
- [ ] What queue is in `_ownedGameCommandQueue` at frame N?
- [ ] Can each resource type overwrite it?
- [ ] Are producer queues allowed to differ?
- [ ] Are wrapper/proxy pointer differences canonicalized correctly?

### Resize / generation

- [ ] What are all `ppPresentQueue[i]` values and what do they mean?
- [ ] Which queue caches update on successful ResizeBuffers1?
- [ ] Does XeFG's owned queue update immediately, later, or never?
- [ ] Can an old-generation queue be used after G -> G+1?
- [ ] What do FGHooks resize fences actually order?

### MHW relevance

- [ ] Is the previously observed `distinct_same_device` topology real distinct queues or proxy identity?
- [ ] Does the target MHW path satisfy the preconditions for any source-confirmed defect?
- [ ] Is the defect capable of producing a timing-sensitive GPU/D3D failure?
- [ ] Is there evidence tying it specifically to E_ABORT 4004?
- [ ] If not, what exact new evidence would decide it?

### Final decision

- [ ] SDLX-006 independently reclassified.
- [ ] SDLX-007 independently reclassified.
- [ ] No fix implemented during audit.
- [ ] Recommended next action is either `NO CHANGE`, `DIAGNOSTIC ONLY`, or a narrowly defined future PR boundary.

---

## 22. Required stopping behavior

Do **not** stop early for any of the following reasons:

- one runtime log is missing;
- the E_ABORT session has no OptiScaler log;
- Intel internal queue behavior is not fully visible;
- the first source search shows no explicit `Wait()`;
- one queue pair appears identical;
- one queue pair appears different;
- the previous audit already called SDLX-006/007 blocking;
- the suspected completion fence turns out to have the wrong semantic direction.

Instead:

1. complete all source paths that can be proven;
2. complete all public API-contract paths that can be proven;
3. identify the exact opaque vendor boundary;
4. mark only that boundary `UNPROVEN`;
5. finish the remaining audit;
6. design the smallest diagnostic needed to close the remaining gap.

The goal is a closed technical argument, not a list of suspicious functions.

---

## 23. Expected quality bar

A satisfactory audit should allow a reviewer to take a single failing frame and answer, without reopening the whole repository:

```text
Which resource was tagged?
Which command list produced it?
Which queue received that command list?
When was it marked CPU-ready?
What proves the producer GPU work became visible to XeFG?
Which queue did XeFG/presentation use?
Did resize/recreation change any of those identities?
Which fence/value, if any, bridges the queues?
Which part is guaranteed by D3D12, Streamline, DXGI, or XeFG?
Which part remains only an assumption?
```

If those questions cannot be answered from the report, the audit is not complete.
