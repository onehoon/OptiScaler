# MHW XeFG Automatic Low-Overhead Trace Diagnostic Work Order

Date: 2026-09-12
Repository: `onehoon/OptiScaler`
Document branch: `master`
Implementation base: latest head of PR #18 / `fix/fg-present-thread-aware-ownedmutex`
Current reviewed PR #18 head at time of writing: `14fe3be1687fdd5d9db45c46f09d4cd9a1dd2d88`
Suggested implementation branch: `diag/mhw-xefg-auto-ring-trace`
Scope: OptiScaler diagnostic instrumentation only

## Objective

Create a temporary, low-overhead diagnostic build for the Monster Hunter Wilds Intel XeFG logging-sensitive crash.

The build must automatically record the recent XeFG / FG swapchain lifecycle into a separate binary trace file without requiring any new INI option and without using normal `spdlog` / `LOG_*` calls on the hot Present / Resize paths.

The tester workflow must be intentionally simple:

1. Install the diagnostic OptiScaler DLL.
2. Keep normal OptiScaler logging disabled.
3. Keep REFramework debug / XeFG diagnostic logging disabled.
4. Launch Monster Hunter Wilds and reproduce the crash normally.
5. Send `OptiScaler_XeFGTrace.bin` together with the normal REFramework log.

The purpose of this PR is observation only. Do not fix `_skip*`, fence synchronization, Present serialization, Resize serialization, or XeFG lifecycle behavior in the same PR.

## Branch / PR policy

PR #18 must remain unmerged while this diagnostic experiment is performed.

Do **not** add this instrumentation commit to PR #18 itself.

Create a separate branch from the latest PR #18 head so the diagnostic build contains the reviewed thread-aware `OwnedMutex` change while preserving PR #18 as a clean A/B synchronization patch.

Conceptually:

```text
master
  \
   PR #18: fix/fg-present-thread-aware-ownedmutex
      \
       diagnostic branch: diag/mhw-xefg-auto-ring-trace
```

The diagnostic PR should be stacked on PR #18 and should be clearly marked as temporary / diagnostic-only.

Do not merge the diagnostic PR into `master` as production behavior without a separate review and explicit decision.

## Why this diagnostic is needed

The failing MHW Intel XeFG case is timing-sensitive:

- OptiScaler normal logging ON: stable or substantially more stable.
- OptiScaler normal logging OFF: crash can reproduce.
- PR #18 corrected a real cross-thread `OwnedMutex` ownership problem, but the crash still reproduced after that change.
- Two crash-session REFramework logs (`22re2_framework_log (2).txt` and `33re2_framework_log (2).txt`) do not show a corresponding REF-side `E_ABORT` or failed `ResizeBuffers1` return before termination.
- Both crash logs show successful initial XeFG binding and successful REF renderer recovery, followed by the actual fatal/termination sequence much later.
- Therefore normal high-frequency file logging is undesirable because it can perturb thread scheduling enough to hide the bug we are trying to observe.

The next diagnostic must preserve the logging-OFF timing profile as much as practical while still retaining the last lifecycle events after a process crash.

## Current code areas that must be observable

The PR #18 branch still contains process-global reentrancy flags in `FG_Hooks.h`:

```cpp
inline static bool _skipResize = false;
inline static bool _skipResize1 = false;
inline static bool _skipPresent = false;
inline static bool _skipPresent1 = false;
```

`FG_Hooks.cpp` also contains shared resize/lifecycle state:

```cpp
static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;
static IUnknown* oldSwapChain = nullptr;
static ID3D12CommandQueue* currentCommandQueue = nullptr;
```

Current Present paths set the counterpart skip flag around `FGPresent()`:

```cpp
_skipPresent1 = true;
auto result = FGPresent(This, SyncInterval, Flags, nullptr);
_skipPresent1 = false;
```

and:

```cpp
_skipPresent = true;
auto result = FGPresent(This, SyncInterval, Flags, pPresentParameters);
_skipPresent = false;
```

Current Resize paths similarly pair `_skipResize` / `_skipResize1` around proxy Resize calls.

The diagnostic must let us answer, from one crash trace, at least these questions:

1. Did a different thread observe a process-global `_skipPresent*` or `_skipResize*` flag while another thread had set it?
2. Did another Present arrive while owner tag `2` was held by a different thread, and did PR #18 correctly serialize it?
3. Did Present and Resize overlap in time?
4. Did multiple Resize/final-release paths touch the same `resizeFence`, `resizeFenceEvent`, or `resizeFenceValue` lifecycle concurrently?
5. What was the final successful/failed underlying DXGI Present / Present1 / ResizeBuffers / ResizeBuffers1 result?
6. What was the final XeFG SDK result immediately before the failure/termination sequence?
7. Was a swapchain/context destroy, release, recreate, activate/deactivate, or final proxy release transaction in progress at the time?

## Required design

### 1. No new INI option

Do not add a configuration key.

This is a dedicated diagnostic build. The trace must initialize automatically.

Recommended activation rule:

- lazily initialize the trace when the process first enters an XeFG-specific initialization / swapchain path, or
- initialize at OptiScaler startup and record only XeFG-relevant events.

Prefer the smallest change that guarantees the file exists for the MHW Intel XeFG test.

If initialization fails, do not fail OptiScaler or disable XeFG. Diagnostic tracing must be best-effort and non-fatal.

### 2. Automatic separate file

Create the trace next to the loaded OptiScaler module, not in an arbitrary current working directory.

Required current-session filename:

```text
OptiScaler_XeFGTrace.bin
```

Recommended startup behavior:

```text
OptiScaler_XeFGTrace.prev.bin   <- previous session, if present
OptiScaler_XeFGTrace.bin        <- current session
```

At startup / first initialization only, it is acceptable to delete the old `.prev` file and rename the previous current file to `.prev` before creating the new current file.

This rotation is outside the hot path and protects the tester from accidentally losing the previous crash trace if the game is relaunched once before collection.

No timestamped-file explosion is needed.

### 3. Use a fixed-size memory-mapped ring buffer

Do not append textual records with `WriteFile` per event.

Create a fixed-size file once and map it with `CreateFileMapping` / `MapViewOfFile` (or an equivalent Windows mapping design).

All hot-path trace writes must then be ordinary mapped-memory writes plus the minimum atomic/interlocked operations necessary for multi-thread ordering.

Do not call any of the following per Present / Resize event:

- `spdlog::*`
- `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`
- `WriteFile`
- `FlushFileBuffers`
- `FlushViewOfFile`
- `std::format` / `fmt::format`
- heap allocation
- string construction
- mutex lock added solely for tracing
- sleep / yield / retry

The trace must not become an accidental synchronization mechanism.

### 4. Capacity must retain more than a few seconds

A very small 256/1024-record ring is not sufficient because the observed fatal UI / process termination sequence can continue for seconds after the underlying failure.

Use a capacity of at least **65,536 fixed-size records** unless code-size/alignment considerations justify a comparably useful capacity.

Target total file size should remain small (roughly a few MiB, not hundreds of MiB).

Example:

```cpp
static constexpr uint32_t kTraceCapacity = 65536;
```

Do not dynamically grow the file.

## Binary format requirements

The format must be explicitly versioned and stable enough to decode after the crash.

### Header

Use fixed-width integer fields. A conceptual header is:

```cpp
struct XeFGTraceHeader
{
    uint32_t magic;          // e.g. 'XFGT'
    uint32_t version;        // start at 1
    uint32_t headerSize;
    uint32_t recordSize;
    uint32_t capacity;
    uint32_t processId;

    uint64_t qpcFrequency;
    uint64_t sessionStartQpc;

    // Monotonically increasing sequence allocator / latest sequence.
    // Implement with an interlocked-safe aligned field.
    uint64_t writeSequence;
};
```

Exact layout may differ, but it must include:

- magic
- format version
- record size
- capacity
- PID
- QPC frequency
- session start QPC
- monotonically increasing sequence state

Add `static_assert` checks for the intended binary layout and alignment.

### Record

Use only fixed-width numeric fields. Do not store strings.

A conceptual record is:

```cpp
struct XeFGTraceRecord
{
    // Written last as the commit marker.
    uint64_t committedSequence;

    uint64_t qpc;
    uint64_t swapchain;
    uint64_t objectOrContext;
    uint64_t auxPointer;
    uint64_t fenceValue;

    uint32_t threadId;
    uint32_t eventType;
    uint32_t mutexOwner;
    uint32_t mutexOwnerThread;

    int32_t result;
    uint32_t flagsSnapshot;

    uint32_t aux0;
    uint32_t aux1;
};
```

