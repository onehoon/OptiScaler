# MHW XeFG PR19 Zero-Config Sparse Text Diagnostic Logging Work Order

Date: 2026-09-13

## Purpose

Extend the existing temporary diagnostic PR #19 (`diag/mhw-xefg-auto-ring-trace`) with a **zero-config, low-volume human-readable XeFG diagnostic log mode** intended for Monster Hunter Wilds (MHW) and REFramework/XeFG lifecycle testing.

This work is specifically for the current PR19 investigation. General-user defaults and backwards compatibility are **not** the priority for this temporary diagnostic branch.

The target runtime defaults are intentionally:

```ini
[Log]
LogToFile=true
LogLevel=7
LogAsync=false
```

The tester should not need to edit `OptiScaler.ini` before every build/run.

PR19's existing binary ring trace remains the authoritative high-frequency diagnostic source. The new text mode exists only to provide a small, immediately readable lifecycle timeline around startup, swapchain recreation, final proxy release, teardown, and failures.

Do **not** merge PR #19 as part of this work.

---

## PR / baseline reviewed

Implement against the current PR19 branch:

```text
PR:      #19 Add low-overhead automatic XeFG crash trace
Branch:  diag/mhw-xefg-auto-ring-trace
Head:    46b2987ea401971f2a8fbe20ad80a979a89b62cb
Base:    fix/fg-present-thread-aware-ownedmutex
```

The current PR already contains:

- automatic `OptiScaler_XeFGTrace.bin` ring tracing,
- Present / Present1 tracing,
- ResizeBuffers / ResizeBuffers1 tracing,
- D3D12 helper/fence/command-list tracing,
- FSRFG activation-path tracing,
- XeFG create/destroy tracing,
- first-negative-result / `E_ABORT` primary-failure capture,
- stale final-proxy identity protection experiment,
- decoder and synthetic self-test support.

This work must **not** duplicate the ring trace with verbose text logging.

---

## Current logging implementation findings

### 1. Current defaults do not create the desired diagnostic file

`OptiScaler/Config.h` currently defines logging defaults equivalent to:

```cpp
CustomOptional<bool> LogToFile { false };
CustomOptional<int> LogLevel { 0 };
CustomOptional<bool> LogAsync { false };
```

For PR19 testing, change the code defaults to:

```cpp
CustomOptional<bool> LogToFile { true };
CustomOptional<int> LogLevel { 7 };
CustomOptional<bool> LogAsync { false };
```

These code defaults are required so an existing INI containing `auto`, or an INI that omits those keys, still enters the PR19 diagnostic mode without manual editing.

### 2. `LogLevel` is currently cast directly to `spdlog::level_enum`

`OptiScaler/Logger.cpp` currently does the equivalent of:

```cpp
shared_logger->set_level(
    (spdlog::level::level_enum) Config::Instance()->LogLevel.value_or_default());
```

Do **not** pass `7` through this cast.

spdlog's standard level layout is effectively:

```text
0 trace
1 debug
2 info
3 warn
4 err
5 critical
6 off
7 n_levels   // sentinel/count, not a normal runtime logging level
```

Therefore `LogLevel=7` must be treated as an **OptiScaler/PR19 custom sentinel**, not as a native spdlog severity.

### 3. Existing sink/flush behavior is acceptable only because the new text stream is sparse

The file sink currently accepts trace-level traffic and the logger uses:

```cpp
shared_logger->flush_on(spdlog::level::trace);
```

With normal trace/debug logging this can materially perturb timing because every accepted message is eligible for an immediate flush.

For the new `LogLevel=7` mode, only a very small number of diagnostic events should pass the logger, so synchronous flush behavior is desirable: if the game hangs inside `D3D12InitFromSwapChainDesc`, the last `enter` marker must already be on disk.

Do not solve this task by enabling full debug logging asynchronously. The current async logger uses a blocking overflow policy, and high-volume logging would still be capable of perturbing game-thread timing.

### 4. Existing normal macros are defined in `OptiScaler/SysUtils.h`

The project already has:

```cpp
LOG_TRACE
LOG_DEBUG
LOG_INFO
LOG_WARN
LOG_ERROR
```

There is no existing PR19-specific sparse text diagnostic macro.

Add a dedicated macro/helper with a clear name such as:

