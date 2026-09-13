# Release/0.9 PR25 Compatibility Parity and Low-Risk Cleanup Work Order

Date: 2026-09-13

## Objective

Prepare the next production-oriented change for the forked `reframework-0.9` branch after P1/P2/P3.

This PR is intentionally a **small compatibility-parity and low-risk cleanup PR**. It should bring the merged Intel Reflex selective-DXGI fix from fork `master` into the 0.9 line using the 0.9 architecture, fix one clear COM lifetime issue in the XeFG swapchain creation path, and finish the remaining low-risk XeFG version-output commit semantics from master PR #14.

This PR is **not** the final ownership/generation hardening pass. Wrapper/FFX generation ownership and the remaining Release-until-zero paths belong to the later PR26.

---

## Target branch and reviewed baselines

Implementation target:

```text
base branch: reframework-0.9
base commit: 7790314de9ffea736bd36bd4bdeda6f8b39bdee5
```

Reference branch reviewed for parity:

```text
fork master: 172040dd8bdfb48b6287861cc7d0b6b5ad18aaf6
```

Relevant merged master work:

- PR #12: Intel Reflex selective DXGI identity quirk for RE9 / PRAGMATA.
- PR #14: exact-success XeFG result/output commit semantics.

Important: **do not cherry-pick PR #12 directly onto `reframework-0.9`.** The branch architectures differ in state naming and spoof-suppression handling.

Suggested implementation branch:

```text
feature/reframework-0.9-pr25-compat-parity
```

Suggested PR title:

```text
PR25: complete release/0.9 Intel compatibility parity and XeFG cleanup
```

---

## Scope summary

PR25 should modify only these files unless a build-only include adjustment is strictly required:

```text
OptiScaler/misc/Quirks.h
OptiScaler/dllmain.cpp
OptiScaler/spoofing/Dxgi_Spoofing.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/proxies/XeFG_Proxy.h
```

The PR has three functional tasks:

1. Adapt master PR #12 to release/0.9 without importing the master spoofing subsystem.
2. Keep the queried `IDXGIFactory2` COM reference alive until `D3D12InitFromSwapChainDesc()` returns.
3. Make `XeFGProxy::Version()` commit queried version data only after exact XeFG success.

---

# Part A — Adapt PR #12 for release/0.9

## A1. Why this must be an adapted backport

Master PR #12 assumes the newer master architecture:

```cpp
State::Instance().gameExe
#include <misc/SkipSpoof.h>
SkipSpoofing()
```

Release/0.9 differs:

```cpp
State::Instance().GameExe
```

and `Dxgi_Spoofing.cpp` contains its own local helper:

```cpp
inline static bool SkipSpoofing()
{
    auto skip = !Config::Instance()->DxgiSpoofing.value_or_default() || State::Instance().skipSpoofing;
    ...
    return skip;
}
```

This difference is critical.

Do **not** use the release/0.9 local `SkipSpoofing()` inside the new Intel Reflex quirk eligibility test.

Why:

PR #12 intentionally works when:

```text
StreamlineSpoofing = true
DxgiSpoofing       = false
game quirk         = FixSlReflexAvailabilityOnIntel
returned adapter   = Intel descriptor
```

but release/0.9's local `SkipSpoofing()` returns `true` whenever `DxgiSpoofing == false`.

Therefore this would be wrong:

```cpp
// WRONG FOR release/0.9
if (!config->StreamlineSpoofing.value_or_default() ||
    config->DxgiSpoofing.value_or_default() ||
    SkipSpoofing())
{
    return false;
}
```

It would permanently disable the quirk in the exact configuration where it is needed.

For release/0.9, use the transient state flag directly:

```cpp
State::Instance().skipSpoofing
```

That flag is already used by release/0.9 `ScopedSkipSpoofing` around internal XeFG/DXGI-sensitive calls, so it preserves the intended “do not spoof internal calls” behavior without treating `DxgiSpoofing=false` as a veto.

Do not backport master `misc/SkipSpoof.h` / `SkipSpoof.cpp` as part of PR25.

---

## A2. Add the quirk without renumbering existing release/0.9 quirks

File:

