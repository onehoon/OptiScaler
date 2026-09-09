# Work Order: Refine XeFG Result Handling to Use an Explicit Warning Policy

**Repository:** `onehoon/OptiScaler`  
**Target branch:** `master`  
**Baseline reviewed:** `c0391097fe11e496b5b087ba3abff10e3f672d6a`  
**Date:** 2026-09-10  
**Historical change being corrected:** `d87d8ca8c88c3c213163c829eb8aeac3e86d79b5` (`fix: honor non-negative XeFG result statuses`)

---

## 1. Goal

Refine the XeFG result handling introduced by `d87d8ca` so that OptiScaler no longer makes the blanket assumption that every positive XeFG warning should be treated as successful completion of the current operation.

The key distinction for this work is:

> **Intel's result-code classification and OptiScaler's continuation policy are two different decisions.**

Intel XeSS-FG defines:

```text
0   -> XEFG_SWAPCHAIN_RESULT_SUCCESS
> 0 -> warning / non-error status
< 0 -> error
```

Intel also documents a generic release-build helper equivalent to:

```cpp
bool Succeeded(xefg_swapchain_result_t result)
{
    return static_cast<int>(result) >= 0;
}
```

However, the same developer guide also states that normally all operations should return exact `XEFG_SWAPCHAIN_RESULT_SUCCESS`, and that warnings other than the expected `xefgSwapChainGetLastPresentStatus()` cases deserve investigation.

Therefore:

- a positive result must **not be mislabeled as an error**;
- but a positive result must also **not automatically authorize OptiScaler to advance lifecycle/state or consume output data**;
- continuation on a warning must be an explicit per-call policy, not a repository-wide `>= 0` rule.

This PR is a conservative correction of the blanket policy. It is **not** a general XeFG refactor.

Intel references:

- `intel/xess/doc/xess_fg_developer_guide_english.md` — Error Handling
- `intel/xess/samples/basic_sample_frame_generation/basic_sample.cpp` — `ThrowIfFailed(xefg_swapchain_result_t, ...)`
- `intel/xess/inc/xess_fg/xefg_swapchain.h` — warning/success/error enum values

---

## 2. Why the current behavior needs refinement

`d87d8ca` changed many checks from exact-success semantics such as:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    return false;
}
```

to sign-based semantics such as:

```cpp
if (static_cast<int32_t>(result) < 0)
{
    return false;
}
```

and changed state/output gates such as:

```cpp
if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    // consume output / advance state
}
```

to:

```cpp
if (static_cast<int32_t>(result) >= 0)
{
    // consume output / advance state
}
```

The classification is consistent with Intel's enum contract, but the control-flow consequence is too broad.

For example, the current master treats any positive result from `xefgSwapChainDestroy()` as completed destruction and allows the old context identity to be discarded. Likewise, a positive result from `xefgSwapChainSetEnabled()` can cause OptiScaler to update `_isActive`, and a positive result from `GetProperties()` / `GetVersion()` can cause returned data to be committed as if the call completed normally.

Those are application-policy decisions, not consequences that follow automatically from `result > 0` being classified as a warning.

---

## 3. Required policy

Use the following policy in this PR.

### 3.1 Classification

```text
result == SUCCESS -> normal success
result > SUCCESS  -> warning
result < SUCCESS  -> error
```

Positive values must be logged as warnings when they are relevant to a checked call. Negative values remain errors.

### 3.2 Continuation

Default rule for this PR:

> **Require exact `XEFG_SWAPCHAIN_RESULT_SUCCESS` for lifecycle completion, state transitions, validity decisions, and consumption of output parameters.**

Do not use `>= 0` as a generic success gate.

A call may continue after a warning only when the surrounding OptiScaler control flow was already intentionally non-fatal for any non-success result, or when there is a documented call-specific reason to do so.

Do **not** invent a new warning allowlist in this PR without concrete runtime evidence or API-specific Intel documentation.

### 3.3 Logging and behavior are separate

Do not implement this by calling every positive warning an `ERROR`.

Use a shape like:

```cpp
const auto resultValue = static_cast<int32_t>(result);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    if (resultValue > 0)
    {
        LOG_WARN("XeFG warning: api = {}, result = {} ({})",
                 apiName,
                 magic_enum::enum_name(result),
                 resultValue);
    }
    else
    {
        LOG_ERROR("XeFG error: api = {}, result = {} ({})",
                  apiName,
                  magic_enum::enum_name(result),
                  resultValue);
    }

    // Preserve the call-specific conservative control flow here.
}
```

The exact helper/layout may be adapted to the current code style. Keep it small; do not create a general result-handling framework.

---

## 4. Do not mechanically `git revert d87d8ca`

Implement this against current `master`.

Do **not** blindly revert the historical commit because later XeFG lifecycle/ownership work overlaps the same file and contains fixes that must remain intact.

In particular, preserve:

- PR #1 / F-02 fail-closed destroy/recreation behavior;
- PR #6 force-drain removal and wrapper ownership fixes;
- PR #13 owner-scoped swapchain cleanup boundaries;
- signed numeric lifecycle logging such as `static_cast<int32_t>(result)` where it improves diagnostics;
- all unrelated Intel Reflex / spoofing changes;
- all REFramework-side behavior.

This work changes **result-policy decisions only**.

---

## 5. Primary files

Expected code scope:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/proxies/XeFG_Proxy.h
```