```cpp
LOG_XEFG_DIAG
```

It should write using spdlog `critical` internally so it survives the internal threshold used for custom mode 7.

Do not redefine the meaning of the existing 0-6 spdlog-compatible values.

---

## Required behavior

When no manual logging configuration is supplied, PR19 should behave as if the following were explicitly configured:

```ini
[Log]
LogToFile=true
LogLevel=7
LogAsync=false
```

`LogLevel=7` means:

> PR19 sparse XeFG lifecycle diagnostic text mode.

Internally, mode 7 should map to `spdlog::level::critical` for filtering/output purposes.

The resulting `OptiScaler.log` should contain only a compact lifecycle/failure timeline plus any unavoidable startup banner for this mode.

It must **not** become a text version of `OptiScaler_XeFGTrace.bin`.

---

## Implementation constraints

1. Implement as an additional commit on PR #19.
2. Keep PR #19 open and draft.
3. Do not merge PR #19.
4. Do not modify PR #18.
5. Do not change XeFG functional behavior, swapchain ownership, synchronization, refcounting, timeouts, or recovery behavior.
6. Do not add new ring-trace event IDs unless a real diagnostic gap is discovered during implementation. This task is primarily text-log routing.
7. Do not renumber existing ring-trace events.
8. Do not change `XeFGTrace` file layout/version for this task.
9. Do not add per-frame text logging.
10. Do not add successful Present/Present1 text lines on every frame.
11. Do not add per-frame Dispatch, SetResource, Hudless, allocator/list, queue, signal/wait, or resource-tagging text logs.
12. Keep `LogAsync=false` for the default PR19 diagnostic mode.
13. Preserve the existing binary ring trace exactly as the high-frequency source.

---

## Required file changes

Expected files include at minimum:

```text
OptiScaler/Config.h
OptiScaler/Logger.cpp
OptiScaler/SysUtils.h
OptiScaler/OptiScaler.ini
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/hooks/FG_Hooks.cpp
```

Additional source files may be touched only when a lifecycle boundary described below is implemented at a more appropriate existing call site.

Avoid broad logging refactors.

---

## Step 1 - Make PR19 diagnostic logging the code default

In `OptiScaler/Config.h`, change the logging defaults to:

```cpp
// Logging
CustomOptional<bool> LogToFile { true };
CustomOptional<bool> LogToConsole { false };
CustomOptional<bool> LogToDebug { false };
CustomOptional<bool> LogToNGX { false };
CustomOptional<bool> OpenConsole { false };
CustomOptional<bool> DebugWait { false };
CustomOptional<int> LogLevel { 7 };
CustomOptional<std::wstring> LogFileName { L"OptiScaler.log" };
CustomOptional<bool> LogSingleFile { true };
CustomOptional<bool> LogAsync { false };
CustomOptional<int> LogAsyncThreads { 4 };
```

Only the requested defaults need to change:

```text
LogToFile: false -> true
LogLevel:   0     -> 7
LogAsync:   already false; keep false explicitly
```

Do not change unrelated logging defaults.

### Why the code default matters

`Config::Reload()` uses `set_from_config()` with nullable values. If the INI says `auto`, the `CustomOptional` default is used.

Therefore the code default must be changed; editing only the sample INI is not sufficient for the requested zero-config behavior.

---

## Step 2 - Update the PR19 INI template as well

In `OptiScaler.ini`, make the PR19 intent explicit:

```ini
[Log]
LogToFile=true
LogLevel=7
LogAsync=false
```

Update the adjacent comments to document this temporary PR19 meaning.

Suggested text:

```ini
; PR19 diagnostic build default
; 0 = Trace / 1 = Debug / 2 = Info / 3 = Warning / 4 = Error
; 5 = Critical / 6 = Off
; 7 = PR19 sparse XeFG lifecycle diagnostics
LogLevel=7
```

This is intentionally diagnostic-branch-specific.

Do not present `7` as a native spdlog level.

---

## Step 3 - Intercept custom level 7 before calling spdlog

In `PrepareLogger()` in `OptiScaler/Logger.cpp`, special-case `7`.

Preferred shape:

```cpp
namespace
{
constexpr int kXeFGDiagnosticLogLevel = 7;
}

...

const int configuredLevel = Config::Instance()->LogLevel.value_or_default();
const bool xefgDiagnosticMode = configuredLevel == kXeFGDiagnosticLogLevel;

const auto spdlogLevel =
    xefgDiagnosticMode
        ? spdlog::level::critical
        : static_cast<spdlog::level::level_enum>(configuredLevel);

shared_logger->set_level(spdlogLevel);
```

The exact local naming may differ.

### Mandatory rule

Never do this when the configured value is 7:

```cpp
static_cast<spdlog::level::level_enum>(7)
```

`7` is the enum-count/sentinel area, not a valid ordinary severity.

### Flush behavior

For mode 7, either of these is acceptable:

```cpp
shared_logger->flush_on(spdlog::level::critical);
```

or keeping the existing global:

```cpp
shared_logger->flush_on(spdlog::level::trace);
```

provided only sparse `critical` diagnostic messages pass in mode 7.

Prefer making the intent explicit:

```cpp
if (xefgDiagnosticMode)
    shared_logger->flush_on(spdlog::level::critical);
else
    shared_logger->flush_on(spdlog::level::trace);
```

This does not change the desired durability: each sparse diagnostic boundary is flushed immediately.

Do not switch mode 7 to async logging.

---

## Step 4 - Add one dedicated sparse diagnostic macro

Add a dedicated macro in the existing logging macro area in `OptiScaler/SysUtils.h`.

Suggested shape:

```cpp
#define LOG_XEFG_DIAG(msg, ...) \
    spdlog::critical("[XeFGDiag] " __FUNCTION__ " " msg, ##__VA_ARGS__)
```

Keep the prefix stable:

```text
[XeFGDiag]
```

The severity is only an internal transport/filter mechanism. The prefix identifies the semantic channel.

Do not replace existing `LOG_INFO`, `LOG_WARN`, or `LOG_ERROR` calls globally.

Do not perform broad logging cleanup in this commit.

---

## Step 5 - Add a one-line mode banner

After the logger is fully installed as the default logger, emit one sparse marker when custom mode 7 is active.

Example:

```cpp
if (xefgDiagnosticMode)
{
    LOG_XEFG_DIAG("mode=enabled log_level=7 async=false");
}
```

If calling the macro directly from `Logger.cpp` is awkward because of macro/include ordering, an equivalent direct `spdlog::critical()` line is acceptable.

This marker proves that:

- PR19 mode 7 was recognized,
- the file sink is active,
- `7` was not accidentally treated as spdlog `off`/invalid,
- the expected build is running.

---

## Sparse text instrumentation policy

### Text log responsibilities

The text log answers:

- Did XeFG context/swapchain initialization begin?
- Which initialization boundary returned?
- Was the XeFG proxy swapchain published/bound?
- Did a resize/recreation occur?
- Did a final proxy release begin?
- Was that final proxy stale or current?
- Did lifecycle teardown begin and return?
- What was the first obvious failure/result visible at a major boundary?

### Binary ring trace responsibilities

The existing PR19 ring trace continues to answer:

- per-frame Present / Present1 ordering,
- Dispatch ordering,
- Hudless/resource tagging,
- command allocator/list state,
- queue submission,
- fence signal/wait,
- fine-grained FSRFG state evaluation,
- high-frequency hook timing,
- primary failure-source event correlation.

Do not duplicate those high-frequency records in text.

---

## Step 6 - XeFG context creation text boundaries

In `XeFG_Dx12::CreateSwapchainContext()` add sparse result lines around the important one-time SDK operations.

Required information:

### `D3D12CreateContext`

After it returns:

```text
[XeFGDiag] ... api=D3D12CreateContext result=<raw> context=<ptr> device=<ptr>
```

### `SetLatencyReduction`

After it returns when called:

```text
[XeFGDiag] ... api=SetLatencyReduction result=<raw> context=<ptr> xell=<ptr>
```

`SetLoggingCallback` may be logged if convenient, but it is lower priority than context creation and latency binding.

Do not dump the XeFG SDK debug callback messages into mode 7.

The existing XeFG SDK logging callback can remain at its current normal levels; those messages will be filtered out by mode 7.

---

## Step 7 - Critical startup boundary: `D3D12InitFromSwapChainDesc`

This is the highest-priority new text boundary because the latest MHW launch-failure evidence stops after REFramework reports XeFG init `pre_init`.