The exact record may be adjusted, but retain equivalent information.

Keep pointers serialized as `uint64_t` / `uintptr_t` values only. Do not AddRef objects for tracing.

### Commit protocol

The trace will be written from multiple threads. A partially written record after a crash must be detectable.

Recommended protocol:

1. Atomically allocate a unique sequence number.
2. Compute `slot = sequence % capacity`.
3. Mark the slot uncommitted / invalid for the new write.
4. Populate all payload fields.
5. Execute an appropriate release fence.
6. Write `committedSequence = sequence` **last** using an aligned interlocked/atomic operation.

The decoder must ignore a record if its committed sequence does not match the sequence expected for that slot.

Do not put a C++ `std::atomic` object directly into a persistent packed binary layout unless its ABI/layout is deliberately handled. Prefer fixed integral fields plus Windows interlocked operations where appropriate.

Ensure all `Interlocked*64` targets are naturally 64-bit aligned.

## Trace API

Create a small dedicated trace helper instead of scattering file-mapping code throughout FG code.

Suggested files:

```text
OptiScaler/diagnostics/XeFGTrace.h
OptiScaler/diagnostics/XeFGTrace.cpp
```

If another project directory is more consistent, that is acceptable, but keep the implementation isolated.

Conceptual interface:

```cpp
namespace XeFGTrace
{
    bool Initialize();
    void Shutdown();

    void Record(EventType type,
                IUnknown* swapchainOrObject = nullptr,
                int32_t result = 0,
                uint64_t aux0 = 0,
                uint64_t aux1 = 0);

    void FlushAfterFailureBestEffort();
}
```

The actual API should avoid varargs, formatting, strings, dynamic allocations, or template complexity on the hot path.

`Record()` must be `noexcept` or otherwise guaranteed not to propagate exceptions into Present/Resize.

If the trace is not initialized, `Record()` must return immediately.

## Event types to record

Use a compact enum with stable numeric values. Do not encode event names into each file record.

At minimum include the following categories.

### Present / Present1 entry and skip decisions

Record at the very beginning of `hkFGPresent` and `hkFGPresent1`, **before** checking `_skipPresent` / `_skipPresent1`.

Required events should distinguish:

```text
PresentEnter
Present1Enter
PresentInternalBypass
Present1InternalBypass
PresentDispatchBegin
PresentDispatchEnd
DxgiPresentBegin
DxgiPresentEnd
DxgiPresent1Begin
DxgiPresent1End
```

The entry records are critical because they must capture the skip flags as they existed when the thread arrived.

### PR #18 mutex behavior

Around the `FGUseMutexForSwapchain` / owner-tag `2` path record:

```text
PresentMutexDecision
PresentMutexWaitBegin
PresentMutexAcquired
PresentMutexSameThreadBypass
PresentMutexUnlock
```

For these events capture both:

- logical owner tag
- actual owner thread ID

PR #18 currently keeps `ownerThread` private. For this diagnostic branch it is acceptable to add a read-only accessor such as:

```cpp
DWORD getOwnerThread() const;
```

Do not change `OwnedMutex` locking semantics beyond PR #18.

### Resize / Resize1

At the very beginning of both hooks, before skip checks:

```text
ResizeEnter
Resize1Enter
ResizeInternalBypass
Resize1InternalBypass
ResizeDxgiBegin
ResizeDxgiEnd
Resize1DxgiBegin
Resize1DxgiEnd
```

The record must snapshot all four `_skip*` flags.

### Resize fence lifecycle

Record around each current use of the shared resize fence in:

- `CreateSwapChain`
- `CreateSwapChainForHwnd`
- `hkResizeBuffers`
- `hkResizeBuffers1`
- `hkFGRelease`

Required events:

```text
FenceSignalBegin
FenceSignalEnd
FenceWaitBegin
FenceWaitEnd
FenceRecreateBegin
FenceRecreateEnd
```

Capture at least:

- `resizeFence` pointer
- `resizeFenceEvent` handle value
- `resizeFenceValue`
- current command queue pointer where relevant
- `Signal()` HRESULT/result where available
- `WaitForSingleObject` result for wait completion

