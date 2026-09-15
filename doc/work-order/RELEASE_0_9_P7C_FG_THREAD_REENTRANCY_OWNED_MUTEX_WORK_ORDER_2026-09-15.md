# Release 0.9 P7-C Work Order — Thread-Local FG Reentrancy and Thread-Aware OwnedMutex

Date: 2026-09-15

## Status

Implementation work order.

P7-B is complete. P7-C is the next and currently final planned synchronization-hardening step for the `reframework-0.9` compatibility branch before a final lifecycle audit.

This work order is designed from the **current `reframework-0.9` source as the implementation source of truth**.

> Do not cherry-pick PR #17 or PR #18 and do not copy `master` synchronization code into this branch.

The old draft PRs and `master` may be used only as reference material for already identified patterns. The implementation must be derived from the current release/0.9 call graph and must preserve the P5/P6/P7-A/P7-B lifecycle design already merged here.

The goal is:

> Same-thread hook recursion must be represented as thread-local state, and an `OwnedMutex` owner tag must never be mistaken for proof that the current OS thread owns the mutex.

P7-C is intentionally narrower than a general FG synchronization rewrite. It must remove code-proven false-reentrancy and wrong-thread-unlock hazards without introducing new vendor-callback waits or changing REF/XeFG lifecycle ordering.

---

## 1. Verified baselines

### OptiScaler target

Repository: `onehoon/OptiScaler`

Branch: `reframework-0.9`

Implementation baseline reviewed:

```text
b0621c37c4dd07f2bffb74a2b32dc0d3ea03f89f
P7-B: own XeFG D3D12 command queues across lifecycle (#32)
```

Relevant release/0.9 files reviewed:

```text
OptiScaler/OwnedMutex.h
OptiScaler/hooks/FG_Hooks.h
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/framegen/IFGFeature.h
OptiScaler/framegen/IFGFeature_Dx12.h
OptiScaler/framegen/ffx/FSRFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp
```

### Fork REFramework cross-check

Repository: `onehoon/REFramework`

Branch: `master`

REF baseline reviewed:

```text
4bf45b370e602f7f6a3ca54f308daa4e353aab8a
XeFG P5-B1: add final proxy pre-retire handoff (#54)
```

Relevant REF files reviewed:

```text
src/D3D12Hook.cpp
src/REFramework.hpp
```

### OptiScaler master/reference-only baseline

Reference only:

```text
onehoon/OptiScaler master
64874932da7ceb3475ed1e8fd8aaf837249f022a
```

Open draft PRs used only as historical/reference material:

```text
PR #17  fix: make FG hook reentrancy guards thread-local
PR #18  Fix cross-thread FG Present mutex ownership
```

Neither PR is merged. Neither is a source of truth for release/0.9.

---

# Verified problems

## 2. Problem A — the four FGHooks recursion guards are process-global

Current release/0.9 `FG_Hooks.h` still contains:

```cpp
inline static bool _skipResize = false;
inline static bool _skipResize1 = false;
inline static bool _skipPresent = false;
inline static bool _skipPresent1 = false;
```

These values do not represent process-wide lifecycle state. They represent **nested calls on the thread currently executing the hook**.

Current Present pairing is conceptually:

```cpp
// Present
_skipPresent1 = true;
auto result = FGPresent(...);
_skipPresent1 = false;

// Present1
_skipPresent = true;
auto result = FGPresent(...);
_skipPresent = false;
```

The ResizeBuffers / ResizeBuffers1 pair uses the same pattern.

With process-global storage this can happen:

```text
Thread A enters Present
    -> _skipPresent1 = true

Thread B independently enters Present1
    -> observes Thread A's _skipPresent1
    -> treats itself as a nested/internal call
    -> bypasses normal FG hook processing
```

That is false reentrancy. Logging or timing changes can alter the race window, but P7-C must treat this as a source-level correctness issue rather than as a claim that it is the sole cause of any specific game crash.