Instrument both:

```text
XeFG_Dx12::CreateSwapchain(...)
XeFG_Dx12::CreateSwapchain1(...)
```

immediately before and immediately after:

```cpp
XeFGProxy::D3D12InitFromSwapChainDesc()(...)
```

### Before call

Log one line with enough identity to compare MHW against Pragmata:

```text
[XeFGDiag] api=InitFromSwapChainDesc stage=enter
  context=<ptr>
  hwnd=<ptr>
  queue=<ptr>
  factory=<ptr>
  width=<n>
  height=<n>
  format=<n>
  buffer_count=<n>
  swap_effect=<n>
  flags=<hex>
  init_flags=<hex>
  max_interpolated_frames=<n>
```

For the legacy `DXGI_SWAP_CHAIN_DESC` path also include `Windowed` when available.

For the `DESC1` path include fullscreen/windowed information from `pFullscreenDesc` when available.

Keep this to one line if practical.

### After call

Immediately after the SDK call returns, before unrelated work:

```text
[XeFGDiag] api=InitFromSwapChainDesc stage=return context=<ptr> result=<name> raw=<int>
```

This pair is intentionally synchronously flushed in mode 7.

If MHW hangs inside the SDK call, the log must end with `stage=enter` and no matching `stage=return`.

That is a successful diagnostic outcome.

---

## Step 8 - `D3D12GetSwapChainPtr` and lifecycle publication

After `D3D12GetSwapChainPtr` returns, log:

```text
[XeFGDiag] api=D3D12GetSwapChainPtr result=<raw> proxy=<ptr> context=<ptr>
```

After the successful local lifecycle assignment:

```cpp
_gameCommandQueue = realQueue;
_swapChain = *swapChain;
_hwnd = hwnd;
```

emit one publication line:

```text
[XeFGDiag] action=xefg_lifecycle_local_published proxy=<ptr> context=<ptr> queue=<ptr> hwnd=<ptr>
```

Also add/retain a sparse text marker at the public tracking publication point in `FGHooks::SetFGSwapchain()` so the log can distinguish:

```text
XeFG internal lifecycle became ready
```

from:

```text
State::currentFGSwapchain/currentSwapchain was published to the hook layer
```

Suggested public marker:

```text
[XeFGDiag] action=fg_swapchain_bound proxy=<ptr> hwnd=<ptr> current_fg=<ptr>
```

Do not AddRef or alter ownership for logging.

---

## Step 9 - Resize logging: boundaries only

Add sparse lines to the FG swapchain hook paths for:

```text
ResizeBuffers
ResizeBuffers1
```

Log only the external boundary and return result.

Suggested enter line:

```text
[XeFGDiag] api=ResizeBuffers stage=enter proxy=<ptr> count=<n> width=<n> height=<n> format=<n> flags=<hex>
```

Suggested return line:

```text
[XeFGDiag] api=ResizeBuffers stage=return proxy=<ptr> hr=0xXXXXXXXX
```

Do the equivalent for `ResizeBuffers1`.

Do not text-log every internal fence/list/resource step involved in resize; the ring trace already covers them.

Resize frequency is low enough for text diagnostics and is valuable for Alt+Tab/fullscreen/borderless lifecycle correlation.

---

## Step 10 - Present policy: failures only

Do **not** add successful Present/Present1 text lines.

If a hook or XeFG SDK Present boundary returns a failing raw result, emit one `LOG_XEFG_DIAG` line with:

```text
api
proxy/context
raw HRESULT/XeFG result
thread if readily available
```

Example:

```text
[XeFGDiag] FAILURE api=Present1 proxy=0x... hr=0x80004004
```

This is especially important for `E_ABORT (0x80004004)`.

Do not manually flush here beyond the mode-7 logger's normal critical flush behavior.

The binary trace remains responsible for identifying the exact preceding high-frequency event.

---

## Step 11 - Preserve non-success XeFG SDK results in mode 7

`XeFG_Dx12.cpp` currently centralizes non-success SDK reporting in `LogXeFGResult()`.

Existing `LOG_WARN` / `LOG_ERROR` output will be filtered out when the logger threshold is internally mapped to `critical`.

Therefore extend `LogXeFGResult()` so every non-success XeFG result also emits one sparse diagnostic line, for example:

```cpp
LOG_XEFG_DIAG("FAILURE api={} result={} raw={}",
              apiName,
              magic_enum::enum_name(result),
              static_cast<int32_t>(result));
```

Keep the existing warning/error logs unchanged for lower logging levels.

Do not emit diagnostic text for successful SDK calls from this generic helper; success boundaries should only be logged at the major lifecycle points explicitly listed in this work order.

---

## Step 12 - Final proxy release / stale identity experiment

PR19 head `46b2987...` already contains the stale final-proxy identity experiment.

Add sparse visibility without changing its behavior.

### In `FGHooks::hkFGRelease()`

Do not log every ordinary `Release()`.

Only log when the code reaches the final-proxy lifecycle path, i.e. the path that invokes:

```cpp
ReleaseSwapchainFromFinalProxyRelease(...)
```

Suggested marker:

```text
[XeFGDiag] action=final_proxy_release_candidate proxy=<ptr> hwnd=<ptr> current_fg_proxy=<ptr>
```

### In `XeFG_Dx12::ReleaseSwapchainFromFinalProxyRelease()`

After acquiring/revalidating the lifecycle state, log exactly one of the following semantic outcomes.

Stale case:

```text
[XeFGDiag] action=stale_final_proxy_release_only final=<ptr> current=<ptr> context=<ptr>
```

Current-lifecycle case:

```text
[XeFGDiag] action=current_final_proxy_release final=<ptr> current=<ptr> context=<ptr>
```

After the normal current-lifecycle teardown path returns:

```text
[XeFGDiag] action=current_final_proxy_release_complete final=<ptr> success=<0|1>
```

Do not move the identity check, lock boundary, callback, or alias clearing just to make logging easier.

The stale-identity experiment must remain behaviorally identical to the reviewed PR19 head.

---

## Step 13 - Teardown boundaries

Add sparse enter/return lines around the major lifecycle teardown functions already traced by PR19:

```text
ReleaseSwapchainLocked
DestroyFGContext
DestroySwapchainContext
XeFGProxy::Destroy
```

### Required fields

Where available include:

```text
proxy
_swapChain
_swapChainContext
_fgContext
hwnd
isShuttingDown
_swapchainRecreationBlocked
raw result
success/failure
```

### Most important pair

Immediately before:

```cpp
XeFGProxy::Destroy()(context)
```

log:

```text
[XeFGDiag] api=XeFGDestroy stage=enter context=<ptr>
```

Immediately after it returns:

```text
[XeFGDiag] api=XeFGDestroy stage=return context=<ptr> result=<name> raw=<int>
```

This provides the same useful human-readable hang boundary as the init pair.

If the SDK destroy hangs, the final text line should be the `stage=enter` marker.

---

## Step 14 - Activation state transitions only

A small number of activation/deactivation transition lines are useful.

In `XeFG_Dx12::Activate()`, when the code actually calls:

```cpp
XeFGProxy::SetEnabled()(..., true)
```

log the returned result once per transition attempt:

```text
[XeFGDiag] action=set_enabled enabled=1 context=<ptr> result=<raw>
```

If `Deactivate()` calls `SetEnabled(false)`, log the equivalent transition.

Do not log `EvaluateState()` every frame.

Do not log the FSRFG activation decision every frame.

Those are already covered by the ring trace.

---

## Explicit text events to include

The final sparse text stream should cover approximately this semantic set:

```text
PR19 diagnostic mode enabled
XeFG create-context result
XeLL / latency-reduction bind result when applicable
InitFromSwapChainDesc enter
InitFromSwapChainDesc return
GetSwapChainPtr return
XeFG local lifecycle publish
FG hook-layer swapchain bind/publication
SetEnabled true/false transition results
ResizeBuffers enter/return
ResizeBuffers1 enter/return
Present/Present1 FAILURE only
non-success XeFG SDK result
final proxy release candidate
stale final proxy release only
current final proxy release
ReleaseSwapchainLocked enter/return
DestroyFGContext enter/return
DestroySwapchainContext enter/return
XeFG Destroy enter/return
```

The exact wording may differ, but keep the `[XeFGDiag]` prefix and stable `api=`, `action=`, `stage=`, `result=` style fields wherever practical.

---

## Events that must NOT be added to the text log

Do not emit a text line for every:

```text
Present
Present1
Dispatch
SetResource
TagFrameResource
Hudless lookup
StartNewFrame
EvaluateState
allocator Reset
command-list Reset
command-list Close
ExecuteCommandLists
queue Signal
fence wait
resource state transition
GetBuffer
frame counter
FG slot/index update
```

These belong in `OptiScaler_XeFGTrace.bin`.

A normal several-minute run should not produce thousands of `[XeFGDiag]` lines simply because the game renders thousands of frames.

---

## Recommended output example: normal Pragmata DLSSG -> XeFG startup

A healthy run should look conceptually like:

```text
[C] [XeFGDiag] PrepareLogger mode=enabled log_level=7 async=false
[C] [XeFGDiag] CreateSwapchainContext api=D3D12CreateContext result=0 context=0x... device=0x...
[C] [XeFGDiag] CreateSwapchain1 api=InitFromSwapChainDesc stage=enter context=0x... hwnd=0x... queue=0x... width=... height=...
[C] [XeFGDiag] CreateSwapchain1 api=InitFromSwapChainDesc stage=return context=0x... result=SUCCESS raw=0
[C] [XeFGDiag] CreateSwapchain1 api=D3D12GetSwapChainPtr result=0 proxy=0x... context=0x...
[C] [XeFGDiag] CreateSwapchain1 action=xefg_lifecycle_local_published proxy=0x... context=0x... queue=0x... hwnd=0x...
[C] [XeFGDiag] SetFGSwapchain action=fg_swapchain_bound proxy=0x... hwnd=0x...
[C] [XeFGDiag] Activate action=set_enabled enabled=1 context=0x... result=0
```

Additional resize lines are expected when the game changes mode/resolution.

No successful per-frame Present spam should appear.

---

## Recommended output example: current MHW startup hang

If MHW again blocks inside the first XeFG initialization call, the diagnostic file should end near:

```text
[C] [XeFGDiag] CreateSwapchain1 api=InitFromSwapChainDesc stage=enter context=0x... hwnd=0x... queue=0x... width=... height=...
```

with no corresponding:

```text
stage=return
```

That directly confirms the same boundary currently inferred from the REFramework log without requiring full OptiScaler debug output.

---

## Recommended output example: stale final proxy race

If the PR19 stale guard is exercised:

```text
[C] [XeFGDiag] hkFGRelease action=final_proxy_release_candidate proxy=0xAAA current_fg_proxy=0xBBB
[C] [XeFGDiag] ReleaseSwapchainFromFinalProxyRelease action=stale_final_proxy_release_only final=0xAAA current=0xBBB context=0x...
```

There must be no teardown of the current `0xBBB` lifecycle caused by the stale `0xAAA` line.

The text log is observational only; the existing PR19 guard behavior must remain unchanged.

---

## MHW / Pragmata validation matrix

### A. Zero-config logging validation

Use a PR19 build with no manual changes to `[Log]`.

Accept either:

- keys omitted, or
- repository INI values as committed by this work order.

Verify:

```text
OptiScaler.log is created automatically
PR19 mode banner exists
LogLevel=7 is recognized
normal trace/debug/info messages do not flood the file
LogAsync is false
```

### B. Pragmata DLSSG -> XeFG smoke test

Pragmata is currently a known-good reference for the master-derived XeFG lifecycle.

Verify:

- game launches,
- XeFG initializes successfully,
- `InitFromSwapChainDesc enter` has matching `return`,
- `GetSwapChainPtr` succeeds,
- lifecycle publication occurs,
- FG activation occurs,
- resizes return successfully,
- no crash/regression is introduced by the sparse text logger.

The text log should stay small during normal gameplay.

### C. MHW DLSSG -> XeFG launch test

Reproduce the current MHW launch behavior.

Determine the last text boundary.

Highest-value outcome:

```text
InitFromSwapChainDesc stage=enter
<no return>
```

If init returns successfully, continue using later sparse boundaries to identify the next stop.

### D. MHW later-crash / E_ABORT test

If the game reaches gameplay, reproduce the original later failure.

Verify that:

- no successful Present spam exists,
- the first failing Present/XeFG API has one readable `[XeFGDiag] FAILURE` line,
- `0x80004004` is visible if E_ABORT occurs,
- the binary ring trace remains available for the detailed preceding event sequence.