Only touch additional files if a small shared diagnostic helper is demonstrably required.

Do not modify REFramework.

---

## 6. Task A — Add a minimal explicit warning classifier

Avoid helpers named or shaped in a way that implies `warning == successful completion`.

Good minimal forms include:

```cpp
static bool IsXeFGWarning(xefg_swapchain_result_t result)
{
    return static_cast<int32_t>(result) > 0;
}
```

or a tiny logging helper that receives the API name and result.

Do **not** add a generic helper like this and then use it as the control-flow gate everywhere:

```cpp
bool XeFGSucceeded(xefg_swapchain_result_t result)
{
    return static_cast<int32_t>(result) >= 0;
}
```

That recreates the policy problem this work is meant to correct.

If a helper is unnecessary, explicit local checks are acceptable and may be preferable for this narrow PR.

---

## 7. Task B — Restore strict lifecycle and initialization completion semantics

Audit the current `XeFG_Dx12.cpp` call sites changed by `d87d8ca`.

The following operations must not advance to their normal-success path merely because the result is positive:

```text
xefgSwapChainD3D12CreateContext
xefgSwapChainSetLatencyReduction
xefgSwapChainDestroy
xefgSwapChainD3D12InitFromSwapChainDesc
xefgSwapChainD3D12GetSwapChainPtr
```

### 7.1 Context creation

Current blanket behavior is effectively:

```cpp
if (static_cast<int32_t>(result) < 0)
    return false;

LOG_INFO("XeFG context created");
```

Change the success gate back to exact success.

Recommended shape:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    const auto value = static_cast<int32_t>(result);

    if (value > 0)
        LOG_WARN("D3D12CreateContext warning: {} ({})", magic_enum::enum_name(result), value);
    else
        LOG_ERROR("D3D12CreateContext error: {} ({})", magic_enum::enum_name(result), value);

    return false;
}

LOG_INFO("XeFG context created");
```

Before finalizing this exact path, inspect whether a non-success return can leave `_swapChainContext` non-null. If current SDK behavior or the existing code makes that possible, do not leak the handle: route it through the existing owner-safe cleanup path rather than simply dropping it. Do not introduce a new force-destroy/retry loop.

### 7.2 Latency reduction

Do not treat a warning as proof that XeLL/XeFG connection completed normally.

Use exact success to continue the initialization sequence. Log a positive result as warning, negative as error.

### 7.3 Swapchain initialization

For both `CreateSwapchain()` and `CreateSwapchain1()`:

```text
D3D12InitFromSwapChainDesc
D3D12GetSwapChainPtr
```

must require exact success before the code reports `XeFG swapchain created`, accepts the returned proxy pointer, or commits `_swapChain` / `_hwnd` state.

A warning here is not permission to consume a potentially uncertain output pointer/state.

Preserve the existing `AbortSwapchainInitialization(...)` / fail-closed lifecycle cleanup behavior.

---

## 8. Task C — Make Destroy strict and preserve F-02 quarantine

This is the highest-priority lifecycle correction.

Current master contains behavior equivalent to:

```cpp
auto result = XeFGProxy::Destroy()(context);

if (static_cast<int32_t>(result) < 0)
{
    _swapChainContext = context;
    _swapchainRecreationBlocked = true;
    return false;
}

_swapchainRecreationBlocked = false;
return true;
```

That means an unexpected positive warning is treated as completed destruction.

Change the lifecycle gate to **exact success**:

```cpp
const auto result = XeFGProxy::Destroy()(context);
const auto value = static_cast<int32_t>(result);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _swapChainContext = context;
    _swapchainRecreationBlocked = true;

    if (value > 0)
    {
        LOG_WARN("[XeFG][Lifecycle] action = destroy_not_confirmed, context = {:X}, "
                 "result = {} ({}), retained = true",
                 (size_t) context, magic_enum::enum_name(result), value);
    }
    else
    {
        LOG_ERROR("[XeFG][Lifecycle] action = destroy_failed, context = {:X}, "
                  "result = {} ({}), retained = true",
                  (size_t) context, magic_enum::enum_name(result), value);
    }

    return false;
}