```text
OptiScaler/misc/Quirks.h
```

Release/0.9 has a smaller `GameQuirk` enum than master.

Do not copy the master enum block wholesale.

Append the new value immediately before `_` so existing release/0.9 quirk numeric values remain unchanged:

```cpp
enum class GameQuirk : uint64_t
{
    ...
    IgnoreTagsWithoutHudlessForFG,
    ForceFGRenderSizeMVs,
    FixSlReflexAvailabilityOnIntel,
    // Don't forget to add the new entry to printQuirks
    _
};
```

Do not add unrelated master-only quirks such as `CreateSLOnThe2ndDevice` in this PR.

Apply the new quirk only to the same production executables as master PR #12:

```cpp
QUIRK_ENTRY("re9.exe",
            GameQuirk::RestoreComputeSigOnNonNvidia,
            GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia,
            GameQuirk::FixSlReflexAvailabilityOnIntel),

QUIRK_ENTRY("pragmata.exe",
            GameQuirk::RestoreComputeSigOnNonNvidia,
            GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia,
            GameQuirk::PregmataFixDLSSModes,
            GameQuirk::FixSlReflexAvailabilityOnIntel),
```

Do not expand scope to these unless separate runtime evidence exists:

```text
re9demo.exe
pragmata_sketchbook.exe
DD2
MHW
other Capcom titles
```

PR25 is a parity backport, not a new game-support expansion.

---

## A3. Add the quirk to `printQuirks()`

File:

```text
OptiScaler/dllmain.cpp
```

Add the same user-visible diagnostic string as master:

```cpp
if (quirks & GameQuirk::FixSlReflexAvailabilityOnIntel)
    stringQuirks.push_back("Fix Streamline Reflex availability on Intel");
```

Keep this as a normal startup quirk diagnostic only. Do not add per-frame logging.

---

## A4. Add release/0.9-specific selective DXGI helpers

File:

```text
OptiScaler/spoofing/Dxgi_Spoofing.cpp
```

Add the release/0.9-local state and quirk includes if not already available through
the PCH:

```cpp
#include <State.h>
#include <misc/Quirks.h>
```

Do **not** include:

```cpp
#include <misc/SkipSpoof.h>
```

because that subsystem is master-only and is out of scope for PR25.

The master-only `misc/IdentifyGpu.h` / `IdentifyGpu::getPrimaryGpu()` contract is
also out of scope. Release/0.9 does not contain that subsystem. The selective
path must instead evaluate the real vendor in the descriptor returned by the
current `GetDesc*` hook. This keeps the 0.9 backport within the existing five-file
scope and makes the non-Intel negative path explicit.

Place the new helper functions after the existing release/0.9 local `SkipSpoofing()` definition, or otherwise ensure the local helper is declared before use.

Recommended release/0.9 implementation:

```cpp
inline static bool IsIntelReflexDxgiQuirkConfigured()
{
    const auto* config = Config::Instance();
    const auto& state = State::Instance();

    if (!(state.gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel))
        return false;

    // This quirk is specifically for Streamline spoofing with broad DXGI
    // spoofing disabled. Do not call the release/0.9 local SkipSpoofing()
    // here because it returns true whenever DxgiSpoofing is disabled.
    if (!config->StreamlineSpoofing.value_or_default() ||
        config->DxgiSpoofing.value_or_default() ||
        state.skipSpoofing)
    {
        return false;
    }

    return true;
}

inline static bool IsIntelReflexDxgiQuirkEligible(uint32_t vendorId)
{
    return IsIntelReflexDxgiQuirkConfigured() && vendorId == VendorId::Intel;
}
```

The final return in the configuration helper is `true`; Intel eligibility is
checked separately against the descriptor received by the hook. In particular,
do not reintroduce a primary-GPU lookup or a master-only GPU-identification
subsystem in this PR.

Caller filtering must use the release/0.9 state member name:

```cpp
template <typename T>
inline static bool ShouldApplyIntelReflexGameIdentity(const std::string& caller, const T* desc)
{
    if (!IsIntelReflexDxgiQuirkEligible(desc->VendorId) ||
        !iequals(caller, State::Instance().GameExe))
    {
        return false;
    }

    const auto* config = Config::Instance();

    const bool targetVendorIdMatches =
        !config->TargetVendorId.has_value() ||
        config->TargetVendorId.value() == desc->VendorId;

    const bool targetDeviceIdMatches =
        !config->TargetDeviceId.has_value() ||
        config->TargetDeviceId.value() == desc->DeviceId;

    return desc->VendorId != VendorId::Microsoft &&
           targetVendorIdMatches &&
           targetDeviceIdMatches;
}
```

Apply the configured target identity exactly as master PR #12 does:

```cpp
template <typename T>
inline static void ApplyIntelReflexGameIdentity(T* desc)
{
    const auto* config = Config::Instance();

    desc->VendorId = config->SpoofedVendorId.value_or_default();
    desc->DeviceId = config->SpoofedDeviceId.value_or_default();

    const auto spoofedName = config->SpoofedGPUName.value_or_default();
    std::memset(desc->Description, 0, sizeof(desc->Description));
    std::wcscpy(desc->Description, spoofedName.c_str());
}
```

---

## A5. Apply the selective identity to all four existing descriptor hooks

After the existing normal release/0.9 DXGI spoofing logic inside each successful `GetDesc*` result path, add:

```cpp
if (ShouldApplyIntelReflexGameIdentity(caller, pDesc))
    ApplyIntelReflexGameIdentity(pDesc);
```

Required functions:

```text
DxgiSpoofing::hkGetDesc
DxgiSpoofing::hkGetDesc1
DxgiSpoofing::hkGetDesc2
DxgiSpoofing::hkGetDesc3
```

Do not replace or rewrite the existing release/0.9 broad spoofing logic.

Do not copy master `Dxgi_Spoofing.cpp` wholesale.

In particular, preserve release/0.9-specific caller exclusions and other branch-local behavior unless the PR25 change explicitly requires otherwise.

The quirk must remain **game-caller selective**. It must not turn broad DXGI spoofing back on.

Expected behavior:

```text
RE9/PRAGMATA game EXE calls GetDesc* for an Intel descriptor:
    -> selective configured NVIDIA identity may be returned

fakenvapi / DXGI / D3D12 / Vulkan / other internal callers:
    -> preserve existing release/0.9 behavior

RE9/PRAGMATA game EXE calls GetDesc* for AMD/NVIDIA descriptors:
    -> descriptor identity remains unchanged

other games:
    -> no PR12 selective behavior
```

---

## A6. Install descriptor hooks even when broad DXGI spoofing is disabled

Current release/0.9 `AttachToAdapter()` exits when both broad DXGI spoofing and VRAM spoofing are disabled.

PR #12 requires descriptor hooks to remain installed for the two game-specific selective cases.

Change the release/0.9 early gate to this shape:

```cpp
void DxgiSpoofing::AttachToAdapter(IUnknown* unkAdapter)
{
    static bool logAdded = false;

    const bool normalDxgiHooksNeeded =
        Config::Instance()->DxgiSpoofing.value_or_default() ||
        Config::Instance()->DxgiVRAM.has_value();

    const bool intelReflexQuirkNeeded = IsIntelReflexDxgiQuirkConfigured();

    if (!normalDxgiHooksNeeded && !intelReflexQuirkNeeded)
    {
        if (!logAdded)
        {
            LOG_WARN("DxgiSpoofing and DxgiVRAM is disabled, skipping hooking");
            logAdded = true;
        }

        return;
    }

    ... existing hook install logic ...
}
```

No additional hook family should be introduced.

---

# Part B — Keep the XeFG `IDXGIFactory2` reference alive through initialization

## B1. Problem

Both reviewed branches currently contain this pattern in both XeFG creation overloads:

```cpp
IDXGIFactory2* factory12 = nullptr;
if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
    ...;

factory12->Release();

// factory12 is used later
result = XeFGProxy::D3D12InitFromSwapChainDesc()(
    _swapChainContext,
    ...,
    factory12,
    &params);
```

`QueryInterface()` acquires a COM reference. Releasing that reference before the last use means the code no longer owns a lifetime guarantee for the interface pointer that is later passed into XeFG.

