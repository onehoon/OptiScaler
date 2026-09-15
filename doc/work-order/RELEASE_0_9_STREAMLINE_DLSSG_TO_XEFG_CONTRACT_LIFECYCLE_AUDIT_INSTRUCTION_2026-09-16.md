# Release 0.9 — Streamline DLSSG → XeFG Contract / Concurrency / Lifecycle Audit Instruction

**Status:** Independent source-audit instruction  
**Target branch to audit:** `reframework-0.9`  
**Pinned audit base:** `c8f1fd0d831514c55fedb57bec24ec791640a674`  
**Code base represented by that HEAD:** PR35 code at `d6b0132f4f6faabf7235dba5b8d684edfd8e2831` plus documentation-only commit(s)  
**Date:** 2026-09-16  

---

## 1. Mission

Perform an **independent, source-level audit** of the complete OptiScaler path that translates **Streamline/DLSSG frame-generation input data into XeFG output and presentation**.

The audit is not limited to one reported crash or one suspected mutex bug. The objective is to determine whether the current adapter correctly preserves all required contracts across:

```text
Streamline / DLSSG input callbacks
    constants / resources / frame IDs / validity
                |
                v
OptiScaler FG frame-state translation
    frame indexing / resource readiness / metadata / ownership
    CPU synchronization / command lists / queue ownership
                |
                v
XeFG submission
    D3D12TagFrameResource
    TagFrameConstants
    SetPresentId
                |
                v
XeFG / DXGI Present
                |
                v
resize / deactivate-reactivate / swapchain recreation / release / shutdown
```

The auditor must treat the observations in this document as **known examples and starting points, not predetermined conclusions**. Independently verify or disprove every suspected problem against the pinned source tree.

Do not begin by implementing a fix. First produce a complete audit report with evidence, reachable interleavings, ownership/ordering diagrams, and a remediation plan ranked by confidence and risk.

---

## 2. Why this audit is required now

A current Intel Monster Hunter Wilds test using the latest `reframework-0.9` build failed during normal gameplay with:

```text
Fatal D3D error (7, E_ABORT, 0x80004004)
```

Test material:

```text
C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004
```

The important reproduction property is unusual and highly relevant to concurrency analysis:

> The E_ABORT 4004 failure is observed especially when OptiScaler file logging is disabled. Enabling the heavy OptiScaler trace/file logger materially reduces or hides the failure.

The failing run is **not a process-shutdown failure**. Gameplay and XeFG presentation continue for minutes before the fatal D3D path is entered.

From the available REFramework log / Capcom crash data:

- the active XeFG binding remains on the same generation before failure;
- no REF detach/rebind transaction immediately precedes the failure;
- observed resize operations return successfully;
- presentation stops before the later REF hook-monitor timeout/quarantine;
- therefore the later REF timeout is an effect, not evidence that the REF monitor initiated the D3D failure;
- the Capcom minidump is generated through the game's fatal-D3D path and does **not** by itself prove an AV inside `igxess_fg.dll` or the Intel graphics driver;
- both a known-good Intel logging-enabled run and the failing logging-disabled run use a game/init command queue and an Intel presentation queue that are distinct but belong to the same device; therefore "the queues are different" is not sufficient as a root-cause claim.

This combination strongly justifies an audit for a timing-sensitive ordering, resource-lifetime, CPU race, GPU dependency, or lifecycle crossover defect.

Do **not** assume that the current leading hypothesis is correct. The audit must establish the actual invariant or missing invariant.

---

## 3. Scope

### 3.1 Primary scope

Audit the full Streamline/DLSSG → XeFG path, including at minimum:

- Streamline hook/export/callback entry points that eventually invoke `Sl_Inputs_Dx12`;
- `OptiScaler/inputs/FG/Streamline_Inputs_Dx12.*`;
- `OptiScaler/framegen/IFGFeature.*`;
- `OptiScaler/framegen/IFGFeature_Dx12.*`;
- `OptiScaler/framegen/xefg/XeFG_Dx12.*`;
- `OptiScaler/hooks/FG_Hooks.*`;
- any resource-tracking, wrapped-swapchain, queue, command-list, fence, or proxy helper that materially participates in the above path;
- resize, FG enable/disable, swapchain recreation, release, and process-shutdown intersections with active Streamline callbacks / XeFG presentation.

### 3.2 The auditor must discover actual callers

Do not stop at `Streamline_Inputs_Dx12.cpp`.

Trace the actual code paths that call, directly or indirectly:

```text
Sl_Inputs_Dx12::setConstants
Sl_Inputs_Dx12::reportResource
Sl_Inputs_Dx12::evaluateState
Sl_Inputs_Dx12::markPresent
Sl_Inputs_Dx12::CheckForFrame

IFGFeature::StartNewFrame
IFGFeature::SetFrameCount
IFGFeature::GetIndex
IFGFeature::GetIndexWillBeDispatched
IFGFeature::GetDispatchIndex

IFGFeature_Dx12::NewFrame
IFGFeature_Dx12::HasResource
IFGFeature_Dx12::GetResource

XeFG_Dx12::SetResource
XeFG_Dx12::Present
XeFG_Dx12::Dispatch
XeFG_Dx12::Activate
XeFG_Dx12::Deactivate
XeFG_Dx12::DestroyFGContext
XeFG_Dx12::ReleaseSwapchain

FGHooks::FGPresent
FGHooks resize handlers
FGHooks release / swapchain-retirement handlers
```

For each entry point, establish:

- caller function(s);
- expected OS thread(s), if known;
- whether the thread assumption is guaranteed by an API contract or merely observed in logs;
- locks already held when entering;
- callbacks that can re-enter OptiScaler while locks are held;
- whether frame N and frame N+1 can overlap on different threads.

If a thread-affinity or callback-order guarantee cannot be proven from code or an external API contract, record it as **unproven**, not as an implicit invariant.

### 3.3 Non-goals

The initial audit is not permission to:

- reverse engineer proprietary Intel/NVIDIA implementation internals unless needed to interpret observed public API behavior;
- add game-name special cases;
- add Intel/NVIDIA DLL-name special cases;
- add arbitrary sleeps or yields;
- turn synchronous file logging into a synchronization mechanism;
- replace the current architecture with one global coarse lock before proving why it is necessary;
- revert PR34 or PR35;
- mix unrelated FSR-FG refactors into the audit;
- claim a race only because code "looks suspicious" without demonstrating a reachable concurrent path or violated ordering contract.

---

## 4. Known observations — examples to verify, not conclusions

The following observations were found during the initial investigation. The auditor must explicitly resolve each as one of:

```text
CONFIRMED DEFECT
SAFE BY PROVEN INVARIANT
SAFE BUT FRAGILE / MAINTAINABILITY ISSUE
INSUFFICIENT EVIDENCE — specify what evidence is required
```

### 4.1 `XeFG_Dx12::SetResource()` is already per-frame resource locked

A simplistic hypothesis that `SetResource()` has no resource mutex is incorrect in the current source.

Current code obtains:

```cpp
auto fIndex = inputResource->frameIndex;
if (fIndex < 0)
    fIndex = GetIndex();

std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);
```

While that lock is held, `SetResource()` may:

- mutate `_frameResources[fIndex]`;
- mutate copied-resource bookkeeping;
- create/copy/flip resources;
- record resource barriers or commands;
- call `XeFGProxy::D3D12TagFrameResource()`;
- call `SetResourceReady(type, fIndex)`.

Therefore do not propose "add a mutex around SetResource" as an audit result unless a more specific uncovered state or API transaction justifies it.

The real question is whether the **scope and lock domain are sufficient for the complete frame transaction**.

### 4.2 `GetResource()` returns an internal map element after releasing its lock

Current pattern in `IFGFeature_Dx12::GetResource()`:

```cpp
std::shared_lock<std::shared_mutex> lock(_resourceMutex[index]);

if (!_frameResources[index].contains(type))
    return nullptr;

if (auto it = _frameResources[index].find(type); it != _frameResources[index].end())
    return &it->second;
```

The shared lock is destroyed at function return, but callers receive a raw pointer to an element inside `_frameResources[index]`.

`IFGFeature_Dx12::NewFrame()` later performs, under exclusive lock:

```cpp
_frameResources[fIndex].clear();
```

Audit questions:

- Can `NewFrame()` reach the same `fIndex` while a `GetResource()` caller is still dereferencing the returned pointer?
- Does some outer lock or frame-index invariant prove this impossible?
- Can `unordered_map` mutation/clear/rehash invalidate the returned pointer during a reachable overlap?
- Are all callers protected by an outer `fg->Mutex`, or are any not?

Do not mark this as a UAF solely from the local code shape. Prove or disprove a reachable invalidation path.

### 4.3 `HasResource()` currently reads the resource map without `_resourceMutex`

Current implementation performs a direct `contains()` on `_frameResources[index]` without taking the per-frame shared mutex.

At the same time:

- `SetResource()` mutates the map under exclusive lock;
- `NewFrame()` clears the map under exclusive lock;
- Streamline resource callbacks call `HasResource()` while deciding which frame index should receive depth, motion vectors, UI, or hudless resources.

Audit whether the current caller relationships provide an outer serialization guarantee. If not, establish the precise read/write interleaving and consequence.

