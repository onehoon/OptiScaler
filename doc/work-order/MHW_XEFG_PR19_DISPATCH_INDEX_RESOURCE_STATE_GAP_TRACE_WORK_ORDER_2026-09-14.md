# MHW XeFG PR19 Dispatch/Resource-State Gap Trace Expansion Work Order

Date: 2026-09-14  
Repository: `onehoon/OptiScaler`  
Implementation target: existing Draft PR #19 only  
PR branch: `diag/mhw-xefg-auto-ring-trace`  
PR head reviewed: `34be5b4d38b8be9881952586477f08a903ef91f6`  
Fork `master` reviewed: `12a1f8271c343801b89cc63eef020630b8d9b793`

## Purpose

Extend the existing temporary MHW XeFG diagnostic PR #19 with one more low-overhead ring-trace commit that closes the remaining gap inside `XeFG_Dx12::Dispatch()` and makes the frame producer/consumer progression directly observable.

This is **not a new PR**. Implement this as an additional commit on PR #19 (`diag/mhw-xefg-auto-ring-trace`). Keep PR #19 open and Draft. Do not merge PR #19 as part of this task.

The target reproduction is:

```text
Monster Hunter Wilds
NVIDIA RTX GPU
OptiScaler XeFG output
fork OptiScaler PR19
fork REFramework
Fatal D3D error (7, E_ABORT, 0x80004004)
```

The current trace already narrowed the final interesting interval to:

```text
XeFGPresentBeforeDispatch
-> DispatchEnter
-> DispatchAfterIndexResolve
-> [missing boundary information]
-> DispatchBeforeHudlessLookup
```

In the captured crash run, the final failing thread reached `DispatchAfterIndexResolve` but did not reach `DispatchBeforeHudlessLookup`. The same run also showed a frame-progression discontinuity around logical frames 5911/5912/5913 that cannot yet be classified from the current event payloads alone.

The next capture must answer two concrete questions:

1. Which exact gate or state read after `GetDispatchIndex()` is the last completed operation?
2. Did `GetDispatchIndex()` intentionally skip a frame because of frame-ahead policy, or did producer/consumer state become inconsistent before the crash?

Do not attempt a functional fix until those two questions are answered by the trace.

---

## Current code findings

### 1. PR19 already traces the outer Dispatch boundaries

Current `XeFG_Dx12::Dispatch()` records:

```text
DispatchEnter
DispatchAfterIndexResolve
DispatchBeforeHudlessLookup
DispatchAfterHudlessLookup
DispatchBeforeHudlessSetResource
```

and the downstream `SetResource()` boundaries.

The remaining unobserved block after `DispatchAfterIndexResolve` contains all of the following:

```cpp
if (fIndex < 0)
    return false;

if (!IsActive() || IsPaused())
    return false;

if (!_resourceReady[fIndex].contains(Depth) ||
    !_resourceReady[fIndex].at(Depth) ||
    !_resourceReady[fIndex].contains(Velocity) ||
    !_resourceReady[fIndex].at(Velocity))
    return false;

// UI-composition state / IsUsingHudless(fIndex)
// interpolation-count state
// WAR_xefgRequestFGToggle handling
// _haveHudless / IsUsingHudless(fIndex)
// _noHudless[fIndex]
```

Any crash, early-return, or state discontinuity inside this block currently has the same visible last event: `DispatchAfterIndexResolve`.

### 2. `GetDispatchIndex()` mutates the consumer cursor before later Dispatch validation

The current shared FG base implementation in `IFGFeature::GetDispatchIndex()` does this:

```cpp
willDispatchFrame = _lastDispatchedFrame + 1;
...
if (frame-ahead policy selects the latest resource-bearing frame)
    willDispatchFrame = _frameCount;

_lastDispatchedFrame = willDispatchFrame;
_lastFGFrame = State::Instance().fgLastFrame;
return willDispatchFrame % BUFFER_COUNT;
```

This is important for diagnosis. `_lastDispatchedFrame` is advanced before `XeFG_Dx12::Dispatch()` checks active/paused state and Depth/Velocity readiness.

This does **not** by itself prove a bug. A frame skip can be intentional when `FGAllowedFrameAhead` policy chooses `_frameCount`. The trace must therefore record the pre/post index-resolution state and the branch reason before any behavior is changed.

### 3. `StartNewFrame()` resets per-slot state

`IFGFeature::StartNewFrame()` increments `_frameCount`, chooses `fIndex = _frameCount % BUFFER_COUNT`, then clears/resets per-slot state including:

```text
_resourceReady[fIndex]
_waitingExecute[fIndex]
_noUi[fIndex]
_noDistortionField[fIndex]
_noHudless[fIndex]
```

