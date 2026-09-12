# Work Order: MHW XeFG Logging Heisenbug — Make FGHooks Reentrancy Guards Thread-Local

**Repository:** `onehoon/OptiScaler`  
**Target branch:** `master`  
**Baseline reviewed:** `865d188caabd5b01b78e71f7a84dfc046022f4c5`  
**Date:** 2026-09-12  
**Primary target:** Intel GPU + Monster Hunter Wilds + XeFG  

---

## 1. Goal

Investigate and fix a logging-sensitive crash in Monster Hunter Wilds where OptiScaler is stable with OptiScaler logging enabled but can crash when OptiScaler logging is disabled.

The first implementation must be intentionally minimal:

> Change the four `FGHooks` Present/Resize reentrancy guards from process-global `static bool` state to `thread_local` state, then perform an A/B validation with logging disabled.

Do not broaden this work into a general XeFG refactor unless this narrow hypothesis is disproved by testing.

---

## 2. Reproduction evidence and isolation

The tester reports the following behavior on Intel GPU / MHW / XeFG:

```text
OptiScaler logging ON  + REF debug logging ON   -> stable
OptiScaler logging OFF + REF debug logging OFF  -> crash
```

The important isolation result is that the same logging-sensitive behavior is also reproduced without REFramework:

```text
OptiScaler + Capcom Patcher
REF absent
OptiScaler logging ON  -> stable
OptiScaler logging OFF -> crash
```

This substantially lowers REFramework as the common root cause for this specific symptom.

Do not modify REFramework as part of this work order.

The current working hypothesis is an OptiScaler timing/reentrancy race that is masked by synchronous logging overhead.

---

## 3. Why logging can change the result

OptiScaler currently defaults to synchronous logging:

```cpp
CustomOptional<bool> LogAsync { false };
```

and the logger performs normal sink synchronization / file output on enabled log paths.

Therefore, enabling verbose/debug logging can materially change timing in hot paths such as:

```text
Present
Present1
ResizeBuffers
ResizeBuffers1
XeFG present / resize forwarding
```

A race that reproduces with logging disabled but disappears with logging enabled should be treated as a possible Heisenbug.

Do not treat logging as a synchronization mechanism and do not fix the issue by adding sleeps, yields, extra logging, or forced flushes.

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

These values are then used as nested-call / reentrancy guards in `FG_Hooks.cpp`.

Representative pattern:

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

These flags appear to represent **same-thread nested-call state**, but they are currently shared by all threads.

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

This work order does **not** claim the above sequence is proven. The change below is a controlled A/B test of that hypothesis.

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

Do not change their meaning, initialization values, or the existing control flow in this first patch.

Do not add a mutex around the full Present/Resize paths as part of this stage.

Do not change the XeFG destroy/recreate lifecycle code in `XeFG_Dx12.cpp`.

Do not modify the owner-scoped swapchain lifetime fixes already present in this fork.

---

## 7. Why `thread_local`, not merely `std::atomic<bool>`

Do not use `std::atomic<bool>` as the primary fix for these four guards.

Atomic state would remove a C++ data race on the individual variable, but it would **not** fix the logical ownership problem:

```text
Thread A sets skip flag
Thread B observes Thread A's flag
Thread B takes the wrong nested-call path
```

If these values describe nested/reentrant state for the current call stack, each thread needs its own state.

`thread_local` is therefore the intended Stage 1 experiment.

If later analysis proves that the guard must intentionally cross threads, stop and document that evidence rather than silently replacing it with another global synchronization scheme.

---

## 8. Keep this patch isolated

Do not mix the following into the Stage 1 implementation:

- `_lastPresentFlags` synchronization;
- `_lastFGFrameTime` synchronization;
- `State::fgLastFrame` synchronization;
- generic State thread-safety cleanup;
- XeFG context destroy/recreate changes;
- additional COM `Release()` logic;
- changes to `FGUseMutexForSwapchain`;
- REFramework compatibility changes;
- Capcom Patcher changes;
- logging architecture changes;
- new sleeps, delays, retries, or timing workarounds;
- broad refactoring of `FG_Hooks.cpp`.

Those are possible follow-up topics only if the narrow A/B test fails.

The purpose of Stage 1 is to preserve causal clarity.

---

## 9. Optional safety improvement only if required by code review

The current code manually sets and clears the skip flags around calls. Do **not** refactor this in Stage 1 unless an early-return or exception-safety defect is found in the exact affected path.

If a scoped guard is later needed, keep it thread-local and behaviorally equivalent, for example:

```cpp
class ScopedBoolFlag
{
public:
    explicit ScopedBoolFlag(bool& flag) : m_flag(flag)
    {
        m_flag = true;
    }

    ~ScopedBoolFlag()
    {
        m_flag = false;
    }

private:
    bool& m_flag;
};
```

But this is **not required for the first A/B patch**. Avoid introducing extra code before testing the storage-duration change by itself.

---

## 10. Primary validation matrix

The most important test is the configuration that reproduces the problem without REFramework.

### Test A — Primary failing control

```text
Game: Monster Hunter Wilds
GPU: Intel
FG output: XeFG
REFramework: absent
Capcom Patcher: present
OptiScaler logging: OFF
Patch: baseline master, no thread_local change
```