### 4.4 `Dispatch()` consumes state protected by several different mechanisms — and some state with no obvious local lock

`XeFG_Dx12::Dispatch()` uses or modifies, among other things:

- `_frameCount`;
- `_lastDispatchedFrame`;
- `_resourceReady[fIndex]`;
- `_frameResources[fIndex]`;
- `_noHudless[fIndex]`;
- `_noDistortionField[fIndex]`;
- camera position/orientation values;
- near/far/FOV/aspect values;
- jitter;
- motion-vector scale;
- reset state;
- frame-time delta;
- interpolation state;
- XeFG enable/toggle state.

It eventually calls public XeFG APIs including:

```text
D3D12TagFrameResource
TagFrameConstants
SetPresentId
```

Some underlying resource-map operations use `_resourceMutex[fIndex]`, but these other frame fields are not all visibly governed by the same mutex.

Audit the effective synchronization supplied by outer callers and callback sequencing.

### 4.5 Streamline frame-boundary locking has a narrow scope

`Sl_Inputs_Dx12::CheckForFrame()` uses `_frameBoundaryMutex`, but `reportResource()` continues substantial work after `CheckForFrame()` returns and that helper lock has been released.

The remaining work includes:

- looking up `_frameIdIndex[]`;
- selecting an OptiScaler frame index;
- calling `HasResource()`;
- updating interpolation metadata;
- calling `SetResource()` and XeFG resource tagging.

`setConstants()` similarly performs additional FG state updates after frame-boundary handling.

`markPresent()` takes `_frameBoundaryMutex` and then performs:

```cpp
_isFrameFinished = true;
_lastFrameId = static_cast<uint32_t>(frameId);

if (State::Instance().currentFG != nullptr)
    State::Instance().currentFG->SetFrameCount(frameId);
```

Audit whether `_frameBoundaryMutex` protects only the Streamline tracker state or whether code incorrectly relies on it as a complete FG-frame transaction boundary.

### 4.6 The Present path has a different outer mutex domain

`FGHooks::FGPresent()` conditionally takes the public FG `OwnedMutex` with logical owner `2` before `fg->Present()`.

The lock remains held through the actual DXGI Present/Present1 call and is released afterward.

Other known logical owners include:

```text
owner 1    — XeFG swapchain release path
owner 2    — FG present path
owner 6678 — resize path
```

The Streamline input path is not obviously part of owner-2 Present serialization.

Audit all uses of `OwnedMutex` and produce a complete lock-order graph.

### 4.7 `OwnedMutex` is non-recursive and owner-aware only for the specified logical owner ID

Current `OwnedMutex` uses a `std::shared_mutex` plus atomic logical-owner and owning-thread fields.

It has:

```cpp
bool isOwnedByCurrentThread(uint32_t _owner) const;
```

This answers whether the current thread owns the mutex under a **specific** logical owner ID. It does not itself answer whether the current OS thread already owns the same underlying non-recursive mutex under a different owner ID.

Audit any path where nested callbacks can attempt to acquire the same mutex under different owner IDs on the same thread.

Do not assume reentrancy is impossible. Prove callback boundaries.

### 4.8 Logging is a major timing perturbation

Current logger defaults/configuration include:

```text
LogLevel = trace
LogAsync = false
```

File logging uses an `spdlog` multi-threaded file sink and the logger calls:

```cpp
shared_logger->flush_on(spdlog::level::trace);
```

A logging-enabled build therefore introduces substantial synchronous:

- sink locking;
- formatting;
- file I/O;
- flushes;
- cross-thread scheduling perturbation.

Because the observed MHW 4004 is strongly sensitive to logging being on/off, **logging-enabled stability is not evidence of synchronization correctness**.

### 4.9 Intel uses distinct same-device queues in both known-good and failing observations

The initial comparison indicates that both the logging-enabled good run and logging-disabled failing run use distinct game/init and Intel presentation queues on the same device.

Therefore the audit must not stop at "multiple queues exist".

Determine whether the resources and command lists consumed by XeFG have the required **GPU happens-before relationship** before the XeFG/presentation queue consumes them.

CPU mutexes do not automatically establish D3D12 cross-queue GPU completion.

---

## 5. Required audit dimensions

### 5.1 Frame identity and frame-order audit

Construct an exact model for the relationship among:

```text
Streamline frameId
_currentFrameId
_lastFrameId
_currentIndex
_frameIdIndex[]

IFGFeature::_frameCount
IFGFeature::_lastDispatchedFrame
GetIndex()
GetIndexWillBeDispatched()
GetDispatchIndex()

XeFG D3D12TagFrameResource(frameId)
XeFG TagFrameConstants(frameId)
XeFG SetPresentId(frameId)
DXGI Present / Present1
```

