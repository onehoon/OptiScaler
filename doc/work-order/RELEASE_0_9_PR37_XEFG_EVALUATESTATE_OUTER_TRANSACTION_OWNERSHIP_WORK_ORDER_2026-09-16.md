# Release 0.9 — PR37 XeFG EvaluateState Outer Transaction Ownership Work Order

**Status:** Implementation work order  
**Target branch:** `reframework-0.9`  
**Reviewed branch tip:** `c64ae7ccf2edbc8c8ac0f4426e06ee9277a5ddb3`  
**Parent implementation:** `1199b5f8433c0693b01a91d94dd4fb13dfccfaf5` (`fix: serialize Streamline XeFG frame input with present (#36)`)  
**Audit source:** `RELEASE_0_9_INTEL_MHW_E_ABORT_4004_STREAMLINE_XEFG_FRAME_ORDER_AUDIT_REPORT_2026-09-16.md`  
**Audit finding:** `SDLX-015`  
**Date:** 2026-09-16

---

## 1. Objective

Implement the smallest safe correction for audit finding **SDLX-015**:

> `XeFG_Dx12::EvaluateState()` must not release an outer `fg->Mutex` transaction that was acquired by its caller and is still required after `EvaluateState()` returns.

PR36 intentionally placed Streamline XeFG input and DXGI Present in the same CPU transaction domain by acquiring `fg->Mutex` with logical owner `2` around the Streamline adapter entry points.

The current source still contains legacy logic inside `XeFG_Dx12::EvaluateState()` that conditionally calls:

```cpp
if (Mutex.isOwnedByCurrentThread(2))
    Mutex.unlockThis(2);
```

when `State::FGchanged` is set.

After PR36, that unlock is no longer ownership-safe: `EvaluateState()` did not necessarily acquire owner `2`, yet it may release the caller's outer transaction while the caller still has frame-state work to perform.

The purpose of PR37 is to restore the invariant:

> The scope that acquires the frame transaction is the scope that releases it.

This PR is **not** a COM-lifetime repair, resource-map rewrite, queue/fence redesign, or lifecycle refactor.

---

## 2. Current source state

### 2.1 PR36 outer transaction

File:

```text
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
```

Current helper:

```cpp
namespace
{
constexpr uint32_t kFrameTransactionMutexOwner = 2;

class ScopedStreamlineFGTransaction
{
  private:
    IFGFeature_Dx12* _fg = nullptr;
    bool _locked = false;

  public:
    explicit ScopedStreamlineFGTransaction(IFGFeature_Dx12* fg) : _fg(fg)
    {
        if (_fg == nullptr || !Config::Instance()->FGUseMutexForSwapchain.value_or_default() ||
            State::Instance().activeFgOutput != FGOutput::XeFG)
        {
            _fg = nullptr;
            return;
        }

        if (_fg->Mutex.isOwnedByCurrentThread())
            return;

        _fg->Mutex.lock(kFrameTransactionMutexOwner);
        _locked = true;
    }

    ~ScopedStreamlineFGTransaction()
    {
        if (_locked)
            _fg->Mutex.unlockThis(kFrameTransactionMutexOwner);
    }
};
}
```

The helper is used by:

```text
Sl_Inputs_Dx12::setConstants()
Sl_Inputs_Dx12::evaluateState()
Sl_Inputs_Dx12::reportResource()
Sl_Inputs_Dx12::markPresent()
```

For `setConstants()`, the intended transaction is broader than `EvaluateState()`:

```text
ScopedStreamlineFGTransaction acquire owner 2
  -> CheckForFrame()
  -> build FG_Constants
  -> XeFG_Dx12::EvaluateState()
  -> activation/paused checks
  -> camera matrix processing
  -> SetCameraValues()
  -> SetJitter()
  -> SetMVScale()
  -> SetCameraData()
  -> SetReset()
  -> SetFrameTimeDelta()
ScopedStreamlineFGTransaction release owner 2
```

All of the post-`EvaluateState()` operations above are part of the same Streamline frame-input transaction and must remain serialized against `FGHooks::FGPresent()`.

### 2.2 Current legacy unlock in `EvaluateState()`

File:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Current tail of `XeFG_Dx12::EvaluateState()`:

```cpp
if (state.FGchanged)
{
    LOG_DEBUG("FGchanged");

    state.FGchanged = false;

    Hudfix_Dx12::ResetCounters();

    // Pause for 10 frames
    UpdateTarget();

    // Release FG mutex
    if (Mutex.isOwnedByCurrentThread(2))
        Mutex.unlockThis(2);
}

state.SCchanged = false;
```

This code predates PR36's caller-owned Streamline RAII transaction.

### 2.3 `OwnedMutex` semantics

File:

```text
OptiScaler/OwnedMutex.h
```

`OwnedMutex` is non-recursive and stores one logical owner plus one OS thread ID.

Relevant methods:

```cpp
void lock(uint32_t owner);
bool isOwnedByCurrentThread(uint32_t owner) const;
bool isOwnedByCurrentThread() const;
void unlockThis(uint32_t owner);
```

`unlockThis()` validates the current owner and thread, clears ownership metadata, and unlocks the underlying `std::shared_mutex`.

There is no scope nesting count or ownership token. Therefore an inner function cannot safely infer that it owns a lock merely because `isOwnedByCurrentThread(2)` is true.

---

## 3. Source-confirmed failure mechanism

Audit finding `SDLX-015` identifies the following reachable sequence:

```text
Thread A — Streamline constants callback

Sl_Inputs_Dx12::setConstants()
  ScopedStreamlineFGTransaction
    fg->Mutex.lock(owner 2)
    _locked = true

  CheckForFrame(...)

  XeFG_Dx12::EvaluateState(...)
    State::FGchanged == true

    ...

    Mutex.isOwnedByCurrentThread(2) == true
    Mutex.unlockThis(2)
      owner = 0
      ownerThread = 0
      shared_mutex.unlock()

  // setConstants() continues here WITHOUT the intended transaction
  SetCameraValues(...)
  SetJitter(...)
  SetMVScale(...)
  SetCameraData(...)
  SetReset(...)
  SetFrameTimeDelta(...)

                         Thread B — Present can now enter
                         fg->Mutex.lock(owner 2)
                         XeFG Present / Dispatch
                         frame-state reads

  ~ScopedStreamlineFGTransaction()
    _locked == true
    unlockThis(2)
      no longer owns the mutex
      warning / ownership mismatch
      OR another owner may now exist
```

The important defect is not the final extra `unlockThis()` warning by itself.

The important defect is that the **outer Streamline transaction can be terminated in the middle of `setConstants()`**, allowing Present or another frame-input path to observe partially updated frame state.

This directly undermines the Phase 1 experiment introduced by PR36.

---

## 4. Required ownership invariant

PR37 must establish the following rule:

```text
Acquire site owns release responsibility.
```

For the current Streamline transaction:

```text
ScopedStreamlineFGTransaction
    acquires owner 2
    -> owns the release
```

For DXGI Present:

```text
FGHooks::FGPresent
    acquires owner 2
    -> owns the release
```

For resize:

```text
resize scope / OwnedLockGuard
    acquires owner 6677/6678
    -> owns the release
```

For final retirement:

```text
ReleaseSwapchainLocked path
    acquires owner 1
    -> owns the release
```

A nested callee such as `XeFG_Dx12::EvaluateState()` must **not release a mutex solely because it observes that the current thread owns the same logical owner ID**.

Logical owner equality is not equivalent to acquisition ownership.

---

## 5. Required pre-change audit

Before modifying the code, inspect the current `reframework-0.9` branch and record every relevant call site and owner-2 acquire/release path.