Do not add synchronization to the fence state in this diagnostic PR.

### Swapchain lifecycle

Record key transitions:

```text
SwapchainCreateBegin
SwapchainCreateEnd
SwapchainCreate1Begin
SwapchainCreate1End
SwapchainReleaseEnter
SwapchainFinalProxyRelease
SwapchainReleaseLifecycleBegin
SwapchainReleaseLifecycleEnd
```

Capture current/new/old swapchain pointer values where practical through the generic pointer/aux fields.

### XeFG SDK calls

Add trace records in `XeFG_Dx12.cpp` around the important SDK calls/results, especially calls capable of changing or invalidating lifecycle state.

At minimum cover:

```text
XeFGCreateContextResult
XeFGInitSwapchainResult
XeFGGetSwapchainPtrResult
XeFGDestroyBegin
XeFGDestroyResult
XeFGSetEnabledResult
XeFGTagFrameConstantsResult
XeFGSetPresentIdResult
XeFGTagFrameResourceResult
```

Also include low-frequency state-changing calls such as `SetNumInterpolatedFrames` / UI composition if they already have an accessible result.

Do not add a trace record for every harmless getter if it provides no diagnostic value.

For `xefg_swapchain_result_t`, store the raw signed `int32_t` value.

Do not reinterpret positive warning values as success in the trace layer. The trace is observational: store the exact raw result returned by the SDK.

## Flags snapshot

Each relevant Present/Resize entry record should snapshot the four process-global skip flags into a bitmask without changing them.

Example:

```cpp
enum TraceFlagBits : uint32_t
{
    SkipPresent  = 1u << 0,
    SkipPresent1 = 1u << 1,
    SkipResize   = 1u << 2,
    SkipResize1  = 1u << 3,
    FgActive     = 1u << 4,
    FgPaused     = 1u << 5,
};
```

Do not make the existing `_skip*` variables atomic or `thread_local` in this PR.

The whole point of this build is to observe their current behavior before deciding how to fix it.

## Timestamping

Use `QueryPerformanceCounter` and store raw QPC ticks in each record.

Store `QueryPerformanceFrequency` once in the header.

Do not format wall-clock timestamps on the hot path.

The decoder can convert QPC deltas into microseconds/milliseconds offline.

## File flushing policy

### Forbidden

Do not flush on every event or every frame.

Do not periodically flush from Present.

Do not use `FILE_FLAG_WRITE_THROUGH` for hot-path durability if it materially turns mapped writes into synchronous disk I/O.

### Required / allowed

- Flush header/mapping once after initialization if useful.
- On clean shutdown, perform a best-effort `FlushViewOfFile` / `FlushFileBuffers` before unmapping/closing.
- When a **real failure is already observed**, a one-time best-effort flush is allowed.

A real failure trigger means, for example:

```cpp
FAILED(dxgiResult)
```

or:

```cpp
static_cast<int32_t>(xefgResult) < 0
```

Use a one-shot interlocked latch so the failure flush happens at most once per process.

Do not flush for every positive XeFG warning because warnings may be recoverable and could become another timing perturbation.

Do not install a new VEH/SEH crash handler solely to flush the trace in this first diagnostic PR. That would add another behavioral variable. Rely on the mapped file plus normal process teardown/cache behavior, with the one-time post-failure flush when a failure return is actually observed.

## Existing logging must remain unchanged

Do not delete existing normal logs in this diagnostic branch.

However, the tester will run with normal OptiScaler file logging disabled, and the new trace path must not call those logs internally.

Do not add extra `LOG_DEBUG` / `LOG_TRACE` lines as part of this diagnostic work.

The diagnostic must still function when standard OptiScaler logging is OFF.

## Offline decoder

Add a small decoder under a tools/scripts location, for example:

```text
tools/decode_xefg_trace.py
```

Use only the Python standard library.

The decoder must:

1. Validate magic/version/record size.
2. Read the header.
3. Find committed records.
4. Reconstruct chronological order from sequence values, including ring wrap.
5. Ignore torn/uncommitted slots.
6. Convert QPC delta to milliseconds.
7. Decode event IDs to readable names.
8. Decode the `_skip*` bitmask.
9. Print or emit CSV/TSV containing at least:
   - sequence
   - time delta
   - thread ID
   - event name
   - swapchain/object/context pointers
   - mutex owner tag
   - mutex owner thread
   - fence value
   - raw result / HRESULT
   - flag snapshot
   - aux values

Do not require the tester to run the decoder. The tester only needs to send the `.bin` file. The decoder exists for developer analysis.

Document the exact binary structs / enum mapping in code comments so a trace can still be decoded manually if needed.

## Expected files to change

Likely:

```text
OptiScaler/diagnostics/XeFGTrace.h
OptiScaler/diagnostics/XeFGTrace.cpp
OptiScaler/hooks/FG_Hooks.cpp
OptiScaler/hooks/FG_Hooks.h                  (only if a trace helper needs access; avoid if unnecessary)
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/OwnedMutex.h                      (read-only owner-thread accessor only, if needed)
OptiScaler/OptiScaler.vcxproj                (if new .cpp/.h files require explicit project entries)
OptiScaler/OptiScaler.vcxproj.filters        (if maintained by the repository)
tools/decode_xefg_trace.py
```

Keep unrelated files out of the PR.

## Explicit non-goals

Do **not** do any of the following in this diagnostic PR:

- merge PR #18
- modify PR #18 branch directly
- convert `_skipPresent` / `_skipPresent1` to `thread_local`
- convert `_skipResize` / `_skipResize1` to `thread_local`
- convert `_skip*` to atomics as a speculative fix
- add a mutex around `_skip*`
- redesign `OwnedMutex`
- add Present/Resize serialization beyond the existing PR #18 behavior
- protect `resizeFence` with a new mutex
- replace or redesign `resizeFenceValue`
- change fence/event creation or release behavior except to observe it
- alter XeFG result handling / success policy
- change swapchain destruction/recreation policy
- change REF compatibility logic
- add REFramework changes
- add Capcom Patcher changes
- add sleeps, yields, delays, retry loops, or polling
- add a crash handler/VEH solely for this trace
- perform general refactoring or formatting cleanup

If a clear bug is discovered while implementing the trace, document it in the diagnostic PR description but do not fix it in the same PR unless the trace implementation itself cannot be made correct without doing so.

## Implementation quality constraints

### Trace must be observational

Trace code must not:

- AddRef/Release traced COM pointers.
- dereference a pointer only for the sake of tracing.
- extend object lifetime.
- hold existing application locks for longer than necessary.
- introduce new ordering between FG threads.

Read already-available values and serialize the numeric snapshots only.

### Hot-path complexity

A normal event record should be approximately:

```text
one sequence allocation
+ one QPC read
+ GetCurrentThreadId
+ fixed scalar/pointer stores
+ one commit store
```

No disk API call should occur for a normal Present/Resize event after initialization.

### Failure isolation

If mapping creation fails, tracing should disable itself and OptiScaler should continue with normal behavior.

No trace failure may cause game startup failure, XeFG disablement, or a new exception.

### Binary compatibility

Use fixed-width types and `static_assert` the expected sizes/alignment.

Bump trace format `version` if the record layout changes later.

## Validation

### Build

Build the normal x64 Release configuration used by the fork.

Requirements:

- no compile errors
- no new changed-code warnings that indicate unsafe packing/alignment/conversion
- `git diff --check` passes
- decoder script parses a synthetic trace or a trace produced by a short test run

### Trace creation smoke test

With the diagnostic build:

1. Launch a XeFG game.
2. Confirm `OptiScaler_XeFGTrace.bin` appears automatically without adding/changing an INI key.
3. Confirm the file remains fixed-size.
4. Confirm the file contains valid header/records.
5. Confirm the decoder orders the events correctly.
6. Exit normally and confirm the file remains readable.

### Primary MHW tester test

Use:

- Monster Hunter Wilds
- Intel GPU
- XeFG output
- diagnostic OptiScaler branch stacked on PR #18
- fork REFramework
- OptiScaler normal logging OFF
- REFramework debug / XeFG diagnostics OFF

No new INI option is required.

Tester action:

```text
Run the game normally until the crash reproduces.
After the crash, do not change settings.
Send:
  1. OptiScaler_XeFGTrace.bin
  2. re2_framework_log.txt (normal/default log)
```