The object may remain alive because another owner currently holds the factory, but relying on that is not valid COM lifetime hygiene and is especially undesirable in a multi-wrapper environment such as OptiScaler + REFramework.

PR25 should fix this without redesigning swapchain ownership.

---

## B2. Required change

File:

```text
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
```

Functions:

```text
XeFG_Dx12::CreateSwapchain
XeFG_Dx12::CreateSwapchain1
```

Preferred implementation: use a local `Microsoft::WRL::ComPtr<IDXGIFactory2>` and keep it alive until after `D3D12InitFromSwapChainDesc()` returns.

Example:

```cpp
Microsoft::WRL::ComPtr<IDXGIFactory2> factory12;

if (FAILED(realFactory->QueryInterface(IID_PPV_ARGS(&factory12))))
    return AbortSwapchainInitialization("QueryInterface(IDXGIFactory2)");
```

Then pass:

```cpp
result = XeFGProxy::D3D12InitFromSwapChainDesc()(
    _swapChainContext,
    hwnd,
    &scDesc,
    &fsDesc,
    realQueue,
    factory12.Get(),
    &params);
```

and similarly for `CreateSwapchain1()`:

```cpp
result = XeFGProxy::D3D12InitFromSwapChainDesc()(
    _swapChainContext,
    hwnd,
    desc,
    pFullscreenDesc,
    realQueue,
    factory12.Get(),
    &params);
```

Do not manually `Release()` `factory12` before the vendor call.

RAII should release the QI-acquired reference when the function leaves, including failure paths.

If the local build environment requires an explicit WRL include, use the smallest local include necessary. Do not introduce a general COM smart-pointer refactor.

Important invariants:

- Do not AddRef/Release `realFactory` unless the local code itself acquired a reference.
- Do not change `realQueue` ownership.
- Do not alter `CheckForRealObject()` semantics.
- Do not change P3 lifecycle locking or recreation quarantine.
- Do not modify current swapchain alias ownership rules.

---

# Part C — Finish XeFG version output-commit semantics

## C1. Problem

Release/0.9 currently writes the vendor output directly into the cached state:

```cpp
if (auto result = _xefgSwapChainGetVersion(&_xefgVersion);
    result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    ...
}
```

If the API returns a warning/error while writing partial or undefined output, `_xefgVersion` has already been modified before the result is validated.

Master PR #14 fixed this by querying into a local object and committing only on exact success.

This is consistent with the P3 rule already used in release/0.9: non-exact success must not automatically commit lifecycle/state/output data.

---

## C2. Required implementation

File:

```text
OptiScaler/proxies/XeFG_Proxy.h
```

Add:

```cpp
#include <magic_enum.hpp>
```

if not already present.

Replace the direct cache write with a local query:

```cpp
static xefg_swapchain_version_t Version()
{
    if (_xefgVersion.major == 0 && _xefgSwapChainGetVersion != nullptr)
    {
        xefg_swapchain_version_t version {};
        auto result = _xefgSwapChainGetVersion(&version);

        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            _xefgVersion = version;
            LOG_INFO("XeFG Version: v{}.{}.{}",
                     _xefgVersion.major,
                     _xefgVersion.minor,
                     _xefgVersion.patch);
        }
        else
        {
            const auto value = static_cast<int32_t>(result);

            if (value > 0)
            {
                LOG_WARN("XeFG version warning: {} ({})",
                         magic_enum::enum_name(result), value);
            }
            else
            {
                LOG_ERROR("XeFG version error: {} ({})",
                          magic_enum::enum_name(result), value);
            }
        }
    }

    // Preserve existing release/0.9 fallback behavior.
    if (_xefgVersion.major == 0)
    {
        _xefgVersion.major = 1;
        _xefgVersion.minor = 0;
        _xefgVersion.patch = 0;
    }

    return _xefgVersion;
}
```

Do not change the existing fallback version policy in PR25.

---

# Explicit non-goals

PR25 must **not** include any of the following:

- PR #17 thread-local FG hook guards.
- PR #18 thread-aware `OwnedMutex` changes.
- PR #19 automatic XeFG trace infrastructure.
- PR #20 / #21 Subnautica 2 geometry diagnostics.
- Remaining FFX/FSR3 `currentWrappedSwapchain` Release-until-zero cleanup.
- Wrapper/XeFG generation IDs.
- FFX context-generation redesign.
- General wrapper ownership changes.
- Additional `ReleaseSwapchain()` behavior changes.
- New XeFG Destroy retry/recovery logic.
- Master XeLL/fakenvapi architecture migration.
- LOW_LATENCY_INPUTS/InputXeLL migration.
- New game quirks beyond exact PR #12 parity.
- Generic DXGI refactor.
- Generic COM smart-pointer conversion.

Those ownership/generation changes belong to PR26 so any runtime regression remains easy to isolate.

---

# Required static review after implementation

Run all of the following before opening the PR.

## Changed-file scope

Expected functional file set:

```text
OptiScaler/misc/Quirks.h
OptiScaler/dllmain.cpp
OptiScaler/spoofing/Dxgi_Spoofing.cpp
OptiScaler/framegen/xefg/XeFG_Dx12.cpp
OptiScaler/proxies/XeFG_Proxy.h
```

Any additional functional file requires explicit justification in the PR body.

## Diff sanity

```bash
git diff --check
```

## Formatting

Run repository clang-format on every changed C/C++ file, then verify with the same formatter version used by CI.

## PR12 adaptation searches

Verify the new 0.9 code does not accidentally use the master state name:

```text
gameExe
```

The new selective caller check must use:

```text
GameExe
```

Verify PR25 did not import:

```text
misc/SkipSpoof.h
misc/SkipSpoof.cpp
```

Verify the Intel Reflex eligibility function does **not** use the release/0.9 local `SkipSpoofing()` as its `Dxgi=false` suppression condition.

Expected condition is equivalent to:

```cpp
!StreamlineSpoofing || DxgiSpoofing || State::Instance().skipSpoofing
```

## Factory lifetime searches

There must be no pattern in the two modified XeFG create paths equivalent to:

```cpp
factory12->Release();
...
D3D12InitFromSwapChainDesc(... factory12 ...);
```

The QI-acquired `IDXGIFactory2` reference must remain alive through the vendor init call.

## Version output-commit search

There must be no direct non-validated version query into the cache:

```cpp
_xefgSwapChainGetVersion(&_xefgVersion)
```

Use local output + exact-success commit.

---

# Build validation

Required:

```text
Configuration: Release
Platform: x64
Target: OptiScaler.dll
```

Build must complete without new errors.

Existing unrelated warnings may remain, but list any new warning introduced by the PR.

---

# Runtime validation matrix

PR25 is low risk, but it changes both adapter identity reporting and the factory lifetime used during XeFG creation. Run targeted smoke tests.

## 1. RE9 — Intel

Required configuration:

```text
Intel GPU
StreamlineSpoofing = true
Dxgi = false
FixSlReflexAvailabilityOnIntel quirk active
```

Validate:

- DLSS options remain available.
- DLSSG/XeFG path still initializes.
- Reflex becomes available as intended by master PR #12.
- No global DXGI identity spoof is observed outside the intended game-caller path.
- launch -> gameplay -> menu -> Alt+Tab -> return -> exit.

## 2. PRAGMATA — Intel

Same checks as RE9.

Also ensure `PregmataFixDLSSModes` behavior is unchanged.

## 3. MHW — Intel XeFG + fork REFramework

The new Reflex quirk must **not** activate for MHW.

Smoke:

```text
cold launch
enter gameplay
open/close overlays
Alt+Tab
window/fullscreen or borderless transition if available
normal exit
```

Confirm no regression in P1/P2/P3 lifecycle logs.

## 4. DD2 — Intel or existing REF/XeFG test machine

Same lifecycle smoke test.

The new Reflex quirk must not activate.

## 5. Non-Intel negative check

On AMD or NVIDIA, or by static/log verification if hardware is unavailable:

```text
FixSlReflexAvailabilityOnIntel may be present in the game quirk table,
but IsIntelReflexDxgiQuirkEligible(desc->VendorId) must return false for
AMD/NVIDIA descriptors.
```

Do not selectively rewrite the game adapter identity on non-Intel hardware.