Answer at minimum:

1. Can resource tags for frame N and constants for frame N+1 be paired with one XeFG present ID?
2. Can `SetFrameCount(frameId)` move the logical frame while a prior frame is inside `Present()` / `Dispatch()`?
3. Can `StartNewFrame()` clear/reuse a ring-buffer slot that the current Present transaction still references?
4. What happens when Streamline reports frameId `0` or reports a frame ID not found in `_frameIdIndex[]`?
5. Are fallback decisions such as `GetIndexWillBeDispatched()` versus `GetIndex()` stable under concurrent callbacks?
6. Does `BUFFER_COUNT` reuse create an ABA-like problem where the same index now refers to a newer logical frame?
7. Is `_resourceFrame` sufficient to detect stale resources, and is it actually used as a correctness barrier?

Produce at least two timing diagrams:

- normal frame N sequence;
- worst-case overlapping frame N / N+1 callback sequence.

### 5.2 Shared-state ownership audit

Inventory every mutable field involved in this adapter.

For each field, record:

| State | Writers | Readers | Nominal owner | Lock/atomic | Can overlap? | Invariant |
|---|---|---|---|---|---|---|

The table must cover at least:

```text
Streamline tracker fields
_frameCount / _lastDispatchedFrame / _targetFrame
_resourceReady[] / _resourceFrame
_noUi[] / _noHudless[] / _noDistortionField[]
_frameResources[] / _resourceCopy[]
camera/jitter/MV/reset/frame-time arrays
interpolation dimensions/positions
_fgFramePresentId / _lastFGFramePresentId
_isActive / _waitingNewFrameData
FGchanged / SCchanged and relevant State globals
XeFG context / swapchain pointers relevant to runtime callbacks
command-list reset flags
queue pointers and generation ownership
```

If a field is intentionally single-thread-only, identify the code or API contract that guarantees that.

### 5.3 Lock audit and lock-order graph

Inventory at minimum:

```text
Sl_Inputs_Dx12::_frameBoundaryMutex
IFGFeature_Dx12::_resourceMutex[index]
IFGFeature::Mutex (OwnedMutex)
XeFG_Dx12::_swapchainLifecycleMutex
wrapped-swapchain local mutexes if reachable
other queue / resource tracking locks encountered in the call graph
```

For every function in scope, list:

- locks acquired;
- acquisition order;
- locks expected to already be held by caller;
- whether acquisition is blocking, try-lock, shared, or exclusive;
- whether external/vendor code or callbacks are called while the lock is held.

Build a directed lock-order graph.

Explicitly search for:

- A→B on one path and B→A on another;
- same-thread self-deadlock through different logical OwnedMutex owner IDs;
- callbacks into OptiScaler while a non-recursive lock is held;
- locks held across XeFG/Streamline/DXGI external calls;
- long critical sections that serialize the application render thread unnecessarily.

### 5.4 Resource lifetime and COM ownership audit

For every `Dx12Resource` stored in `_frameResources[]`, determine:

- who owns the underlying `ID3D12Resource*`;
- whether OptiScaler AddRefs it or merely borrows it;
- validity promised by Streamline (`OnlyValidNow`, `UntilPresent`, etc.);
- whether copies are made when needed;
- when map entries are cleared;
- when `_resourceCopy` entries are released/replaced;
- whether raw pointers escape the lock scope;
- whether a resource can be released/recycled by the producer before XeFG consumes it.

Resolve the `GetResource()` raw-pointer example explicitly.

Also audit command-list pointers carried inside `Dx12Resource`:

- ownership;
- reset/reuse timing;
- whether XeFG is allowed to retain/use them beyond the API call;
- whether `ValidNow` / `UntilPresent` conversions are semantically correct.

### 5.5 XeFG public API transaction audit

Document the exact intended per-frame XeFG transaction used by OptiScaler.

At minimum trace:

```text
D3D12TagFrameResource(frame N, ...)
    depth
    motion vectors
    hudless/UI as applicable

TagFrameConstants(frame N, ...)
SetPresentId(frame N)
Present / Present1
```

Determine from source and public XeFG API contract:

- which operations are thread-safe;
- which operations require ordering even if individually thread-safe;
- whether frame N+1 tags may overlap frame N Present;
- whether `TagFrameConstants` and `SetPresentId` must happen on the same thread;
- whether all resource tags for a frame must complete before `SetPresentId` / Present;
- whether OptiScaler currently provides that happens-before guarantee;
- whether enable/disable/configuration calls may overlap frame submission.

Distinguish:

```text
API is thread-safe
```

from:

```text
the application has established the correct logical frame order
```

They are not equivalent.