If the tester accidentally relaunches once, also collect `OptiScaler_XeFGTrace.prev.bin`.

## What to look for in the first crash trace

### A. Cross-thread skip contamination

Look for a pattern such as:

```text
T100: PresentEnter       skipPresent1=0
T100: ... sets counterpart skip state ...
T200: Present1Enter      skipPresent1=1
T200: Present1InternalBypass
```

where the skip flag was set by another thread, not by same-thread nested forwarding.

Equivalent checks apply to `_skipResize` / `_skipResize1`.

If confirmed, implement the fix in a **separate** PR after deciding whether thread-local state is valid for the XeFG proxy call topology or whether a stronger reentrancy token/design is required.

### B. Present serialization after PR #18

Confirm that when:

```text
mutexOwner = 2
mutexOwnerThread = Thread A
```

and Thread B enters Present, Thread B records `PresentMutexWaitBegin` and does not enter the protected FG Present work until `PresentMutexAcquired` after Thread A releases it.

If this fails, PR #18 itself needs re-review.

### C. Present/Resize overlap

Use QPC ordering to identify whether Resize/Resize1/final release begins while another thread is inside the FG Present / underlying Present interval.

Do not infer overlap only from thread IDs; use begin/end sequence and timestamps.

### D. Resize fence race candidate

Look for interleaved events where different threads:

- increment / use the same `resizeFenceValue`
- signal the same fence
- arm the same event
- recreate/release the fence/event while another path is waiting/using it

If confirmed, create a separate synchronization-fix work order / PR. Do not patch the fence in this diagnostic PR.

### E. Actual failure return

Search backward from the final committed record for:

- `FAILED(HRESULT)` from Present/Present1/ResizeBuffers/ResizeBuffers1
- negative `xefg_swapchain_result_t`
- Destroy/recreate failure
- final proxy-release transaction
- lifecycle state transition immediately preceding the fatal sequence

The raw result value is more important than formatted error text.

## Result policy

The diagnostic PR is successful even if it does not fix the crash.

Its success criterion is that a logging-OFF crash produces enough low-perturbation data to choose the next isolated fix based on evidence.

Do not convert an observed correlation directly into a broad synchronization rewrite. Prefer one small follow-up PR per confirmed issue.

## Acceptance criteria

This work is complete when all of the following are true:

1. A separate diagnostic branch is created from the latest PR #18 head.
2. PR #18 itself remains unchanged and unmerged.
3. No new INI/config option is required.
4. `OptiScaler_XeFGTrace.bin` is automatically created for the diagnostic run.
5. Previous trace preservation via `.prev.bin` is implemented or an equally simple one-session safety mechanism is provided.
6. Trace storage uses a fixed-size memory-mapped ring buffer.
7. Normal hot-path records perform no synchronous file write/flush, no normal logger call, no formatting, no heap allocation, and no trace mutex locking.
8. Records contain sequence, QPC, thread ID, event type, skip-flag snapshot, swapchain/context/object pointer values, mutex owner information, fence value, raw result, and auxiliary values sufficient for correlation.
9. Torn/incomplete records are detectable via a commit protocol.
10. Present, Present1, ResizeBuffers, ResizeBuffers1, mutex owner-tag `2`, resize-fence lifecycle, swapchain release/recreate, and key XeFG SDK results are traced.
11. One-time post-failure flushing is allowed only after a real failure return and is not executed on ordinary frames.
12. No `_skip*`, fence, XeFG lifecycle, or presentation behavior is changed by the diagnostic instrumentation.
13. A standard-library-only offline decoder can reconstruct and print the ring in chronological order.
14. Release x64 build succeeds and `git diff --check` passes.
15. The tester can reproduce with normal OptiScaler logging OFF and collect the trace by sending one automatically generated `.bin` file plus the normal REF log.

## Suggested PR title

`Add low-overhead automatic XeFG crash trace`

Suggested PR description emphasis:

> This is a temporary diagnostic PR stacked on PR #18. It does not attempt to fix the remaining MHW crash. It adds an automatically created, fixed-size memory-mapped XeFG lifecycle trace so logging-OFF crashes can be analyzed without introducing the synchronous file logging that currently changes reproduction behavior.