Required invariant:

```text
same-thread nested call     -> may observe its own recursion guard
other thread                -> must never consume another thread's recursion guard
```

---

## 3. Problem B — `OwnedMutex::owner` is an operation tag, not a thread owner

Current release/0.9 `OwnedMutex` stores only:

```cpp
std::shared_mutex mtx;
std::atomic<uint32_t> owner { 0 };
```

`lock(tag)` records the logical tag after locking, and `unlockThis(tag)` validates only that tag.

Therefore this state:

```text
owner == 2
```

means only:

```text
some thread currently owns the mutex for operation tag 2
```

It does **not** mean:

```text
the current thread owns the mutex for operation tag 2
```

Those two meanings are currently conflated at several call sites.

---

## 4. Problem C — FG Present can falsely bypass another thread's owner-2 lock

Current `FGHooks::FGPresent(...)` contains the equivalent of:

```cpp
if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
    Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
    fg->Mutex.getOwner() != 2)
{
    fg->Mutex.lock(2);
    mutexUsed = true;
}
```

This correctly avoids recursively locking the non-recursive mutex when Present re-enters on the **same thread**.

However, if Thread A owns tag 2 and Thread B enters Present, Thread B also sees `getOwner() == 2` and bypasses locking.

Conceptually:

```text
Thread A:
    Mutex.lock(2)
    FG Present critical section

Thread B:
    getOwner() == 2
    -> assumes recursion
    -> skips lock
    -> enters the same critical section concurrently
```

Required invariant:

```text
same thread + owner tag 2 -> recursion bypass
other thread + owner tag 2 -> serialize on the mutex
```

---

## 5. Problem D — `unlockThis()` can unlock a mutex from the wrong OS thread

Current `unlockThis(tag)` checks only the logical owner tag before calling:

```cpp
mtx.unlock();
```

A `std::shared_mutex` must be unlocked by the thread that owns the exclusive lock. A matching logical tag from another thread is not sufficient ownership.

This is not only theoretical because release/0.9 contains cross-function conditional unlock patterns such as:

```cpp
if (Mutex.getOwner() == 2)
    Mutex.unlockThis(2);
```

and:

```cpp
if (Mutex.getOwner() == 1)
    Mutex.unlockThis(1);
```

The conditional caller is not necessarily the function that originally acquired that tag.

P7-C must make wrong-thread unlock impossible at the primitive level and must audit these conditional-unlock call sites so valid same-thread callback escapes remain possible without relying on undefined mutex ownership behavior.

---

## 6. Problem E — wrapper owner-tag bypasses mix two different semantics

The current wrapper contains both:

### Same-thread hook recursion markers

`FGHooks::hkResizeBuffers()` acquires:

```cpp
OwnedLockGuard lg(fg->Mutex, 6677);
```

and then calls the original ResizeBuffers path.

`FGHooks::hkResizeBuffers1()` similarly uses:

```cpp
OwnedLockGuard lg(fg->Mutex, 6678);
```

The wrapped swapchain currently checks those tags to avoid taking owner tag 3 again.

For the direct nested path:

```text
FGHooks hkResizeBuffers / hkResizeBuffers1
    -> owns 6677 / 6678 on this thread
    -> invokes original/wrapped resize on the same thread
    -> wrapper must not recursively lock tag 3
```

These are same-thread recursion markers. Another thread observing 6677/6678 must not automatically receive the same bypass.

### Process-wide / thread-affinity-unknown suppression

The wrapper also has DLSSG/Nukems logic such as:

```cpp
if (!(_localMutex.getOwner() == 4 &&
      Config::Instance()->FGInput.value_or_default() == FGInput::Nukems))
{
    OwnedLockGuard lock(_localMutex, ...);
}
```

The code itself documents that DLSSG may call these paths from Present. The callback's OS-thread affinity is not established.

Changing this owner-4 bypass to current-thread-only could make a worker-thread vendor callback block on a Present thread that is waiting for that callback to return.

