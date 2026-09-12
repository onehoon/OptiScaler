# Work Order: MHW XeFG Logging Heisenbug — Make FGHooks Reentrancy Guards Thread-Local

**Repository:** `onehoon/OptiScaler`  
**Target branch:** `master`  
**Baseline reviewed:** `865d188caabd5b01b78e71f7a84dfc046022f4c5`  
**Date:** 2026-09-12  
**Primary target:** Intel GPU + Monster Hunter Wilds + XeFG  

---

## 1. Goal

Investigate and fix a logging-sensitive crash in Monster Hunter Wilds where OptiScaler is stable with verbose/debug logging enabled but can crash when normal logging is disabled.

The first implementation must remain intentionally minimal:

> Change the four `FGHooks` Present/Resize reentrancy guards from process-global `static bool` state to `thread_local` state, then perform an A/B validation with logging disabled.

Do not broaden this work into a general XeFG refactor unless this narrow hypothesis is disproved by testing.

The **primary runtime validation environment is the real combined configuration used for the current MHW work:**

```text
Monster Hunter Wilds
Intel GPU
OptiScaler + XeFG
fork REFramework
OptiScaler logging OFF
REFramework XeFG/debug logging OFF
```

The previously observed `OptiScaler + Capcom Patcher` reproduction is supporting isolation evidence, not the required first test environment for this work order.

---

## 2. Reproduction evidence and current isolation

The tester reports the following behavior on Intel GPU / MHW / XeFG:

```text
OptiScaler logging ON  + REF debug logging ON   -> stable
OptiScaler logging OFF + REF debug logging OFF  -> crash
```

The same logging-sensitive behavior was also reported without REFramework, using:

```text
OptiScaler + Capcom Patcher
REFramework absent
OptiScaler logging ON  -> stable
OptiScaler logging OFF -> crash
```

That second result is important because it substantially lowers REFramework as the common root cause of this **specific logging-sensitive symptom**.

However, it does **not** change the primary validation target for this task. The first patched A/B test must use the normal `OptiScaler + fork REFramework` MHW environment because that is the configuration currently being validated for production use.

Do not modify REFramework as part of this work order.

The current working hypothesis is an OptiScaler timing/reentrancy race that is masked by logging overhead.

---

## 3. Why logging can change the result

OptiScaler currently defaults to synchronous logging:

```cpp
CustomOptional<bool> LogAsync { false };
```

Enabled verbose/debug logging can therefore materially change execution timing in hot paths such as:

```text
Present
Present1
ResizeBuffers
ResizeBuffers1
XeFG present / resize forwarding
```

This is a classic Heisenbug pattern: the instrumentation itself may reduce or remove the problematic interleaving.

REFramework debug logging can also perturb the same presentation chain by adding work while its D3D12 hook/lifecycle lock is held. Therefore, a run becoming stable when REF debug logging is enabled does not prove that REF fixed the underlying problem.

Do not treat logging as synchronization and do not fix the issue by adding sleeps, yields, extra logging, or forced flushes.

---

## 4. Primary code concern

Current `FGHooks` declares these guards as process-global static booleans:

```cpp
inline static bool _skipResize = false;
inline static bool _skipResize1 = false;
inline static bool _skipPresent = false;
inline static bool _skipPresent1 = false;
```

File:

```text
OptiScaler/hooks/FG_Hooks.h
```

These values are used as nested-call / reentrancy guards in `FG_Hooks.cpp`.

Representative Resize pattern:

```cpp
HRESULT FGHooks::hkResizeBuffers(...)
{
    if (_skipResize)
    {
        _skipResize = false;
        // nested/internal handling
        ...
    }

    _skipResize1 = true;
    const auto result = o_FGSCResizeBuffers(...);
    _skipResize1 = false;
    ...
}
```

and the inverse path:

```cpp
HRESULT FGHooks::hkResizeBuffers1(...)
{
    if (_skipResize1)
    {
        _skipResize1 = false;
        // nested/internal handling
        ...
    }

    _skipResize = true;
    const auto result = o_FGSCResizeBuffers1(...);
    _skipResize = false;
    ...
}
```

Present / Present1 use the same concept:

```cpp
_skipPresent1 = true;
auto result = FGPresent(...);
_skipPresent1 = false;
```

and:

```cpp
_skipPresent = true;
auto result = FGPresent(...);
_skipPresent = false;
```

These values appear to represent **same-thread nested-call state**, but they are currently shared by all threads.

---

## 5. Failure model to validate

A plausible cross-thread sequence is:

```text
Thread A:
    hkResizeBuffers1()
    _skipResize = true
    -> calls original / XeFG path

Thread B, concurrently:
    hkResizeBuffers()
    sees _skipResize == true
    incorrectly treats its independent call as Thread A's nested call
    clears _skipResize = false

Thread A:
    actual nested ResizeBuffers arrives
    _skipResize is now false
    nested call goes through the full hook path instead of the intended skip path
```