### E. Final proxy stale-identity test

If event 109 / stale final proxy behavior occurs, verify the text stream clearly distinguishes:

```text
final proxy candidate
stale-only release
current lifecycle teardown
```

Do not infer causality from timing alone; correlate with the binary ring trace.

---

## Log-volume acceptance criteria

The purpose of mode 7 is to avoid the timing perturbation observed with full OptiScaler debug logging.

Acceptance criteria:

1. No per-frame successful Present text logging.
2. No per-frame Dispatch/resource text logging.
3. A normal five-minute Pragmata session should produce a compact lifecycle-oriented file, not a multi-megabyte debug stream.
4. Repeated resizes may add lines, but frame rendering alone must not continuously grow the text log.
5. The binary ring trace remains the only high-frequency recorder.

Do not optimize for an exact line-count threshold if the game legitimately performs many swapchain recreations; optimize for event semantics.

---

## Build / static validation

Before handing the build to the tester:

```text
1. Build x64 Release OptiScaler.vcxproj.
2. Run clang-format on touched C/C++ files.
3. Run git diff --check.
4. Run python tools/decode_xefg_trace.py --self-test.
5. Run python -m py_compile tools/decode_xefg_trace.py.
```

The decoder should not require changes unless ring-trace events were intentionally added, which this work order does not expect.

Also verify that the generated DLL is the PR19 build intended for local testing.

---

## Code-review checklist

Before considering the work ready for runtime testing, verify all of the following:

- [ ] `LogToFile` code default is `true`.
- [ ] `LogLevel` code default is `7`.
- [ ] `LogAsync` code default is `false`.
- [ ] sample `OptiScaler.ini` requires no manual edit for PR19 mode.
- [ ] `7` is intercepted before conversion to `spdlog::level_enum`.
- [ ] mode 7 maps internally to `critical`.
- [ ] one stable `[XeFGDiag]` macro/helper exists.
- [ ] normal 0-6 values are not redefined.
- [ ] successful Present/Present1 are not text-logged per frame.
- [ ] `InitFromSwapChainDesc` has an immediately-before marker.
- [ ] `InitFromSwapChainDesc` has an immediately-after marker.
- [ ] XeFG Destroy has an immediately-before marker.
- [ ] XeFG Destroy has an immediately-after marker.
- [ ] `D3D12GetSwapChainPtr` result is text-visible.
- [ ] local and public swapchain publication are distinguishable.
- [ ] ResizeBuffers/ResizeBuffers1 have only sparse boundary logging.
- [ ] final proxy stale/current outcomes are text-visible.
- [ ] non-success XeFG SDK results remain visible despite mode-7 thresholding.
- [ ] E_ABORT can appear in sparse text if returned at a logged major boundary.
- [ ] ring trace behavior/layout is unchanged.
- [ ] stale final-proxy guard behavior is unchanged.
- [ ] no ownership/refcount/synchronization behavior changed for logging convenience.
- [ ] x64 Release build passes.
- [ ] decoder self-test passes.
- [ ] `git diff --check` passes.

---

## Non-goals

Do not use this task to:

- fix the MHW startup hang,
- fix E_ABORT,
- add wrapper-generation protection,
- modify the stale final-proxy experiment,
- alter swapchain publication ordering,
- alter COM ownership,
- redesign the logger globally,
- make mode 7 a production/user-facing logging standard,
- change PR19 ring-buffer capacity or file format,
- add async diagnostics,
- add timeout/recovery behavior,
- change REFramework.

This is a **diagnostic observability change only**.

---

## Expected implementation outcome

After this change the tester should be able to copy/build PR19 and launch a game without touching the INI.

The default diagnostic artifacts should be:

```text
OptiScaler.log
    sparse, human-readable XeFG lifecycle/failure boundaries

OptiScaler_XeFGTrace.bin
    existing low-overhead high-frequency ring trace
```

The intended division of responsibility is:

```text
REFramework debug log
    -> REF renderer / hook / swapchain lifecycle context

OptiScaler LogLevel=7 sparse text log
    -> human-readable XeFG lifecycle boundaries and failures

PR19 binary ring trace
    -> detailed high-frequency timing/order evidence
```

This gives MHW testing enough human-readable context without returning to full OptiScaler trace/debug logging and its associated timing/I/O disturbance.