`SetResourceReady()` subsequently marks a resource ready and records `_resourceFrame[type] = _frameCount`.

Therefore the next trace needs enough producer-side metadata to distinguish:

```text
intentional frame-ahead skip
```

from:

```text
consumer points at slot N while resource state in slot N belongs to another logical frame
```

without changing the underlying lifecycle.

---

## Required invariants

These are merge-blocking for this diagnostic commit.

### D1 — diagnostics only

Do not change:

- `GetDispatchIndex()` selection policy,
- `_lastDispatchedFrame` mutation timing,
- `FGAllowedFrameAhead`,
- resource readiness semantics,
- `IsActive()` / `IsPaused()` behavior,
- Hudless/UI behavior,
- XeFG SDK calls,
- swapchain lifecycle,
- mutex ownership,
- Present/Present1 routing,
- resize behavior,
- retries/timeouts/sleeps/yields.

### D2 — keep PR19 as the only implementation PR

- Add commits to `diag/mhw-xefg-auto-ring-trace`.
- Do not create a new diagnostic PR.
- Do not modify PR #18.
- Do not retarget PR #19.
- Do not merge PR #19.

### D3 — preserve the ring format

Current trace format is version 1 with 80-byte records and the current maximum event ID is 109.

- Append new event IDs after the current maximum.
- Never renumber existing event IDs.
- Do not change `TraceRecord` layout.
- Do not change `kTraceVersion` for this task.
- If PR19 head has moved by implementation time, first re-read the enum and append after the then-current maximum.

### D4 — no extra per-frame text logging

Use `XeFGTrace::Record()` only for the new high-frequency boundaries. Do not add `LOG_DEBUG`, `LOG_INFO`, or `LOG_XEFG_DIAG` per frame for this investigation.

### D5 — do not call state helpers extra times merely for tracing

Where an existing path already calls `IsUsingHudless(fIndex)` or another helper, capture and trace the value from that existing evaluation when practical. Do not introduce an additional call if the helper could acquire state or later gain side effects.

---

## Required trace expansion

Exact enum names may be adjusted for local naming consistency, but the following information must become observable.

### A. Index-resolution state

Add a marker immediately **before** `GetDispatchIndex()` containing at minimum:

```text
_frameCount
_lastDispatchedFrame
FGAllowedFrameAhead effective value
```

Add or extend a marker immediately **after** `GetDispatchIndex()` containing:

```text
willDispatchFrame
fIndex
_lastDispatchedFrame after the call
_frameCount
```

The decoder must make it possible to classify each call as one of:

```text
same-frame / no dispatch
next-frame sequential dispatch
latest-frame jump due to frame-ahead policy
initial-dispatch path
```

Do not infer the branch only from modulo slot numbers.

Recommended appended event names:

```text
DispatchIndexResolveBegin
DispatchIndexResolveState
```

If a compact branch-reason enum can be computed without changing control flow, store it in `aux0`/`aux1` of the post event.

### B. Eligibility gate

Immediately after the `fIndex < 0` check, make the active/pause decision visible.

Required values:

```text
fIndex
IsActive result
IsPaused result
_targetFrame if readily available without adding new behavior
_frameCount
```

If the function returns because it is inactive or paused, emit one explicit diagnostic event before returning.

Recommended events:

```text
DispatchEligibilitySnapshot
DispatchEarlyReturn
```

Use a small reason enum for `DispatchEarlyReturn`, for example:

```text
1 = invalid_index
2 = inactive
3 = paused
4 = depth_missing
5 = depth_not_ready
6 = velocity_missing
7 = velocity_not_ready
8 = hudless_state_transition
```

Do not encode strings in the ring.

### C. Resource-ready gate

Preserve current short-circuit semantics while making each failure class distinguishable.

The trace must distinguish:

```text
Depth key absent
Depth key present but false
Velocity key absent
Velocity key present but false
all required resources ready
```

Prefer sequential local boolean capture that preserves the current check order. Do not use `operator[]` in diagnostics because it can mutate the map.

Emit one compact snapshot/pass/fail event carrying the four booleans.

Recommended event:

```text
DispatchResourceReadySnapshot
```

### D. UI/Hudless state gates

Add boundaries around the state resolution that currently sits between resource readiness and `DispatchBeforeHudlessLookup`.

The trace must identify whether execution reached and returned from:

```text
UI composition change check
IsUsingHudless(fIndex)
WAR_xefgRequestFGToggle handling
_haveHudless initialization/comparison
_noHudless[fIndex] read
```

Keep this compact. The goal is not to mirror every source line.

Recommended events:

```text
DispatchBeforeHudlessStateResolve
DispatchHudlessStateSnapshot
DispatchBeforeNoHudlessRead
DispatchAfterNoHudlessRead
```

`DispatchHudlessStateSnapshot` should expose at minimum:

```text
fIndex
_haveHudless.has_value()
_haveHudless value when present
current IsUsingHudless(fIndex) value when already evaluated by the normal path
_uiComposition
WAR_xefgRequestFGToggle
```

### E. Producer/consumer generation correlation

Add one low-cost producer-side ring event at the narrowest existing point where resource readiness for a logical frame becomes authoritative.

The preferred source-level location is `IFGFeature::SetResourceReady()` or an XeFG-only caller immediately adjacent to it, depending on which option keeps the diagnostic scoped to XeFG without changing generic FG behavior.

For each relevant resource-ready transition, record:

```text
logical _frameCount
slot/index
resource type
_resourceFrame[type] after assignment
```

At minimum trace Depth and Velocity. If Hudless/UI are already cheap to include using the same event, include the resource type rather than adding separate event IDs.

Recommended event:

```text
FrameResourceReadyGeneration
```

If implementing this directly in generic `IFGFeature` would impose PR19 diagnostics on unrelated FG outputs, instead emit the correlation from the existing XeFG resource submission path. Scope is more important than the suggested location.

---

## Event-budget guidance

Keep the addition small. Target approximately 6-9 new event types total.

Do not instrument every map lookup or every assignment. One event should carry multiple booleans/IDs using existing record fields.

A good final sequence should look conceptually like:

```text
DispatchEnter
DispatchIndexResolveBegin
DispatchAfterIndexResolve / DispatchIndexResolveState
DispatchEligibilitySnapshot
DispatchResourceReadySnapshot
DispatchBeforeHudlessStateResolve
DispatchHudlessStateSnapshot
DispatchBeforeNoHudlessRead
DispatchAfterNoHudlessRead
DispatchBeforeHudlessLookup
...
```

A failure or early return must leave an unambiguous last completed boundary.

---

## Files expected in scope

Primary:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/diagnostics/XeFGTrace.h
tools/decode_xefg_trace.py
```

Potentially, only if needed for producer-generation correlation:

```text
OptiScaler/framegen/IFGFeature.cpp
OptiScaler/framegen/IFGFeature.h
```

Do not broaden into unrelated FG hooks or lifecycle files.

---

## Decoder requirements

Update `tools/decode_xefg_trace.py` in the same PR19 commit series.

Required:

1. Add names for all appended event IDs.
2. Preserve decoding of every existing event ID.
3. Extend `--self-test` with at least:
   - one sequential dispatch,
   - one frame-ahead jump,
   - one resource-ready failure,
   - one Hudless-state boundary.
4. Do not change existing CSV/TSV columns unless absolutely necessary. Prefer decoding new `aux0`/`aux1` semantics in text through event-specific interpretation only if useful.
5. `python -m py_compile tools/decode_xefg_trace.py` must pass.

---

## Runtime acceptance matrix

Primary validation:

```text
MHW + NVIDIA RTX + XeFG + fork REFramework
PR19 diagnostic branch
OptiScaler_XeFGTrace.bin enabled automatically
```

Capture at least one long-running session or one E_ABORT reproduction.

The resulting trace must answer, without source-code guessing:

```text
What were _frameCount and _lastDispatchedFrame before index resolution?
Why was willDispatchFrame selected?
Was a frame intentionally skipped by frame-ahead policy?
Did active/pause gating pass?
Were Depth and Velocity present and ready in the selected slot?
Did Hudless state resolution complete?
Did _noHudless[fIndex] read complete?
Which logical frame produced the resources stored in the selected slot?
```

Control smoke test:

```text
one non-Capcom XeFG title or another known-good XeFG path
```

The trace additions must not change observed FG behavior.

---

## Static/build validation

Required before pushing the implementation commit to PR #19:

```text
git diff --check
python tools/decode_xefg_trace.py --self-test
python -m py_compile tools/decode_xefg_trace.py
Release x64 OptiScaler build
```

Also audit that:

- no existing event ID changed,
- `TraceRecord` size remains 80,
- `kTraceVersion` remains 1,
- no new sleep/yield/retry/timeout exists,
- no new functional branch changes XeFG behavior,
- no new per-frame text logger call was added.

---

## Stop condition

After one authoritative failing capture, stop adding broad diagnostics if the last completed event identifies a single gate or data access.

If the trace proves that `GetDispatchIndex()` advances `_lastDispatchedFrame` into a frame that is subsequently rejected by the resource/eligibility gates, treat that as a **separate production-fix decision**. Do not silently change the cursor semantics inside this diagnostic commit.

Likewise, if the trace proves a producer/consumer generation mismatch, prepare a separate minimal fix after the evidence is reviewed.

The purpose of this PR19 extension is to make the next failure decisive, not to speculate a fix into the diagnostic branch.