Therefore owner tag 4 must remain tag-based/global in P7-C.

---

# REF cross-check

## 7. Fork REF models hook recursion as thread-local state

The current fork REF D3D12 hook uses thread-local recursion depth variables including:

```cpp
thread_local int32_t g_present_depth = 0;
thread_local int32_t g_resize_buffers_depth = 0;
thread_local int32_t g_resize_buffers1_depth = 0;
thread_local int32_t g_resize_target_depth = 0;
```

REF also uses:

```cpp
std::recursive_mutex m_hook_monitor_mutex;
```

and holds the hook-monitor lock through the relevant original Present/Resize calls.

This confirms an important architectural distinction:

```text
hook recursion depth = per-thread state
cross-thread exclusion = mutex state
```

That is compatible with P7-C's model.

However, REF does **not** make OptiScaler's current process-global `_skip*` flags safe. Hook ordering is not guaranteed such that REF must always serialize before Opti's hooks. OptiScaler must maintain correct synchronization independently.

No REF source modification is expected for P7-C.

---

# P7-C invariants

## 8. Separate three concepts explicitly

Implementation and review must distinguish:

```text
A. same-thread recursion state
   -> thread_local guard or isOwnedByCurrentThread(tag)

B. global operation-in-progress state
   -> getOwner() / existing lifecycle state may remain valid

C. mutex ownership
   -> only the actual owning OS thread may unlock
```

Do not use one mechanism as a substitute for another.

---

## 9. Required implementation A — make the four hook guards thread-local

Change exactly the four FGHooks recursion flags to:

```cpp
inline static thread_local bool _skipResize = false;
inline static thread_local bool _skipResize1 = false;
inline static thread_local bool _skipPresent = false;
inline static thread_local bool _skipPresent1 = false;
```

Keep their existing control flow and meaning.

Do not add sleeps, yields, retries, global mutexes, or logging-dependent synchronization around these flags.

Do not convert unrelated FGHooks state to thread-local in this PR.

---

## 10. Required implementation B — make `OwnedMutex` track the actual owner thread

Add an owning-thread identifier alongside the logical operation tag.

Recommended shape:

```cpp
class OwnedMutex
{
  private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner { 0 };
    std::atomic<DWORD> ownerThread { 0 };

  public:
    void lock(uint32_t ownerTag)
    {
        mtx.lock();
        ownerThread.store(GetCurrentThreadId(), std::memory_order_release);
        owner.store(ownerTag, std::memory_order_release);
    }

    bool isOwnedByCurrentThread(uint32_t ownerTag) const
    {
        return owner.load(std::memory_order_acquire) == ownerTag &&
               ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
    }

    uint32_t getOwner() const
    {
        return owner.load(std::memory_order_acquire);
    }
};
```

Exact naming may differ.

`unlockThis(tag)` must require both:

```text
logical owner tag matches
AND
ownerThread == GetCurrentThreadId()
```

before calling `mtx.unlock()`.

Conceptually:

```cpp
void unlockThis(uint32_t ownerTag)
{
    const auto currentOwner = owner.load(std::memory_order_acquire);
    const auto owningThread = ownerThread.load(std::memory_order_acquire);
    const auto currentThread = GetCurrentThreadId();

    if (currentOwner != ownerTag || owningThread != currentThread)
    {
        LOG_WARN(...);
        return;
    }

    owner.store(0, std::memory_order_release);
    ownerThread.store(0, std::memory_order_release);
    mtx.unlock();
}
```

The owner fields are diagnostic/reentrancy metadata; the underlying mutex remains the actual exclusion primitive.

Do not implement recursion inside `OwnedMutex` itself. It must remain a non-recursive mutex with explicit call-site recursion handling.

---

## 11. Required implementation C — fix FG Present owner-2 recursion detection

Change the Present lock decision from tag-only to same-thread-aware.

Required behavior:

```cpp
if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
    Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
    !fg->Mutex.isOwnedByCurrentThread(2))
{
    fg->Mutex.lock(2);
    mutexUsed = true;
}
```

`mutexUsed` continues to mean this invocation acquired the mutex and therefore must release it.

Do not replace this with a global `owner != 2` check.

---

## 12. Required implementation D — SetFullscreenState owner-3 bypass is same-thread only

The `currentFG->Mutex` owner-3 check in wrapped `SetFullscreenState` is a recursion-avoidance path for the same call flow.

Convert only that FG mutex decision to:

```cpp
if (fg != nullptr && fg->IsActive() &&
    !fg->Mutex.isOwnedByCurrentThread(3))
{
    fg->Mutex.lock(3);
    ffxLock = true;
}
```

Keep the existing `_localMutex.getOwner() == 4` DLSSG/Nukems bypass tag-based/global.

Do not convert local owner 4 to `isOwnedByCurrentThread(4)` in P7-C.

---

## 13. Required implementation E — make 6677/6678 wrapper resize recursion thread-aware

The FGHooks resize tags are source-proven same-thread nesting markers because the hook owns the tag while invoking the original resize method.

For wrapped ResizeBuffers, use a local acquisition flag and current-thread tests.

Conceptual shape:

```cpp
bool ffxLock = false;
auto* fg = State::Instance().currentFG;

if (fg != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
{
    const bool nestedResizeHook =
        fg->Mutex.isOwnedByCurrentThread(6677) ||
        fg->Mutex.isOwnedByCurrentThread(6678);

    if (!nestedResizeHook)
    {
        fg->Mutex.lock(3);
        ffxLock = true;
    }
}

...

if (ffxLock && fg != nullptr)
    fg->Mutex.unlockThis(3);
```

This fixes both sides of the invariant:

```text
same-thread 6677/6678 -> do not recursively lock
other-thread 6677/6678 -> serialize instead of bypassing
```

Do not leave an unconditional `unlockThis(3)` when this invocation did not acquire tag 3.

---

## 14. ResizeBuffers1 — split proven recursion from ambiguous Present state

Current wrapped ResizeBuffers1 checks:

```text
6677
6678
2
```

These do not all have the same meaning.

Required classification:

```text
6677 / 6678
    = source-proven same-thread nested resize hook
    -> use isOwnedByCurrentThread(...)

2
    = Present operation observed in progress
    = thread affinity is not proven for every vendor callback path
    -> keep tag-based/global in P7-C
```

Recommended shape:

```cpp
bool ffxLock = false;
auto* fg = State::Instance().currentFG;

if (fg != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
{
    const bool nestedResizeHook =
        fg->Mutex.isOwnedByCurrentThread(6677) ||
        fg->Mutex.isOwnedByCurrentThread(6678);

    // Intentionally global for P7-C. Do not convert without a proven
    // same-thread contract for every vendor callback path.
    const bool presentOperationInProgress = fg->Mutex.getOwner() == 2;

    if (!nestedResizeHook && !presentOperationInProgress)
    {
        fg->Mutex.lock(3);
        ffxLock = true;
    }
}

...

if (ffxLock && fg != nullptr)
    fg->Mutex.unlockThis(3);
```

The exact structure may differ, but the lock/unlock must be balanced by actual acquisition, not by backend assumptions.

This also removes the current fragile pattern where tag 3 may be acquired in the function while the final unlock is controlled by a different condition such as `activeFgOutput == FSRFG`.

Do not unlock an outer 6677/6678 lock from the wrapper.

---

# Conditional unlock audit

## 15. `FSRFG_Dx12::EvaluateState` owner-2 conditional unlock

Current code contains:

```cpp
if (Mutex.getOwner() == 2)
    Mutex.unlockThis(2);
```

`EvaluateState()` is reached from FFX frame-generation prepare paths and may be invoked from vendor callback contexts.

P7-C must preserve valid same-thread callback escape behavior while preventing wrong-thread mutex unlock.