### 5.6 Command-list and GPU queue ordering audit

This is mandatory and must not be replaced by CPU mutex analysis.

Identify:

- queue on which Streamline-provided command lists/resources are produced;
- `_gameCommandQueue` ownership and publication;
- internal UI/SC command-list submission queue;
- queue used by Intel/native XeFG presentation path if observable through public interfaces;
- all fences/signals/waits that establish cross-queue ordering;
- assumptions that rely on DXGI/XeFG implicitly synchronizing resources.

For each resource type, answer:

> By what concrete D3D12 synchronization mechanism is the producer's GPU work guaranteed complete/visible before the XeFG consumer uses this resource?

If the answer is "same queue," prove it for the relevant path.

If the queues differ, identify the fence/wait/API guarantee.

A CPU mutex, atomics, or logger serialization is not a GPU dependency.

### 5.7 Streamline callback-order audit

Determine the real callback order and permissible variations for:

```text
setConstants
reportResource(depth)
reportResource(motion vectors)
reportResource(hudless/UI)
evaluateState
markPresent
```

Audit:

- whether these callbacks can occur on multiple threads;
- whether callbacks for adjacent frames can overlap;
- whether resource callbacks may arrive after constants or after `markPresent`;
- how frameId `0`, repeats, skips, and jumps are handled;
- whether `CheckForFrame()` transitions are idempotent and race-safe;
- whether `_isFrameFinished` can be observed inconsistently;
- whether `_frameIdIndex[]` can be read while another thread rewrites it.

Trace the actual Streamline hook/export code to establish these facts. Do not infer them only from the adapter class.

### 5.8 Present transaction audit

`FGHooks::FGPresent()` currently uses the FG `OwnedMutex` and holds it through OptiScaler FG work and actual DXGI present.

Audit:

- why this lock scope is required;
- which Streamline frame-input operations, if any, must participate in the same transaction;
- whether holding it through the external DXGI call can trigger reentrant callbacks;
- whether the lock can block a resource callback needed for Present completion;
- whether Present observes an immutable snapshot of frame N or live mutable state.

The preferred architecture should make the frame handed to Present **logically immutable** once presentation begins, rather than relying on timing.

### 5.9 Lifecycle crossover audit

For each of these transitions:

```text
normal dispatch/present
FG deactivate
FG reactivate
resize target
ResizeBuffers / ResizeBuffers1
runtime swapchain recreation
final proxy release
process shutdown
```

answer:

- can a Streamline callback be active concurrently?
- can the current XeFG context disappear or be disabled underneath a callback?
- can frame resources be cleared while a callback is tagging them?
- can `SetResource()` call XeFG while a lifecycle transaction is retiring the context?
- which lock/state flag prevents that?
- what happens to a callback already in flight when `FGchanged` / `SCchanged` is set?

Preserve the current proven shutdown fixes described below.

---

## 6. Existing lifecycle fixes that must not regress

This audit follows two important fixes that are already validated separately.

### 6.1 PR34 — wrapped final-release reentrancy

PR34 fixed a real deadlock in the wrapped XeFG final COM release path by introducing terminal final-release ownership and moving external FG retirement outside the wrapper's non-recursive local mutex.

Do not move XeFG/REF retirement back under the wrapper local mutex.

Do not remove the terminal-owner / reentrant-zero-release protection merely to simplify another lock design.

### 6.2 PR35 — process-shutdown REF pre-retire skip

After PR34, NVIDIA MHW process shutdown could complete Opti/REF semantic retirement but then crash in the generic `libxess_fg.dll` path.

PR35 now skips the REF pre-retire handoff only during confirmed process shutdown and leaves live-runtime lifecycle ordering intact.

A subsequent NVIDIA MHW test:

```text
C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더 (3)
```

shut down normally without generating a dump.

Observed successful shutdown included:

```text
shutting_down = true
ref_pre_retire_skipped, reason = process_shutdown
QueueLifecycle retired
clear_generation
final_release_complete
```

The broad Streamline/XeFG audit must not casually alter this behavior.

If a finding requires touching release/shutdown code, explain exactly why and include dedicated PR34/PR35 regression coverage.

---

## 7. Required audit procedure

The auditor should follow this sequence rather than jumping directly to code changes.

### Phase A — Freeze the source baseline

1. Confirm the target commit is the pinned audit base or document the newer SHA being audited.
2. If target branch moved, compare the new HEAD to the pinned base and list any changes that affect the audit scope.
3. Never mix findings from different revisions without identifying the exact SHA.

### Phase B — Build the complete call graph

Trace from actual Streamline-facing entry points through `Sl_Inputs_Dx12`, the FG abstraction, XeFG submission, and DXGI Present.