_swapchainRecreationBlocked = false;
State::Instance().currentFGSwapchain = nullptr;
return true;
```

The exact log wording may differ, but preserve the invariant:

```text
SUCCESS exactly
    -> destruction confirmed
    -> old context identity may be cleared
    -> recreation may proceed

warning or error
    -> destruction not confirmed by OptiScaler policy
    -> retain exact old context identity
    -> recreation remains blocked
```

Do not retry `Destroy()` automatically.

Do not force-release COM references to make Destroy succeed.

---

## 9. Task D — Restore exact-success gates for output/state-bearing APIs

Audit and correct these `d87d8ca` changes:

### 9.1 `GetProperties()`

Do not consume `props.maxSupportedInterpolations` unless result is exact success.

```cpp
if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _maxInterpolationCount = props.maxSupportedInterpolations;
}
else
{
    // warning vs error logging
}
```

### 9.2 `SetEnabled()` in `Activate()` / `Deactivate()`

Do not modify `_isActive`, `_lastDispatchedFrame`, or related local state unless the XeFG call returned exact success.

A warning may be non-fatal at the SDK classification layer, but it does not prove OptiScaler's requested state transition completed exactly as expected.

### 9.3 XeFG version query

In `XeFG_Proxy.h`, do not cache/commit `_xefgVersion` as valid solely because the result is non-negative.

Prefer exact-success validation before the normal `XeFG Version: ...` path.

If a warning is returned, log it diagnostically and leave the version state uncommitted/unchanged unless the surrounding current implementation has a clearly documented fallback.

---

## 10. Task E — Restore explicit handling for per-frame/configuration calls

Audit the remaining call sites changed by `d87d8ca`, including at least:

```text
xefgSwapChainSetUiCompositionState
xefgSwapChainSetNumInterpolatedFrames
xefgSwapChainTagFrameConstants
xefgSwapChainSetPresentId
xefgSwapChainD3D12TagFrameResource
resource-tagging paths in SetResource(...)
```

For this PR, preserve the **pre-d87 control-flow intent** at each call site rather than inventing one global rule.

Examples:

### Calls where non-success was already diagnostic-only

If the old code logged a non-success but intentionally continued even on an error, keep that non-fatal control flow, but make warning/error severity correct:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    if (IsXeFGWarning(result))
        LOG_WARN(...);
    else
        LOG_ERROR(...);

    // Existing call-site behavior continues here.
}
```

This is not the same as declaring the call a success; it merely preserves an already non-fatal call-site policy.

### Calls where non-success aborted or triggered recovery

Restore exact-success gating:

```cpp
if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    if (IsXeFGWarning(result))
        LOG_WARN(...);
    else
        LOG_ERROR(...);

    // Preserve the existing abort / fgChanged / UpdateTarget path.
}
```

Do not silently ignore a positive warning.

Do not add special continuation for `WARNING_TOO_FEW_FRAMES`, `WARNING_FRAMES_ID_MISMATCH`, `WARNING_MISSING_PRESENT_STATUS`, or `WARNING_RESOURCE_SIZES_MISMATCH` in these calls unless runtime evidence proves the specific API actually returns the warning and continuation is desired.

Intel explicitly calls out `GetLastPresentStatus()` as the expected warning-heavy path after history reset/resolution changes. That does not automatically establish the same policy for every tagging/control API.

---

## 11. Task F — Preserve signed diagnostics

One useful part of `d87d8ca` was moving some lifecycle result logging away from unsigned formatting.

Keep signed numeric output for XeFG status codes where practical:

```cpp
static_cast<int32_t>(result)
```

Do not regress diagnostics to a huge unsigned value for negative errors.

For example, retain this style:

```cpp
LOG_INFO("... result = {} ({})",
         magic_enum::enum_name(result),
         static_cast<int32_t>(result));
```

This work is about continuation policy, not removing useful signed logging.

---

## 12. Explicit non-goals

Do not include any of the following in this PR:

- a wholesale revert of all post-`d87d8ca` XeFG work;
- REFramework source changes;
- swapchain COM ownership changes;
- backbuffer release/drain changes;
- retry loops for XeFG Destroy or initialization;
- changes to Intel Reflex / DXGI spoofing;
- changes to XeLL architecture;
- changes to bundled XeSS/XeFG SDK versions;
- a generic DXGI refactor;
- a repository-wide error/result abstraction;
- speculative warning allowlists without runtime evidence.