Required change:

```cpp
if (Mutex.isOwnedByCurrentThread(2))
    Mutex.unlockThis(2);
```

Do **not** add a wait for owner 2 here. A worker callback may be part of the Present operation that owns tag 2, and blindly waiting could create a callback deadlock.

If runtime validation later proves an unsafe cross-thread mutation here, that is a separate synchronization problem and must not be guessed into P7-C.

---

## 16. `XeFG_Dx12::EvaluateState` owner-2 conditional unlock

XeFG has the analogous pattern:

```cpp
if (Mutex.getOwner() == 2)
    Mutex.unlockThis(2);
```

Apply the same current-thread-only rule:

```cpp
if (Mutex.isOwnedByCurrentThread(2))
    Mutex.unlockThis(2);
```

Do not change XeFG lifecycle quarantine, P7-A handoff ordering, or P7-B queue ownership while doing this.

---

## 17. `FSRFG_Dx12::Dispatch` owner-1 conditional unlock

Current FSRFG Dispatch contains a conditional owner-1 unlock even though Dispatch itself is not the owner-1 acquisition site:

```cpp
if (Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
    Mutex.getOwner() == 1)
{
    Mutex.unlockThis(1);
}
```

This appears to serve a same-thread nested callback/escape path while release is in progress.

Required change:

```cpp
if (Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
    Mutex.isOwnedByCurrentThread(1))
{
    Mutex.unlockThis(1);
}
```

Do not wait on owner 1 from Dispatch and do not redesign FSRFG release lifecycle in this PR.

---

## 18. Audit every `unlockThis(...)` call on the P7-C implementation branch

Before the implementation PR is considered complete, run a repository-wide audit of every `unlockThis(` call in the current branch.

Classify each call as one of:

```text
1. direct paired owner
   same function / same RAII scope acquired it
   -> safe after primitive thread check

2. same-thread nested callback escape
   caller did not acquire here, but same-thread ownership is intentional
   -> guard with isOwnedByCurrentThread(tag)

3. cross-thread/global observation
   caller merely sees another operation is in progress
   -> must never call unlockThis
```

Do not assume matching tags imply category 1 or 2.

Any newly discovered explicit conditional unlock that is not obviously paired with the local acquisition must be resolved in the PR rather than left relying on tag-only ownership.

---

# Intentionally unchanged global-state observations

## 19. Do not mechanically replace every `getOwner()` comparison

The following patterns are intentionally not globally converted to current-thread checks.

### 19.1 DLSSG/Nukems `_localMutex` owner 4

Keep:

```cpp
_localMutex.getOwner() == 4
```

for the documented DLSSG Present re-entry/deadlock avoidance path.

Thread affinity is not proven. Making it current-thread-only could introduce a worker-thread callback deadlock.

### 19.2 Release-lifecycle owner 1 observations

XeFG and FSRFG contain owner-1 checks that mean, conceptually:

```text
release lifecycle is already in progress
```

Those are global lifecycle-state observations, not automatically recursion checks.

Do not replace such early-return/defer checks with `isOwnedByCurrentThread(1)` unless the current release/0.9 call graph proves the check is strictly same-thread recursion.

In particular, preserve the P5/P6/P7-A fail-closed lifecycle behavior.

### 19.3 Ambiguous wrapped ResizeBuffers1 owner 2

Keep the owner-2 Present-in-progress bypass global in P7-C as described above.

This is a deliberate conservative exception, not an endorsement that every cross-thread access is safe. It avoids introducing a new wait/deadlock assumption without runtime proof.

---

# Required non-changes

## 20. No REF change

P7-C is expected to be OptiScaler-only.

Do not change:

```text
REFramework hook monitor
REF XeFG binding
REFramework_XeFG_PreRetireSwapchainV1
REF Present/Resize lifecycle
```

The REF audit is evidence for the thread model, not a request to modify REF.

## 21. No P7-A lock-order changes

Keep:

```text
REF pre-retire handoff
    -> FG mutex
    -> vendor/lifecycle teardown
```

Do not move REF calls under the FG mutex.

## 22. No P7-B queue changes

Do not change:

```text
XeFG owned command queue
candidate queue commit timing
FGHooks queue/fence generation ownership
RetireQueueGeneration
```

except for compilation-only adjustments if absolutely required.

## 23. No XeFG/XeLL lifecycle or result-code policy changes

P5/P6 semantics remain unchanged.

Do not alter:

```text
XeFG exact-success critical commit rules
XeFG Destroy warning/error quarantine behavior
XeLL exact-success lifecycle
fakenvapi publish/unpublish ordering
```

## 24. No master architecture backport

Do not import:

```text
master FG hook architecture
master XeFG architecture
master XeLL integration
PR #17 branch wholesale
PR #18 branch wholesale
PR #19 experimental tracing/state changes
```

The old draft PRs may be cited as prior hypotheses only.

## 25. No timing hacks

Do not add:

```text
Sleep
Yield
retry loops
extra synchronization delays
logging used as synchronization
forced single-thread execution
```

P7-C must fix ownership semantics, not alter timing to hide races.

---

# Expected implementation files

## 26. Primary files

Expected files are approximately:

```text
OptiScaler/OwnedMutex.h
OptiScaler/hooks/FG_Hooks.h
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/framegen/ffx/FSRFG_Dx12.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Additional files are acceptable only if the repository-wide `unlockThis` audit finds a concrete current-branch call site that needs the same ownership correction.

No REFramework file should be changed.

Recommended PR title:

```text
P7-C: make FG reentrancy and mutex ownership thread-aware
```

---

# Validation

## 27. Static/build validation

Required:

```text
Release x64 build
clang-format dry-run/check
git diff --check
```

Also perform explicit audits for:

```text
_skipPresent
_skipPresent1
_skipResize
_skipResize1
getOwner()
unlockThis(
OwnedLockGuard
6677
6678
owner tag 1
owner tag 2
owner tag 3
_localMutex owner tag 4
```

Acceptance requires being able to explain the semantics of every changed `getOwner()` or `unlockThis()` call.

---

## 28. Required unit/static reasoning cases

### A. Same-thread Present recursion

```text
Thread A owns tag 2
Thread A nested Present/Present1
-> no recursive lock
-> nested call follows existing bypass semantics
```

### B. Cross-thread Present overlap

```text
Thread A owns tag 2
Thread B enters Present
-> Thread B must not interpret tag 2 as its own recursion
-> Thread B waits for the mutex
```

### C. Present/Present1 guard isolation

```text
Thread A sets _skipPresent1
Thread B enters Present1
-> Thread B must not observe Thread A's guard
```

### D. Resize/ResizeBuffers1 guard isolation

Same rule as C for `_skipResize*`.

### E. Same-thread FGHooks resize -> wrapper recursion

```text
Thread A owns 6677 or 6678
Thread A enters wrapped resize
-> wrapper skips tag-3 recursive lock
-> wrapper does not unlock the outer 6677/6678 owner
```

### F. Cross-thread resize while 6677/6678 is owned

```text
Thread A owns 6677/6678
Thread B enters wrapper resize
-> Thread B must not receive same-thread recursion bypass
-> it serializes according to the wrapper's normal tag-3 path
```

### G. Wrong-thread `unlockThis`

```text
Thread A owns tag X
Thread B calls unlockThis(X)
-> underlying mutex must not be unlocked
```

The call should fail closed at the ownership check.

### H. Same-thread conditional callback unlock

For the audited EvaluateState/Dispatch escape cases:

```text
same thread owns matching tag
-> conditional release may proceed
```

but:

```text
another thread owns matching tag
-> conditional release must not occur
```

### I. DLSSG/Nukems local owner-4 path

Existing tag-based bypass remains unchanged.

Do not require same-thread ownership for this path in P7-C.

---

# Runtime matrix

## 29. Priority runtime validation

### Primary

```text
Monster Hunter Wilds
Intel GPU
XeFG
fork REFramework
OptiScaler reframework-0.9
```

Test both:

```text
OptiScaler logging OFF + REF debug/XeFG logging OFF
OptiScaler logging ON  + REF debug logging ON
```

The purpose is not to prove that P7-C fixes every MHW crash. It is to verify that removing timing-sensitive false reentrancy does not regress the known stable configurations and improves or preserves the previously unstable one.

### Additional REF titles

```text
Dragon's Dogma 2 + REF + XeFG
RE9 / current Capcom REF validation title + XeFG
```

### Non-REF regression

At least one non-Capcom/non-REF XeFG title should verify that the thread-aware mutex change is not accidentally dependent on REF.

### FSRFG regression

Because `OwnedMutex` is shared by FG backends and P7-C audits FSRFG conditional unlocks, perform at least one FSRFG smoke test:

```text
launch
FG active Present
resize/window-mode transition
FG disable/enable
clean exit
```

### DLSSG/Nukems regression

Where available, run one smoke test to confirm the intentionally unchanged `_localMutex owner == 4` bypass has not been altered.

---

## 30. Stress sequences

Exercise repeatedly where practical:

```text
Present / Present1 alternation
ResizeBuffers
ResizeBuffers1
windowed <-> borderless/fullscreen transitions
rapid FG enable/disable
swapchain recreation
REF overlay active/inactive
clean game exit
```

For MHW specifically, include the configuration that previously showed logging-sensitive behavior.

---

# Review checklist

## 31. Reviewer must reject the PR if any of these appear

Reject or request changes if the implementation:

```text
- cherry-picks PR #17/#18 wholesale instead of adapting current 0.9
- converts every getOwner() check mechanically
- makes DLSSG/Nukems owner 4 current-thread-only without proof
- allows unlockThis() from a non-owning OS thread
- adds a wait inside ambiguous vendor callback paths solely because another thread owns tag 1/2
- changes REF source
- changes P7-A pre-retire ordering
- changes P7-B queue ownership
- changes XeFG/XeLL result semantics
- adds sleep/yield/logging timing workarounds
- leaves wrapper tag-3 unlock unbalanced with actual acquisition after touching the resize logic
```

---

# Acceptance criteria

P7-C is complete only when all of the following are true:

- the four FGHooks Present/Resize recursion guards are thread-local;
- `OwnedMutex` tracks the actual owning OS thread as well as the logical owner tag;
- `unlockThis()` cannot unlock from a non-owning thread;
- FG Present owner-2 recursion detection is current-thread-aware;
- wrapped SetFullscreenState owner-3 recursion detection is current-thread-aware;
- FGHooks 6677/6678 nested resize bypasses are current-thread-aware in wrapped resize paths;
- wrapped resize tag-3 unlocks are balanced with whether that invocation actually acquired tag 3;
- FSRFG and XeFG conditional owner-2 callback unlocks only occur for the owning current thread;
- FSRFG conditional owner-1 Dispatch unlock only occurs for the owning current thread;
- all other `unlockThis()` call sites are audited and classified;
- DLSSG/Nukems `_localMutex owner == 4` remains tag-based/global;
- ambiguous wrapped ResizeBuffers1 Present owner-2 observation remains conservative/global unless a same-thread contract is proven during implementation review;
- lifecycle owner-1 global/defer semantics remain intact;
- no REF changes are introduced;
- no master architecture is imported;
- P5/P6/P7-A/P7-B behavior remains intact;
- Release x64 build and formatting checks pass;
- runtime smoke/stress validation shows no regression in REF + XeFG and shared FSRFG paths.

After P7-C, perform a final release/0.9 lifecycle/synchronization audit. Create a new implementation phase only if that audit finds another concrete source-proven blocker; do not invent a P8 solely to continue the sequence.