Include alternate / exceptional paths:

- FG disabled;
- paused;
- reset-history;
- missing resource;
- frame jump;
- UI/hudless enabled or absent;
- resize in progress;
- runtime FG toggle;
- recreation;
- shutdown.

### Phase C — Inventory shared state and ownership

Produce the shared-state table described above.

Every mutable variable that crosses two entry points must have an identified owner/invariant.

### Phase D — Build CPU happens-before model

For each relevant write and read, identify the concrete synchronization edge:

```text
mutex
shared_mutex
atomic release/acquire
same-thread sequenced-before
external API callback guarantee
none
```

Do not write "probably same thread."

### Phase E — Build GPU happens-before model

Trace command-list recording, ExecuteCommandLists, fences, queue signals/waits, and resource states.

Separate CPU publication from GPU completion.

### Phase F — Audit frame identity

Prove that all XeFG data for frame N is attributed to frame N, including when adjacent callbacks overlap.

### Phase G — Audit resource lifetime

Resolve all borrowed/raw pointers and map-element escapes.

### Phase H — Audit lifecycle crossovers

Test the model against resize, FG toggle, recreation, release, and shutdown.

### Phase I — Classify findings

For every candidate, either prove safety or document the defect with a reachable sequence.

Do not leave the known examples unresolved.

---

## 8. Evidence standard for every finding

Every reported finding must contain all of the following.

### 8.1 Finding header

```text
ID: SDLX-001
Severity: Critical / High / Medium / Low
Confidence: High / Medium / Low
Category: CPU race / lifetime / frame identity / GPU ordering / deadlock / lifecycle / other
```

Severity and confidence are separate dimensions.

### 8.2 Exact source evidence

Include:

- commit SHA;
- file path;
- function(s);
- relevant code excerpt or line references;
- exact shared object/state involved.

### 8.3 Reader/writer or producer/consumer pair

Example format:

```text
Writer:
  function A
  thread/context
  locks held

Reader:
  function B
  thread/context
  locks held
```

For GPU findings, identify queue and command-list relationships instead.

### 8.4 Reachable bad interleaving

Show a concrete sequence, for example:

```text
T1: obtain pointer to frameResources[index][Depth]
T1: GetResource returns and releases shared lock
T2: StartNewFrame selects same ring slot
T2: unique-lock resourceMutex[index]
T2: frameResources[index].clear()
T2: unlock
T1: dereference escaped pointer
```

Then prove whether the surrounding architecture actually permits T1/T2 overlap.

Without the reachability proof, classify it as a candidate, not a confirmed bug.

### 8.5 Consequence

State the concrete plausible effect:

- wrong-frame XeFG tagging;
- stale pointer/UAF;
- corrupted resource metadata;
- invalid command-list lifetime;
- deadlock;
- missing GPU dependency;
- E_ABORT/device removal;
- visual corruption only;
- harmless due to invariant.

Do not claim that any individual finding caused MHW 4004 unless the evidence supports causality.

### 8.6 Proposed correction

For confirmed findings, describe the smallest robust architectural correction.

Do not implement it in the audit report unless separately requested.

---

## 9. Severity guidance

Use the following as guidance, not automatic scoring.

### Critical

Examples:

- reachable UAF / memory corruption;
- deterministic deadlock in a normal lifecycle path;
- context destruction concurrent with vendor API use;
- invalid COM lifetime that can execute freed memory.

### High

Examples:

- violated XeFG per-frame ordering contract on a reachable normal path;
- resource from frame N can be submitted as N+1;
- missing cross-queue synchronization that can cause D3D failure;
- frame-state mutation concurrent with dispatch with credible 4004/device-loss consequences.

### Medium

Examples:

- timing-sensitive race requiring a rarer resize/toggle condition;
- stale diagnostic/read state that can cause unnecessary FG reset or transient disable;
- lock inversion only reachable through an uncommon callback sequence.

### Low

Examples:

- undocumented but currently safe invariant;
- diagnostic weakness;
- maintainability issue with no currently reachable incorrect behavior.

---

## 10. Instrumentation policy

Because logging changes the observed failure rate, normal synchronous OptiScaler file logging is a poor instrument for this audit.

If runtime evidence is required, prefer minimally perturbing instrumentation such as:

- atomically incremented counters;
- fixed-size in-memory ring buffers;
- timestamp/thread/frameId tuples written to preallocated memory;
- ETW or equivalent low-overhead event recording if already available;
- post-crash memory dump of diagnostic state.

Do not:

- add `Sleep()` / `SwitchToThread()` as a diagnostic fix;
- add synchronous flushes to make the crash disappear;
- conclude that a patch is correct only because trace logging is enabled.

