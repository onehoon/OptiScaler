# MHW XeFG Thread-Aware `OwnedMutex` Present Serialization Work Order

Date: 2026-09-12
Repository: `onehoon/OptiScaler`
Target branch: `master`
Scope: OptiScaler only

## Objective

Implement a narrowly scoped synchronization fix for a suspected multi-threaded XeFG present race seen in Monster Hunter Wilds (MHW), especially when OptiScaler logging is disabled.

The first implementation target is `OwnedMutex` ownership semantics used by the FG swapchain present path. The current implementation tracks only a logical owner tag (for example `2` for swapchain Present) and does not track the OS thread that owns the mutex. As a result, a second thread entering the same logical owner path can observe `owner == 2` and incorrectly skip locking even though another thread actually owns the mutex.

This work order is intentionally limited to making ownership checks thread-aware and validating whether that alone stabilizes the failing MHW XeFG case.

## Background / observed behavior

The current failing scenario is timing-sensitive:

- Game: Monster Hunter Wilds
- GPU: Intel
- FG output: XeFG
- OptiScaler logging ON: stable or substantially more stable
- OptiScaler logging OFF: crash can reproduce
- The same general symptom has also been reported without REFramework, so this investigation should stay inside OptiScaler.
- Previous MHW logs showed Present / Resize-related activity occurring on more than one thread.

Because synchronous logging changes scheduling and adds sink locking / file I/O / flushing, logging can plausibly hide a concurrency bug without fixing it.

## Primary code concern

Current `OwnedMutex` stores only a logical owner value:

```cpp
class OwnedMutex
{
  private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner { 0 };

  public:
    void lock(uint32_t _owner)
    {
        mtx.lock();
        owner.store(_owner, std::memory_order_release);
    }

    void unlockThis(uint32_t _owner)
    {
        uint32_t current_owner = owner.load(std::memory_order_acquire);
        if (current_owner == 0 || current_owner != _owner)
            return;

        owner.store(0, std::memory_order_release);
        mtx.unlock();
    }
};
```

The FG Present path currently contains logic equivalent to:

```cpp
if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
    config->FGUseMutexForSwapchain.value_or_default() && fg->Mutex.getOwner() != 2)
{
    fg->Mutex.lock(2);
    mutexUsed = true;
}
```

This avoids deadlock for same-path reentrancy, but `owner == 2` does not prove that the current thread owns the mutex.

Potential failure pattern:

```text
Thread A:
  Present
  lock(2)
  owner = 2

Thread B:
  Present
  sees owner == 2
  skips lock

Thread A + Thread B:
  execute FG Present critical path concurrently
```

That defeats the purpose of `FGUseMutexForSwapchain` in a multi-threaded Present case.

## Required implementation

### 1. Make `OwnedMutex` ownership thread-aware

Extend `OwnedMutex` so it tracks both:

- logical owner tag
- actual owning OS thread ID

Use an atomic thread ID field, for example:

```cpp
std::atomic<DWORD> ownerThread { 0 };
```

Update `lock()` so the thread ID is recorded only after the underlying mutex has been acquired:

```cpp
void lock(uint32_t ownerTag)
{
    mtx.lock();
    ownerThread.store(GetCurrentThreadId(), std::memory_order_release);
    owner.store(ownerTag, std::memory_order_release);
}
```

Update unlock handling so ownership state is cleared before unlocking the underlying mutex.

Do not change the logical owner tag model itself in this PR.

### 2. Add an explicit current-thread ownership query

Add a helper with semantics equivalent to:

```cpp
bool isOwnedByCurrentThread(uint32_t ownerTag) const
{
    return owner.load(std::memory_order_acquire) == ownerTag &&
           ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
}
```

Name may differ if there is a clearer project-consistent name.

The important semantic rule is:

> Same logical owner tag on a different thread must NOT count as reentrancy.

### 3. Fix the FG Present lock bypass

Replace the current `getOwner() != 2` reentrancy decision with the thread-aware helper.

Desired semantics:

```cpp
if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
    config->FGUseMutexForSwapchain.value_or_default() &&
    !fg->Mutex.isOwnedByCurrentThread(2))
{
    fg->Mutex.lock(2);
    mutexUsed = true;
}
```

Behavior must be:

- same-thread nested Present with owner tag `2` -> do not relock
- different-thread Present while owner tag `2` is active -> block on the mutex
- normal single-thread Present -> unchanged behavior

### 4. Audit other `OwnedMutex` owner-tag bypass checks

Search all `getOwner()`-based reentrancy checks.

If another path is clearly using `owner == N` to mean “the current thread already owns this operation,” convert that check to the new thread-aware helper in the same PR.

However, keep this audit conservative:

- do not redesign unrelated synchronization
- do not change owner tags
- do not replace `OwnedMutex` globally with another mutex type
- do not expand into general FG state synchronization

If a usage is ambiguous, leave it unchanged and note it in the PR description.

## Explicit non-goals

Do NOT include any of the following in this PR:

- `_skipPresent` / `_skipPresent1` conversion to `thread_local`
- `_skipResize` / `_skipResize1` conversion to `thread_local`
- changes to XeFG SDK callback assumptions
- changes to XeFG swapchain destroy / recreate ownership logic
- changes to the existing fork-specific XeFG lifecycle mutex
- REFramework changes
- Capcom Patcher changes
- new logging-based timing workarounds
- sleeps, yields, artificial delays, or retry loops
- broad conversion of FG state members to atomics
- unrelated cleanup or refactoring

This PR must test one hypothesis: **the logical owner tag is being mistaken for actual thread ownership.**

## Important implementation constraints

### Do not use logging as synchronization

Do not add high-frequency debug logging around Present as part of the fix. The bug under investigation is specifically sensitive to logging ON/OFF timing, so extra logging could hide the issue.

Small diagnostic logs that are not on the hot Present path are acceptable only if truly necessary.

### Preserve same-thread recursion protection

Do not simply remove:

```cpp
getOwner() != 2
```

and always call `lock(2)`.

The existing bypass appears intended to avoid self-deadlock during nested calls. The replacement must distinguish:

- same-thread reentrancy
- different-thread contention

### Preserve owner-tag diagnostics

Keep `getOwner()` available if it is still used for diagnostics / logging / operation-type visibility.

The fix should add thread ownership semantics, not remove the existing logical owner concept.

### Ensure unlock matches the real owning thread

Review `unlockThis()` while making this change.

At minimum, do not allow a different thread with the same owner tag to be treated as the valid unlock owner.

Preferred behavior is for `unlockThis(ownerTag)` to validate both the logical owner tag and current thread ID before clearing ownership and unlocking.

A safe pattern is conceptually:

```cpp
void unlockThis(uint32_t ownerTag)
{
    const auto currentOwner = owner.load(std::memory_order_acquire);
    const auto currentOwnerThread = ownerThread.load(std::memory_order_acquire);

    if (currentOwner != ownerTag || currentOwnerThread != GetCurrentThreadId())
    {
        LOG_WARN(...);
        return;
    }

    owner.store(0, std::memory_order_release);
    ownerThread.store(0, std::memory_order_release);
    mtx.unlock();
}
```

Exact memory ordering can be adjusted if there is a clear reason, but do not weaken synchronization casually.

## Files expected to change

Primary:

- `OptiScaler/OwnedMutex.h`
- `OptiScaler/hooks/FG_Hooks.cpp`

Possible additional files only if the conservative owner-check audit finds another obviously equivalent misuse.

Keep the diff small.

## Validation

### Build validation

- Build the normal x64 Release configuration used by the fork.
- No new compiler warnings from the changed code.
- Existing build behavior must remain unchanged.

### Primary tester scenario

The tester should not need special instrumentation.

Use the normal real-world combination:

- Monster Hunter Wilds
- Intel GPU
- XeFG output
- fork OptiScaler build containing this PR
- fork REFramework
- OptiScaler logging: OFF
- REFramework debug / XeFG diagnostic logging: OFF

The key test is simply whether the previous crash reproduces under normal play / transition conditions.

Exercise the same actions that previously exposed instability, including ordinary gameplay and relevant window/display transitions when practical.

### Regression smoke test

At minimum, confirm one or more non-Capcom XeFG games still:

- launch
- enable XeFG
- present generated frames
- exit normally

No extensive multi-GPU test matrix is required for this first hypothesis test.

## Result interpretation

### If MHW becomes stable with logging OFF

Treat this as strong evidence that `OwnedMutex` false-reentrancy was a real contributor.

Do not immediately bundle additional concurrency changes into this PR. Keep this fix isolated so the result remains attributable.

### If the crash still reproduces

Keep this fix only if code review confirms the thread-aware ownership behavior is independently correct and low-risk.

Then investigate the next candidates separately, especially:

- process-global `_skipPresent` / `_skipPresent1`
- process-global `_skipResize` / `_skipResize1`
- unprotected shared resize fence state (`resizeFenceValue`, fence/event lifetime)
- other plain shared Present state such as frame timing / frame counters

Do not mix those follow-up experiments into this PR.

## Acceptance criteria

This work is complete when:

1. `OwnedMutex` tracks actual owning thread identity in addition to its logical owner tag.
2. Same-thread reentrancy remains non-blocking where intended.
3. Different threads using the same owner tag no longer bypass the mutex.
4. FG Present uses thread-aware ownership to decide whether locking can be skipped.
5. Unlock validation cannot mistake another thread with the same tag for the owner.
6. The change remains narrowly scoped with no `_skip*`, XeFG lifecycle, REFramework, or unrelated refactor changes.
7. Release build succeeds.
8. The resulting build is ready for the simple MHW Intel XeFG logging-OFF A/B test described above.

## PR guidance

Keep this as a standalone PR with a focused title such as:

`Fix cross-thread FG Present mutex ownership`

The PR description should explain that `OwnedMutex::owner` represents an operation tag, not a thread identity, and that the previous Present reentrancy check could therefore allow a second thread to bypass serialization when another Present thread already held owner tag `2`.