Confirm that the tester can still reproduce the crash under the known sequence.

Record only the minimum reproduction details needed to compare the builds.

### Test B — Primary patched test

Same environment and sequence as Test A:

```text
OptiScaler logging: OFF
Patch: only the four thread_local guard changes
```

This is the decisive test.

Repeat enough times to distinguish a real stability improvement from a single lucky run.

### Test C — Logging control

Same patched build:

```text
OptiScaler logging: ON
REFramework: absent
Capcom Patcher: present
```

Confirm that the patch does not regress the previously stable logged configuration.

### Test D — Secondary REF coexistence test

Only after the REF-free A/B test:

```text
OptiScaler + fork REFramework
OptiScaler logging: OFF
REF XeFG debug logging: OFF
```

Confirm that the patch does not break the combined REF/Opti path.

Do not use Test D to decide the primary hypothesis before Test B is completed.

---

## 11. Runtime actions to exercise

Use the same tester reproduction path first. If the crash is intermittent, include repeated execution of the operations most likely to cross Present/Resize boundaries:

```text
- game launch into normal rendering
- XeFG active gameplay
- menu transitions
- resolution or display-mode transitions if part of the known reproduction
- Alt+Tab / foreground transitions if part of the known reproduction
- repeated gameplay for the same duration that normally reproduces the crash
```

Do not introduce artificial stress that was not required to reproduce the original issue until the normal reproduction path has been compared.

---

## 12. Logging rules for the test build

The bug is specifically logging-sensitive, so diagnostics must not accidentally invalidate the experiment.

For the primary patched run:

- keep normal OptiScaler logging in the tester's known failing OFF configuration;
- do not add per-frame `LOG_DEBUG`, `LOG_TRACE`, or file logging to Present/Resize hooks;
- do not add `Sleep`, `yield`, debugger breaks, or forced synchronization for diagnostics.

If lightweight evidence is later required, prefer diagnostics that minimally perturb timing, such as counters sampled outside the hot path or a fixed-size in-memory event buffer dumped only after failure/exit.

Do not implement such diagnostics in Stage 1 unless the thread-local A/B result is inconclusive.

---

## 13. Code audit after the Stage 1 change

After changing the declarations, inspect all reads/writes of:

```text
_skipResize
_skipResize1
_skipPresent
_skipPresent1
```

Confirm:

1. They are used only as reentrancy/nested-call guards.
2. No code depends on one thread intentionally setting a skip flag for a different thread.
3. The paired Present/Present1 and ResizeBuffers/ResizeBuffers1 behavior remains otherwise unchanged.
4. No new include is required for `thread_local`.
5. No ABI/exported-interface change is introduced.

If any cross-thread dependency is found, stop and report it before changing semantics further.

---

## 14. Build and static verification

Build the normal project configuration used for the fork's test artifact.

At minimum verify:

```text
- OptiScaler compiles successfully.
- No new compiler warnings are introduced by the four declarations.
- All existing relevant tests/build checks remain green.
- The generated DLL loads normally in the known test setup.
```

Search the resulting diff and confirm that Stage 1 contains no unrelated source changes.

Expected functional diff should be approximately four declaration edits plus, if necessary, a focused test/comment update.

---

## 15. Acceptance criteria

Stage 1 is successful if all of the following are true:

1. The four FGHooks reentrancy guards are thread-local.
2. No unrelated Present/Resize/XeFG control flow is changed.
3. MHW Intel + XeFG with OptiScaler logging OFF is materially more stable in the REF-free `OptiScaler + Capcom Patcher` configuration.
4. Logging ON remains stable.
5. The combined OptiScaler + REFramework path is not regressed in a secondary smoke test.
6. No new sleeps/logging delays are required for stability.

If Test B removes the crash across repeated runs, treat this as strong evidence that process-global reentrancy state was a major contributor.

Do not immediately add broader synchronization cleanup to the same PR unless separately justified.

---

## 16. Failure / stop conditions

If the thread-local change does **not** improve the logging-OFF reproduction, do not start randomly modifying lifecycle code in the same PR.

Stop and report the A/B result.

The next investigation should then audit other hot-path shared state, with priority on:

```text
FGHooks::_lastPresentFlags
FGHooks::_lastFGFrameTime
State::fgLastFrame
other Present/FG state written from multiple threads
swapchain aliases and lifecycle state touched outside existing XeFG lifecycle locking
```

The next phase should establish which of those values are actually accessed from multiple threads before choosing atomics, mutexes, or thread-local storage.

Do not assume all shared values need the same synchronization model.

---

## 17. Important non-goals

This work is not intended to:

- redesign OptiScaler's logging system;
- redesign all FGHooks synchronization;
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

Do not mechanically add `thread_local` to other fields in `FGHooks` or `State` as part of this task.

---

## 19. Deliverable

Produce a focused PR with:

```text
1. the four thread_local changes;
2. any narrowly necessary explanatory comment/test only;
3. a short PR description explaining the logging-sensitive MHW reproduction;
4. A/B test results, clearly separating:
   - REF-free OptiScaler + Capcom Patcher result;
   - optional OptiScaler + REFramework smoke result;
5. explicit confirmation that no logging/sleep timing workaround was added.
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