Any diagnostic code should live on a dedicated audit/diagnostic branch and remain separate from a production fix unless explicitly reviewed.

Recommended minimal event record fields:

```text
sequence number
QPC timestamp
OS thread ID
logical event type
Streamline frameId
Opti _frameCount
ring-buffer index
_lastDispatchedFrame
resource type
resource pointer
command-list pointer
queue pointer if relevant
XeFG context pointer
generation
```

A monotonically increasing sequence number is more useful than relying only on timestamps.

---

## 11. Required test matrix

The source audit is primary, but findings should be validated against realistic runtime scenarios when possible.

### 11.1 Intel MHW — primary reproduction

Configuration:

```text
DLSSG / Streamline input
XeFG output
OptiScaler file logging OFF
```

Required focus:

- normal gameplay for longer than the historically observed failure window;
- frame N/N+1 overlap evidence;
- no `E_ABORT 0x80004004`;
- no hidden dependency on file logging.

Logging ON may be used **only as a timing-control comparison**, not as acceptance evidence.

### 11.2 Intel runtime transitions

Exercise:

- alt-tab / focus transitions;
- borderless/fullscreen-related transitions if supported;
- resolution / resize;
- FG enable → disable → enable;
- pause/reactivation paths;
- any repeatable swapchain recreation path.

### 11.3 NVIDIA MHW regression

At minimum verify clean process exit after any eventual implementation that touches shared locks/lifecycle.

PR35 success criteria must remain intact:

- no shutdown deadlock;
- no `libxess_fg.dll` post-retire AV;
- final wrapper retirement completes.

### 11.4 Other adapters / GPUs where useful

If available, compare:

- non-Intel generic XeFG backend;
- AMD GPU using XeFG;
- another Streamline/DLSSG game;
- another OptiScaler FG input path feeding XeFG.

The purpose is to distinguish:

```text
MHW-specific callback pattern
Streamline-input-wide defect
XeFG-output-wide defect
Intel-native-backend timing sensitivity
```

Do not introduce per-game fixes merely because one game reproduces first.

---

## 12. Required deliverables

The audit is not complete until the following are committed to the repository.

### 12.1 Main audit report

Create a detailed report under a path such as:

```text
doc/audit/RELEASE_0_9_STREAMLINE_DLSSG_TO_XEFG_CONTRACT_LIFECYCLE_AUDIT_REPORT_2026-09-XX.md
```

The report must include:

- audited commit SHA;
- executive summary;
- complete call graph;
- shared-state ownership table;
- per-function lock table;
- lock-order graph;
- frame N transaction timeline;
- frame N/N+1 overlap timeline;
- CPU happens-before analysis;
- GPU queue/dependency analysis;
- resource-lifetime analysis;
- lifecycle state-transition analysis;
- findings table;
- remediation plan.

### 12.2 Findings table

Example:

| ID | Severity | Confidence | Area | Status | Summary | Recommended action |
|---|---|---|---|---|---|---|
| SDLX-001 | High | High | frame ordering | confirmed | ... | ... |
| SDLX-002 | Medium | Medium | lifetime | candidate | ... | ... |

### 12.3 Resolution of every known example in this instruction

The report must explicitly resolve:

- `SetResource()` per-frame mutex scope;
- `GetResource()` escaped raw map-element pointer;
- unlocked `HasResource()`;
- direct `Dispatch()` access to shared frame state;
- narrow `_frameBoundaryMutex` scope;
- separation between Streamline callbacks and Present `OwnedMutex` domain;
- same-thread/different-owner `OwnedMutex` reentrancy risk;
- logging-induced serialization;
- CPU-vs-GPU ordering across distinct queues.

### 12.4 Remediation plan

Do not simply say "add locks."

For each confirmed issue, specify:

- desired invariant;
- minimal code ownership boundary;
- lock/snapshot/fence mechanism proposed;
- lock ordering;
- expected performance cost;
- deadlock risk;
- regression tests;
- whether it should be its own PR.

Prefer multiple small, reviewable fixes over one broad synchronization rewrite.

---

## 13. Questions the auditor must answer explicitly

The final report must answer every question below.