Hybrid-GPU expectation: this release/0.9 backport intentionally does not make a
global primary-GPU determination. It applies only when the game caller receives
an Intel adapter descriptor. If a hybrid system exposes both Intel and NVIDIA
descriptors, only the Intel descriptor is eligible and the NVIDIA descriptor must
remain unchanged. This is intentionally narrower than master PR #12 and should
be covered by the runtime or static/log validation.

---

# Acceptance criteria

PR25 is ready to merge only if all conditions below are true.

1. RE9 and PRAGMATA have the intended selective Reflex DXGI behavior adapted from master PR #12: only a game-caller Intel descriptor is eligible.
2. The implementation is adapted to release/0.9 (`GameExe`, legacy `skipSpoofing`, and no master-only `IdentifyGpu`) rather than copied blindly from master.
3. The release/0.9 local `SkipSpoofing()` semantic trap is avoided.
4. Broad `Dxgi=false` remains broad spoofing off; only the game-caller selective identity path is added.
5. Existing internal caller exclusions remain branch-local and are not replaced by master wholesale.
6. `IDXGIFactory2` QI references stay alive through both XeFG `D3D12InitFromSwapChainDesc()` calls.
7. `XeFGProxy::Version()` commits vendor output only after exact success.
8. P1/P2/P3 ownership and fail-closed lifecycle logic is unchanged.
9. No PR17/18/19/20/21 experimental/diagnostic work is included.
10. No PR26 ownership/generation work is included.
11. `git diff --check` passes.
12. clang-format check passes.
13. Release x64 build passes.
14. Runtime smoke tests show no REF/XeFG regression.

---

# Suggested PR body

```markdown
## Summary

Complete the low-risk compatibility parity pass for the `reframework-0.9` line after P1/P2/P3.

- Adapt the merged master PR #12 Intel Reflex selective-DXGI quirk to the release/0.9 architecture for RE9 and PRAGMATA.
- Preserve broad `Dxgi=false` while allowing game-exe-only configured adapter identity for the Reflex availability check on Intel.
- Keep the QI-acquired `IDXGIFactory2` reference alive through XeFG swapchain initialization.
- Commit XeFG version-query output only after exact `XEFG_SWAPCHAIN_RESULT_SUCCESS`.

## Important branch adaptations

- Uses release/0.9 `State::GameExe`, not master `gameExe`.
- Does not import master `misc/SkipSpoof` or `misc/IdentifyGpu`.
- Uses `State::skipSpoofing` for the selective Reflex quirk suppression gate because the release/0.9 local `SkipSpoofing()` returns true whenever broad DXGI spoofing is disabled.
- Checks the returned descriptor vendor directly, so AMD/NVIDIA descriptors remain unchanged and the behavior is intentionally narrower than master PR #12 on hybrid systems.

## Non-goals

No wrapper/FFX generation changes, no remaining force-drain cleanup, no PR17/18/19 synchronization/diagnostic changes, no Subnautica diagnostic work, and no XeFG lifecycle redesign beyond the local factory lifetime fix.

## Validation

- git diff --check
- clang-format check
- Release x64 build
- RE9 / PRAGMATA Intel Reflex validation
- MHW / DD2 REF + XeFG smoke validation
```

---

# Reviewer focus

Review this PR primarily for accidental cross-branch semantic imports.

The highest-risk review mistake would be accepting code that visually matches master PR #12 but uses the wrong release/0.9 spoof-suppression semantics.

Specifically reject an implementation that does this:

```cpp
// Wrong on release/0.9
if (!config->StreamlineSpoofing.value_or_default() ||
    config->DxgiSpoofing.value_or_default() ||
    SkipSpoofing())
{
    return false;
}
```

because the release/0.9 local `SkipSpoofing()` includes `!DxgiSpoofing` in its definition and would therefore make the selective quirk unreachable.

The correct release/0.9 equivalent must test the transient suppression state independently:

```cpp
if (!config->StreamlineSpoofing.value_or_default() ||
    config->DxgiSpoofing.value_or_default() ||
    State::Instance().skipSpoofing)
{
    return false;
}
```

That distinction is the central adaptation required for PR25.