Keep the diff narrowly focused on XeFG result interpretation and the state transitions controlled by those results.

---

## 13. Static validation

Before runtime testing:

1. `git diff --check` must pass.
2. Run clang-format on modified C/C++ files using the repository's expected version/style.
3. Release x64 build must pass when the MSVC toolchain is available.
4. Audit both modified files for blanket sign-based success checks introduced by `d87d8ca`:

```text
static_cast<int32_t>(result) >= 0
static_cast<int32_t>(result) < 0
```

Classification checks may still exist for warning/error logging, but **no lifecycle/state/output success decision should depend only on non-negativity**.

5. Search for every changed XeFG API call and record its policy in the PR description:

```text
API | exact-success required? | warning action | error action | state/output committed?
```

6. Confirm `DestroySwapchainContext()` still retains `_swapChainContext` and sets `_swapchainRecreationBlocked = true` for any result that this PR does not treat as confirmed exact success.

7. Confirm PR #13 owner-scoped alias cleanup remains unchanged.

---

## 14. Runtime validation

Runtime validation should focus on regressions from stricter policy, not only on whether the game launches.

### Primary

```text
Monster Hunter Wilds
Intel GPU
XeFG output
REFramework loaded
```

Exercise:

- game startup;
- FG enable/disable if exposed by the normal path;
- repeated resize / window-state transitions used by the current MHW investigation;
- shutdown;
- restart;
- swapchain recreation path if naturally triggered.

Verify:

- no crash/hang;
- no recreated XeFG context over an unconfirmed old context;
- no force-drain logging;
- no REF overlay regression caused by this result-policy change;
- normal successful XeFG calls still follow the same fast path;
- any positive warning is visible and is not silently promoted to exact success.

### Regression

Where available, test at least one additional known-good RE Engine title such as DD2 and one OptiScaler XeFG path without REFramework.

Do not add game-specific behavior to make these tests pass.

---

## 15. Warning diagnostics expected during runtime

If a positive XeFG result is observed, capture:

```text
API name
symbolic result name
signed numeric result
current lifecycle stage
whether OptiScaler continued, aborted, or retained state
```

Example:

```text
[XeFG][Warning] api = xefgSwapChainSetPresentId, result = XEFG_SWAPCHAIN_RESULT_WARNING_..., value = 4, action = frame_path_not_confirmed
```

Do not spam per-frame logs for successful calls.

If warning logging becomes per-frame spam in a real game, report that observation in the PR rather than hiding the warning by treating it as success.

---

## 16. Acceptance criteria

This work is complete when all of the following are true:

- [ ] No blanket `non-negative == successful completion` policy remains in the XeFG call sites changed by `d87d8ca`.
- [ ] Positive values are classified/logged as warnings, not errors.
- [ ] Exact success is required before lifecycle completion, state transitions, and output consumption unless a call site is explicitly documented as non-fatal.
- [ ] `DestroySwapchainContext()` treats any non-exact-success result as destruction not confirmed and preserves F-02 fail-closed behavior.
- [ ] `Activate()` / `Deactivate()` do not change internal active state on an unconfirmed warning result.
- [ ] `GetProperties()` / version query do not commit output on a warning by default.
- [ ] Per-frame/configuration APIs preserve their pre-d87 call-site control-flow intent and do not silently swallow warnings.
- [ ] Signed status-code logging remains intact.
- [ ] PR #6 / #13 ownership fixes remain intact.
- [ ] No REFramework changes are included.
- [ ] `git diff --check` passes.
- [ ] Release x64 build passes when toolchain is available.
- [ ] Runtime validation results are documented in the PR, or clearly marked pending if the implementation environment cannot run the required games/hardware.

---

## 17. PR shape

Use one focused PR.

Suggested branch:

```text
fix/xefg-explicit-warning-policy
```

Suggested title:

```text
Refine XeFG warning handling and success gates
```

The PR description should explicitly state:

1. Intel still classifies positive values as warnings/non-errors.
2. This PR does **not** dispute that SDK contract.
3. The correction is that OptiScaler should not infer **continue/commit state** from the sign alone.
4. Exact success is restored where lifecycle/state/output validity depends on the call.
5. Existing non-fatal call-site behavior is preserved where appropriate, with warnings now visible instead of silently swallowed.
6. `d87d8ca` is being narrowed semantically, not blindly reverted.

Create the PR as **Draft** until static/build validation is complete. Runtime game validation may remain explicitly pending if unavailable in the implementation environment.