1. **Can a pointer returned by `GetResource()` be invalidated while a caller is still using it?** If no, prove the outer invariant.
2. **Can `HasResource()` race with `SetResource()` or `NewFrame()` on the same index?**
3. **Can `SetFrameCount()` or `StartNewFrame()` run while `XeFG_Dx12::Dispatch()` is consuming the previous logical frame?**
4. **Can a frame N+1 Streamline callback mutate shared constants/metadata while frame N is inside the Present transaction?**
5. **Can the ring-buffer index be reused for a new logical frame while old references to that index remain live?**
6. **Are `D3D12TagFrameResource`, `TagFrameConstants`, and `SetPresentId` always applied to the same logical frame before its Present?**
7. **What exact ordering guarantee exists between resource tagging and Present?**
8. **What exact D3D12 synchronization guarantees producer GPU work is complete before XeFG consumes each resource?**
9. **When game/init and XeFG presentation queues differ, where is the fence/signal/wait or documented API guarantee?**
10. **Does Streamline itself guarantee callback thread affinity/order that makes any apparent race safe?** Cite the contract/code proving it.
11. **Can resize, deactivate, recreation, or release begin while a Streamline resource/constants callback is in flight?**
12. **Can a Streamline callback invoke XeFG using a context that is concurrently being disabled or retired?**
13. **Can `OwnedMutex` self-deadlock when the same OS thread re-enters under a different logical owner ID?**
14. **Are any vendor/DXGI calls made while holding locks that can be reacquired by callbacks from those external calls?**
15. **Is correctness currently dependent on accidental synchronization from synchronous logging?**
16. **If logging OFF changes only timing, which missing explicit invariant would make ON/OFF behavior equivalent?**
17. **What is the narrowest production architecture that makes a submitted frame immutable/complete before Present without serializing unrelated work?**

---

## 14. Preferred invariants to evaluate — not mandatory implementation instructions

The auditor should assess whether the architecture would be safer if it explicitly guaranteed concepts like the following:

### 14.1 Frame publication invariant

> A logical frame is mutable while Streamline is collecting inputs, then atomically/synchronously becomes ready for XeFG consumption. After publication, Present consumes an immutable snapshot or otherwise protected frame transaction.

### 14.2 Resource lifetime invariant

> A resource entry cannot be cleared, replaced, or invalidated while any consumer retains a reference/pointer to it.

### 14.3 XeFG submission invariant

> All resource tags and constants for frame N complete before `SetPresentId(N)` and the Present that consumes N; frame N+1 cannot mutate N's consumed state.

### 14.4 Lock-order invariant

If multiple CPU locks remain necessary, define exactly one global order and enforce it everywhere.

A possible order must be derived from the actual call graph; do **not** copy an example mechanically.

### 14.5 GPU dependency invariant

> CPU publication of a resource is distinct from GPU completion. If producer and consumer queues differ, a real GPU synchronization edge or documented API guarantee must exist.

These are evaluation criteria, not authorization to redesign the system without evidence.

---

## 15. Prohibited audit shortcuts

Do not submit a report that relies on any of the following shortcuts:

```text
"It is probably all the same render thread."
"XeFG is thread safe, so ordering is safe."
"The logger fixes it, so keep logging enabled."
"Use one global mutex around everything."
"Sleep 1 ms before Present."
"Intel driver bug" without evidence.
"MHW bug" without evidence.
"Different queues are the bug" without showing the missing dependency.
"GetResource looks unsafe" without a reachable invalidation sequence.
```

Likewise, do not downgrade a proven race merely because it is difficult to reproduce with trace logging enabled.

---

## 16. Audit completion criteria

The audit may be considered complete only when all of the following are true:

- actual Streamline-facing caller paths have been traced rather than assumed;
- every shared mutable state item in the adapter has an identified ownership/synchronization rule;
- lock acquisition order is documented across Streamline, resource, Present, resize, and lifecycle paths;
- frame N and overlapping N/N+1 timelines are documented;
- XeFG resource/constants/present-ID ordering is proven or a defect is identified;
- CPU synchronization is separated from GPU queue synchronization;
- borrowed resource/command-list lifetimes are documented;
- all known examples in Section 4 are resolved;
- resize/recreation/shutdown crossovers are evaluated;
- PR34/PR35 invariants are preserved in the proposed remediation plan;
- every confirmed issue contains a reachable bad interleaving or concrete violated contract;
- every proposed production fix has regression coverage and expected lock/performance impact;
- final acceptance does not depend on synchronous file logging being enabled.

---

## 17. Handoff priority

Recommended audit order:

```text
P0  Trace actual Streamline callback entry points and thread/order guarantees
P0  Frame identity / frame N vs N+1 transaction model
P0  CPU lock coverage and resource lifetime
P0  GPU queue/fence dependency model
P1  XeFG API transaction ordering
P1  Present / resize / lifecycle crossover
P1  OwnedMutex reentrancy and lock inversion
P2  Performance / contention review of proposed corrections
P2  Diagnostics and maintainability improvements
```

Do not implement the earlier narrow MHW synchronization hypothesis as the final architecture until this audit establishes which invariant is actually missing.

The purpose of this audit is to make the Streamline DLSSG → XeFG adapter correct **by explicit contract**, not merely stable under one game's current timing.