At minimum inspect:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/OwnedMutex.h
```

Search for:

```text
EvaluateState(
Mutex.lock(2)
Mutex.unlockThis(2)
isOwnedByCurrentThread(2)
OwnedLockGuard
FGchanged
SCchanged
```

The implementation PR must state whether any direct `EvaluateState()` caller currently relies on `EvaluateState()` to release owner `2` on its behalf.

Do not assume the legacy unlock is unused merely because PR36 now provides an outer transaction.

If a caller is found that intentionally transfers release ownership into `EvaluateState()`, stop and document that path before implementing a broader change.

The expected result from the current reviewed source is that `EvaluateState()` should not own this release, but the implementation agent must verify this against the actual PR37 base.

---

## 6. Preferred implementation

### 6.1 Narrow correction

If call-site audit confirms that `EvaluateState()` does not own owner-2 acquisition, remove the internal release from `XeFG_Dx12::EvaluateState()`.

Preferred result:

```cpp
if (state.FGchanged)
{
    LOG_DEBUG("FGchanged");

    state.FGchanged = false;

    Hudfix_Dx12::ResetCounters();

    // Pause for 10 frames
    UpdateTarget();

    // Do not release fg->Mutex here.
    // EvaluateState does not own the frame transaction; the caller that
    // acquired the mutex is responsible for releasing it.
}

state.SCchanged = false;
```

A shorter comment is acceptable if it clearly documents ownership responsibility.

### 6.2 Do not replace it with another implicit unlock heuristic

Do **not** change this into variants such as:

```cpp
if (Mutex.isOwnedByCurrentThread())
    Mutex.unlockThis(2);
```

or:

```cpp
if (Mutex.getOwner() == 2)
    Mutex.unlockThis(2);
```

or thread-ID / call-stack / game-name heuristics.

Those still confuse observed ownership with acquisition responsibility.

### 6.3 Do not make `OwnedMutex` recursive in PR37

PR37 must not redesign `OwnedMutex` into a recursive mutex, add nesting counts globally, or reinterpret every logical owner.

That is outside the narrow SDLX-015 correction and would materially broaden synchronization semantics for Present, resize, and lifecycle paths.

### 6.4 Do not move the unlock into another XeFG helper

Do not preserve the old behavior indirectly by moving the unlock into:

```text
Deactivate()
UpdateTarget()
DestroyFGContext()
DestroySwapchainContext()
SetFrameCount()
```

unless an independently proven ownership contract requires it.

The goal is caller-owned transaction lifetime, not relocation of the same hidden ownership transfer.

---

## 7. Required transaction behavior after PR37

### 7.1 `setConstants()` with `FGchanged == false`

Expected:

```text
Streamline callback
  -> acquire fg->Mutex owner 2
  -> CheckForFrame
  -> EvaluateState
  -> all camera/jitter/MV/reset/frame-time writes
  -> release owner 2 at RAII scope exit
```

### 7.2 `setConstants()` with `FGchanged == true`

Expected:

```text
Streamline callback
  -> acquire fg->Mutex owner 2
  -> CheckForFrame
  -> EvaluateState
       -> Deactivate / UpdateTarget / optional context work
       -> clear FGchanged
       -> DO NOT release outer transaction
  -> remaining camera/jitter/MV/reset/frame-time writes or early-return logic
  -> release owner 2 at RAII scope exit
```

There must be no window after `EvaluateState()` in which a competing Present thread can acquire owner `2` before the Streamline scope completes.

### 7.3 Same-thread re-entry

PR36's owner-agnostic check must remain unchanged unless the implementation agent finds a concrete defect:

```cpp
if (_fg->Mutex.isOwnedByCurrentThread())
    return;
```

This preserves:

```text
Present(owner 2) -> Streamline same-thread re-entry
Streamline(owner 2) -> Present same-thread re-entry
lifecycle/resize owner -> Streamline same-thread re-entry
```

without introducing a second lock acquisition on the non-recursive mutex.

PR37 must not regress this behavior.

---

## 8. Explicit out-of-scope findings

The audit report identified additional defects. They are **not part of PR37**.

Do not fix the following in the same PR:

### SDLX-008 — COM lifetime / use-after-release patterns

Examples include:

```text
wrapped_swapchain.cpp
WaitForGPUIdle()
LocalPresent()
_real1.._real4 QueryInterface ownership
Reflex device12 retention
```

This should be a separate PR because it changes object ownership and wrapper lifetime behavior.

### SDLX-002 — `HasResource()` unlocked map read

Do not add resource-map locking in PR37.

### SDLX-003 — `GetResource()` raw pointer after lock release

Do not rewrite the resource accessor or hold `_resourceMutex` across Present in PR37.

### SDLX-005 — four-slot frame/ring ABA risk

Do not add generation tokens or frame snapshots in PR37.

### SDLX-006 — producer/presentation queue GPU dependency

Do not add D3D12 fences, queue waits, or change queue selection in PR37.

### SDLX-007 — ResizeBuffers1 queue-generation split

Do not change queue publication in PR37.

### SDLX-010 — shutdown/lifecycle drain

Do not modify PR34/PR35 retirement semantics or REFramework handoff in PR37.

The causal value of PR37 depends on keeping it small.

---

## 9. Files expected to change

Expected implementation change:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Potentially no other source file should need modification.

Allowed supporting changes if genuinely needed:

```text
comments
small tests specifically exercising transaction ownership
PR documentation
```

Changes to the following require explicit justification in the PR description:

```text
OptiScaler/OwnedMutex.h
OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
REFramework source
```

A PR37 implementation that spreads across COM lifetime, queue/fence, or resource-container files should be treated as scope creep.

---

## 10. Static validation requirements

### 10.1 Ownership audit

After the change, verify:

```text
XeFG_Dx12::EvaluateState()
```

contains no hidden release of owner `2`.

Verify every remaining:

```text
unlockThis(2)
```

in the relevant XeFG/FG code has a documented matching acquire responsibility.

### 10.2 Lock order

PR37 must preserve:

```text
fg->Mutex
  -> _frameBoundaryMutex
    -> _resourceMutex[index]
```

Do not introduce:

```text
_frameBoundaryMutex -> fg->Mutex
_resourceMutex -> fg->Mutex
```

### 10.3 Build/format

Required:

```text
clang-format check for changed files
git diff --check
Release x64 build of OptiScaler.sln
```

The PR must record the build result and produced binary status.

---

## 11. Runtime validation for PR37

PR37 is a transaction-ownership correction. It should receive focused runtime validation before the longer E_ABORT matrix.

### 11.1 Required focused test

Use a XeFG + Streamline/DLSSG path that can exercise `FGchanged`.

Prefer Intel MHW because it is the target path, but the purpose of this test is specifically ownership behavior, not yet proving the 4004 root.

Exercise at least one transition that causes or is expected to cause `FGchanged`, for example a supported FG state transition/toggle or another known safe state-change path.

Verify:

```text
no Present deadlock
no self-deadlock
no unmatched owner warning caused by the PR37 path
no loss of frame input after EvaluateState
no immediate XeFG context/present failure
clean exit
```

Do not add heavy trace logging merely to observe success if the logging itself materially changes the timing-sensitive target behavior.

If instrumentation is required, prefer narrow transition-only logging or counters rather than per-frame logging.

### 11.2 Intel MHW logging-OFF smoke

Before merging, if hardware access is available:

```text
OptiScaler file logging OFF
launch MHW
reach active gameplay with XeFG
exercise normal movement/rendering for a short smoke session
clean exit
```

This is a smoke test, not the final 5x10-minute causal acceptance matrix.

### 11.3 Long causal A/B timing

The full Intel MHW logging-OFF matrix should be run only when the P0 safety baseline is considered adequate.

The audit report recommends addressing both:

```text
SDLX-015 transaction ownership
SDLX-008 COM lifetime
```

before relying on long-duration A/B results.

Therefore PR37 itself must not claim:

```text
"E_ABORT 4004 fixed"
```

unless the required final runtime matrix is independently completed.

---

## 12. Regression matrix

At minimum verify or explicitly mark untested:

### Intel / MHW

```text
XeFG active
Streamline/DLSSG input
FGchanged transition
normal Present
resize if naturally exercised
clean shutdown
```

### NVIDIA / MHW

Ensure no obvious regression to PR35 shutdown behavior:

```text
normal active XeFG session
clean game exit
no final proxy-retire crash
```

A full multi-run PR35 shutdown matrix can be performed with the subsequent safety/acceptance pass, but PR37 must not knowingly regress it.

### Non-REF smoke

If readily available, verify that the removal of the internal unlock does not depend on REFramework presence.

PR37 is an OptiScaler ownership fix and must not become REF-specific.

---

## 13. Diagnostics guidance

Useful transition-only diagnostics, if needed:

```text
[XeFG][FrameTxn] event=fgchanged_enter owner=<...> current_thread_owned=<...>
[XeFG][FrameTxn] event=fgchanged_exit owner=<...> current_thread_owned=<...>
```

Do not add permanent per-frame logging to every constants/resource/Present call as part of this PR.

The target bug is logging-sensitive; excessive diagnostics can hide the behavior being tested.

Any temporary diagnostics must either be removed before merge or be sufficiently rare and low-overhead to avoid changing the experiment materially.

---

## 14. Forbidden fixes

Do not solve SDLX-015 using:

```text
Sleep()
std::this_thread::sleep_for()
YieldProcessor timing padding
forced logger flushes
Intel-only branches
MonsterHunterWilds.exe checks
libxess_fg.dll-name checks
thread-ID allowlists
call-stack allowlists
replacing the mutex with a global recursive mutex
broad lifecycle serialization
REFramework changes
```

The defect is an ownership-contract problem and should be fixed as such.

---

## 15. PR description requirements

The PR description must include:

1. audit finding reference: `SDLX-015`;
2. base commit used for implementation;
3. explanation that PR36 added a caller-owned Streamline/Present transaction using owner `2`;
4. explanation that `XeFG_Dx12::EvaluateState()` could still release that outer owner `2` when `FGchanged` was set;
5. call-site audit result stating whether any caller relied on that internal unlock;
6. exact ownership invariant after the fix;
7. changed files;
8. build/format validation;
9. focused runtime results or an explicit statement that runtime testing was unavailable;
10. statement that this PR does **not** claim to prove the E_ABORT 4004 root cause.

Suggested title:

```text
fix: preserve outer XeFG frame transaction through EvaluateState
```

---

## 16. Review checklist

A reviewer should reject the PR if any of the following are true:

```text
[ ] EvaluateState still releases owner 2 that it did not acquire.
[ ] The fix depends on checking only the current logical owner instead of acquisition ownership.
[ ] The fix makes OwnedMutex globally recursive without a separate design justification.
[ ] A new reverse lock order is introduced.
[ ] PR34/PR35 retirement behavior is modified.
[ ] COM lifetime fixes are mixed into the same PR.
[ ] Resource-map locking or queue/fence changes are mixed into the same PR.
[ ] Intel/MHW/vendor-specific heuristics are introduced.
[ ] Build/format validation is missing.
```

A reviewer should expect the final implementation to be very small if the call-site audit confirms the reviewed ownership model.

---

## 17. Acceptance criteria

PR37 is ready to merge when all of the following are true:

### Source

```text
PASS: EvaluateState no longer releases an outer caller-owned owner-2 transaction.
PASS: caller-owned RAII / Present scopes retain release responsibility.
PASS: no new lock-order inversion is introduced.
PASS: PR36 same-thread re-entry protection remains intact.
PASS: PR34/PR35 lifecycle behavior is unchanged.
```

### Validation

```text
PASS: clang-format check.
PASS: git diff --check.
PASS: Release x64 build.
PASS or explicitly documented unavailable: focused FGchanged runtime smoke.
```

### Scope

```text
PASS: no COM lifetime repair in this PR.
PASS: no resource-map lifetime rewrite in this PR.
PASS: no queue/fence redesign in this PR.
PASS: no REFramework source change.
```

---

## 18. Follow-up after PR37

The expected next safety PR is the audit's **SDLX-008 COM lifetime** correction, kept separate from PR37.

That follow-up should review at least:

```text
wrapped_swapchain.cpp::WaitForGPUIdle
wrapped_swapchain.cpp::LocalPresent
WrappedIDXGISwapChain4::_real1.._real4 ownership
ReflexHooks retained D3D12 device ownership
```

Only after the P0 ownership/lifetime baseline is stabilized should the long Intel MHW logging-OFF causal matrix be treated as strong evidence for or against the frame-order hypothesis.

Subsequent hardening candidates remain:

```text
SDLX-002 HasResource map locking
SDLX-003 GetResource escaped pointer lifetime
SDLX-005 frame/ring generation snapshot
SDLX-006 producer/presentation queue GPU dependency
SDLX-007 resize queue generation
SDLX-010 lifecycle drain
```

Do not fold those into PR37.

---

## 19. Completion report template

The implementing agent should report:

```text
PR37 implementation base:
Files changed:
Call sites audited:
Remaining unlockThis(2) sites and ownership rationale:
Exact ownership behavior before:
Exact ownership behavior after:
clang-format:
git diff --check:
Release x64 build:
Focused FGchanged runtime test:
Intel MHW logging-OFF smoke:
NVIDIA/PR35 shutdown smoke:
Known untested items:
```

The final report must explicitly distinguish static correctness from runtime validation.