Equivalent contamination is possible between `Present` and `Present1`.

Logging can reduce the probability of this interleaving enough to hide the crash.

This work order does **not** claim the sequence above is already proven. The change below is a controlled A/B test of that hypothesis.

---

## 6. Required change — Stage 1 only

Change only the four reentrancy flags to `thread_local` storage:

```cpp
inline static thread_local bool _skipResize = false;
inline static thread_local bool _skipResize1 = false;
inline static thread_local bool _skipPresent = false;
inline static thread_local bool _skipPresent1 = false;
```

Expected file:

```text
OptiScaler/hooks/FG_Hooks.h
```

Do not change their meaning, initialization values, or existing control flow in the first patch.

Do not add a mutex around the full Present/Resize paths in this stage.

Do not change XeFG destroy/recreate lifecycle code in `XeFG_Dx12.cpp`.

Do not modify the owner-scoped swapchain lifetime fixes already present in this fork.

Do not modify REFramework.

---

## 7. Why `thread_local`, not merely `std::atomic<bool>`

Do not use `std::atomic<bool>` as the primary fix for these four guards.

Atomic state would remove a C++ data race on the individual variable, but would **not** fix the logical ownership problem:

```text
Thread A sets skip flag
Thread B observes Thread A's flag
Thread B takes the wrong nested-call path
```

If these values describe nested/reentrant state for the current call stack, each thread needs its own state.

`thread_local` is therefore the intended Stage 1 experiment.

If code review finds that one of these guards is intentionally expected to propagate across threads, stop and document that evidence before implementing the change.

---

## 8. Keep this patch isolated

Do not mix the following into Stage 1:

- `_lastPresentFlags` synchronization;
- `_lastFGFrameTime` synchronization;
- `State::fgLastFrame` synchronization;
- generic `State` thread-safety cleanup;
- XeFG context destroy/recreate changes;
- additional COM `Release()` logic;
- changes to `FGUseMutexForSwapchain`;
- REFramework compatibility changes;
- Capcom Patcher changes;
- logging architecture changes;
- sleeps, delays, retries, or timing workarounds;
- broad refactoring of `FG_Hooks.cpp`.

Those are possible follow-up topics only if the narrow A/B test fails.

The purpose of Stage 1 is to preserve causal clarity.

---

## 9. Code audit before implementation

Before changing the declarations, inspect all reads/writes of:

```text
_skipResize
_skipResize1
_skipPresent
_skipPresent1
```

Confirm:

1. They are used as reentrancy/nested-call guards.
2. No existing code intentionally depends on one thread setting a skip flag for a different thread.
3. Present/Present1 and ResizeBuffers/ResizeBuffers1 otherwise retain their current semantics.
4. `thread_local` introduces no exported ABI/interface change.

If an intentional cross-thread dependency is found, stop and report it before implementing Stage 1.

---

## 10. Primary validation matrix

### Test A — Primary failing control: REF configuration

Use the real environment first.

```text
Game: Monster Hunter Wilds
GPU: Intel
FG output: XeFG
OptiScaler: current baseline master
REFramework: fork build used by the current XeFG compatibility tests
Capcom Patcher: not used
OptiScaler logging: OFF
REF XeFG/debug logging: OFF
Patch: no thread_local change
```

Confirm that the known crash remains reproducible under the tester's normal sequence.

This is the primary baseline.

### Test B — Primary patched test: REF configuration

Use the exact same environment and sequence as Test A, changing only the OptiScaler build:

```text
OptiScaler logging: OFF
REF XeFG/debug logging: OFF
Patch: only the four thread_local guard changes
```

This is the decisive first test.

Repeat enough times to distinguish real improvement from a single lucky run.

### Test C — Logged control with REF

Use the same patched OptiScaler + REF setup:

```text
OptiScaler logging: ON
REF debug logging: ON or the tester's previously stable debug configuration
```

Confirm that the patch does not regress the previously stable logged configuration.

### Test D — Secondary REF-free isolation

Only after the primary REF A/B result is known, optionally repeat with:

```text
OptiScaler + Capcom Patcher
REFramework absent
OptiScaler logging: OFF
```

Purpose:

- confirm whether the same OptiScaler-only timing failure is removed without REF;
- strengthen attribution to OptiScaler if both environments improve;
- avoid using Capcom Patcher as the primary gate for the current REF/Opti production path.

The previously reported REF-free reproduction is already useful evidence, but it is not required to replace Test A/Test B as the first validation path.

---

## 11. Runtime actions to exercise

Use the tester's known reproduction path first.

If the crash is intermittent, include repeated execution of operations likely to cross Present/Resize boundaries:

```text
- game launch into normal rendering
- XeFG active gameplay
- menu transitions
- resolution/display-mode transitions if part of the known reproduction
- Alt+Tab / foreground transitions if part of the known reproduction
- repeated gameplay for approximately the same duration that normally reproduces the crash
```

Do not add artificial stress until the normal reproduction path has been compared.

---

## 12. Logging rules for the test build

The bug is specifically logging-sensitive, so diagnostics must not accidentally invalidate the experiment.

For Test B:

- keep OptiScaler logging in the tester's known failing OFF configuration;
- keep REF XeFG/debug logging OFF;
- do not add per-frame `LOG_DEBUG`, `LOG_TRACE`, or extra file logging to Present/Resize hooks;
- do not add `Sleep`, `yield`, debugger breaks, or forced synchronization for diagnostics.

If additional evidence is later required, prefer low-perturbation diagnostics such as counters or a fixed-size in-memory event buffer dumped outside the hot path.

Do not add such diagnostics in Stage 1 unless the A/B result is inconclusive.

---

## 13. Optional safety improvement only if code review requires it

The current code manually sets and clears the skip flags around calls.

Do **not** refactor this in Stage 1 unless an actual early-return or exception-safety defect is found in the exact affected path.

A scoped guard may be considered in a later patch, but the first experiment must isolate the storage-duration change by itself.

---

## 14. Build and static verification

Build the normal project configuration used for the fork's test artifact.

At minimum verify:

```text
- OptiScaler compiles successfully.
- No new compiler warnings are introduced by the four declarations.
- Existing relevant tests/build checks remain green.
- The generated DLL loads normally in the known MHW setup.
- The Stage 1 source diff contains no unrelated functional changes.
```

Expected functional diff should be approximately four declaration edits plus, if necessary, a narrowly focused comment/test update.

---

## 15. Acceptance criteria

Stage 1 is successful if all of the following are true:

1. The four `FGHooks` reentrancy guards are thread-local.
2. No unrelated Present/Resize/XeFG control flow is changed.
3. The **primary** `MHW Intel + XeFG + OptiScaler + fork REFramework` configuration is materially more stable with both OptiScaler logging and REF debug logging OFF.
4. The previously stable logged REF configuration remains stable.
5. No new sleeps/logging delays are required for stability.
6. If the optional `OptiScaler + Capcom Patcher` secondary test is performed, it should be recorded separately rather than substituted for the REF primary result.

If Test B removes the crash across repeated runs, treat that as strong evidence that process-global reentrancy state was a major contributor.

Do not immediately mix broader synchronization cleanup into the same PR unless separately justified.

---

## 16. Failure / stop conditions

If the thread-local change does **not** improve the logging-OFF REF reproduction, do not start randomly modifying lifecycle code in the same PR.

Stop and report the A/B result.

The next investigation should audit other hot-path shared state, with priority on:

```text
FGHooks::_lastPresentFlags
FGHooks::_lastFGFrameTime
State::fgLastFrame
other Present/FG state written from multiple threads
swapchain aliases and lifecycle state touched outside existing XeFG lifecycle locking
```

Establish which values are actually accessed from multiple threads before choosing atomics, mutexes, or thread-local storage.

Do not assume all shared values need the same synchronization model.

---

## 17. Important non-goals

This work is not intended to:

- redesign OptiScaler's logging system;
- redesign all `FGHooks` synchronization;
- serialize all Present calls globally;
- change XeFG API result semantics;
- revisit COM ownership fixes already merged in this fork;
- modify REFramework;
- modify Capcom Patcher;
- fix unrelated Capcom game issues;
- create a game-specific MHW sleep/timing workaround.

The desired result is a small, generally correct correction to reentrancy-state ownership if the hypothesis is confirmed.

---

## 18. Suggested implementation diff

The initial code change should be no broader than this conceptually:

```diff
-    inline static bool _skipResize = false;
-    inline static bool _skipResize1 = false;
-    inline static bool _skipPresent = false;
-    inline static bool _skipPresent1 = false;
+    inline static thread_local bool _skipResize = false;
+    inline static thread_local bool _skipResize1 = false;
+    inline static thread_local bool _skipPresent = false;
+    inline static thread_local bool _skipPresent1 = false;
```

Do not mechanically add `thread_local` to other `FGHooks` or `State` fields as part of this task.

---

## 19. Deliverable

Produce a focused PR with:

```text
1. the four thread_local changes;
2. any narrowly necessary explanatory comment/test only;
3. a short PR description explaining the logging-sensitive MHW reproduction;
4. primary A/B results from:
   OptiScaler + fork REFramework,
   Opti log OFF,
   REF debug log OFF;
5. the logged REF control result;
6. optional secondary REF-free OptiScaler + Capcom Patcher result, clearly labeled as secondary isolation evidence;
7. explicit confirmation that no logging/sleep timing workaround was added.
```

Suggested PR title:

```text
fix: make FG hook reentrancy guards thread-local
```

Suggested commit title:

```text
fix: isolate FG hook reentrancy state per thread
```

The implementation should remain small enough that the runtime A/B result can be attributed to this single semantic change.
