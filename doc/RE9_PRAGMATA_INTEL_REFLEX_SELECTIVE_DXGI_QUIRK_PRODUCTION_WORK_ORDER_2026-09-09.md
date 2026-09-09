# Work Order: RE9 / PRAGMATA Intel Reflex Selective DXGI Identity Quirk

Date: 2026-09-09  
Repository: `onehoon/OptiScaler`  
Production base branch: `master`  
Reviewed master SHA: `3809b22150da2e5eb723f58d9e8eadb58705a553`  
Experimental reference branch: `poc_reflex`  
Experimental handoff: `doc/PRAGMATA_INTEL_REFLEX_ROOT_CAUSE_AND_PRODUCTION_HANDOFF_2026-09-09.md`  
Suggested implementation branch: `fix/intel-reflex-selective-dxgi-quirk`

---

## 1. Goal

Implement the smallest production-ready OptiScaler quirk that restores NVIDIA Reflex availability on Intel GPUs in:

- Resident Evil Requiem: `re9.exe`
- PRAGMATA: `pragmata.exe`

Both games have now been runtime-validated with the experimental `GameOnly` selective DXGI identity path.

The validated behavior is:

- real GPU: Intel;
- `StreamlineSpoofing=true`;
- global DXGI spoofing remains disabled (`Dxgi=false`);
- only DXGI adapter description queries originating from the target game executable see the configured spoofed NVIDIA adapter identity;
- `sl.common.dll`, Intel/Xe runtimes, DXGI/D3D system modules, and unrelated callers continue to see the real Intel identity;
- Reflex becomes available without enabling global DXGI spoofing.

The production implementation must convert this validated POC behavior into a normal OptiScaler `GameQuirk` and nothing more.

---

## 2. Root Cause / Evidence Summary

The experimental work narrowed the issue to a game-side DXGI adapter/vendor capability decision.

Important observations:

1. Streamline spoofing already unlocks the relevant DLSS/DLSS-G paths on Intel.
2. Global `Dxgi=true` also makes Reflex available, but it exposes NVIDIA identity too broadly and causes unwanted behavior/performance overhead.
3. Selective POC testing showed that spoofing only `sl.common.dll` was not the required condition.
4. Spoofing only calls originating from the game executable (`GameOnly`) makes Reflex available.
5. PRAGMATA and RE9 have both been confirmed to work with this `GameOnly` policy.

Therefore production must not preserve the four-way POC selector. The confirmed policy is simply:

> On Intel, for the two game-specific quirks, while Streamline spoofing is active and global DXGI spoofing is off, return the configured spoofed NVIDIA adapter identity only to DXGI `GetDesc*` calls whose caller is the running game executable.

---

## 3. Production Scope

The intended production diff should be limited to these existing files:

1. `OptiScaler/misc/Quirks.h`
2. `OptiScaler/dllmain.cpp`
3. `OptiScaler/spoofing/Dxgi_Spoofing.cpp`

No new source/header/config file should be necessary.

Expected change size: small, localized, and substantially smaller than the experimental branch.

---

## 4. Branch / Extraction Policy

### 4.1 Start from current production master

Create a fresh implementation branch from the latest `master`.

The source baseline reviewed for this work order is:

```text
3809b22150da2e5eb723f58d9e8eadb58705a553
```

If `master` advances before implementation starts, use the new latest `master` and re-check the three touched files before editing.

### 4.2 Do not merge the POC lineage

Do **not**:

- merge `poc_reflex` into the implementation branch;
- cherry-pick PR #11 wholesale;
- cherry-pick the complete Reflex diagnostic commit sequence;
- reproduce the experimental config plumbing.

The POC branch is behavioral/reference evidence only.

Implement the final behavior directly on top of current `master`.

---

## 5. Existing Master Code Constraints

### 5.1 Quirk model

`GameQuirk` and `QuirkEntry` already provide the correct title-specific mechanism.

Current entries are executable-based and the runtime code applies additional conditions where needed.

Do not redesign `QuirkEntry` or introduce vendor-aware quirk-table fields/macros for this fix.

In particular, do not add any special per-adapter LUID policy. Existing quirk behavior is scoped by executable and runtime/vendor conditions, not by a new adapter-LUID quirk framework.

### 5.2 DXGI adapter hooks

Current `Dxgi_Spoofing.cpp` contains hooks for:

- `IDXGIAdapter::GetDesc`
- `IDXGIAdapter1::GetDesc1`
- `IDXGIAdapter2::GetDesc2`
- `IDXGIAdapter4::GetDesc3`

The existing broad spoofing path must remain behaviorally unchanged.

### 5.3 Mandatory hook-installation issue

Current `DxgiSpoofing::AttachToAdapter()` exits when both normal DXGI spoofing and DXGI VRAM override are disabled:

```cpp
if (!Config::Instance()->DxgiSpoofing.value_or_default() && !Config::Instance()->DxgiVRAM.has_value())
{
    // ...
    return;
}
```

That is incompatible with the required production behavior because both target games normally use `Dxgi=false` for this fix.

Therefore the deep-code quirk must be allowed to request the existing adapter `GetDesc*` hooks even when global DXGI spoofing is disabled.

This is required functionality, not an optional cleanup.

---

## 6. Implementation Plan

## 6.1 `OptiScaler/misc/Quirks.h`

Add the deep-code quirk:

```cpp
FixSlReflexAvailabilityOnIntel,
```

Place it with the existing deeper-code quirks, before the `_` sentinel, and keep the existing comment reminding contributors to update `printQuirks`.

Attach it only to the two runtime-validated production executables.

Conceptual result:

```cpp
QUIRK_ENTRY("re9.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::FixSlReflexAvailabilityOnIntel),

QUIRK_ENTRY("pragmata.exe", GameQuirk::RestoreComputeSigOnNonNvidia, GameQuirk::DisableDxgiSpoofing,
            GameQuirk::RestoreComputeSigOnNvidia, GameQuirk::PregmataFixDLSSModes,
            GameQuirk::FixSlReflexAvailabilityOnIntel),
```

Do not add this quirk to:

- `re9demo.exe`;
- `pragmata_sketchbook.exe`;
- DD2;
- Monster Hunter Wilds;
- other Capcom games.

Those executables have not been shown to require this exact correction.

---

## 6.2 `OptiScaler/dllmain.cpp`

Update `printQuirks()` using the existing one-line pattern.

Example:

```cpp
if (quirks & GameQuirk::FixSlReflexAvailabilityOnIntel)
    stringQuirks.push_back("Fix Streamline Reflex availability on Intel");
```

Do not add a config-level quirk transformation in `CheckQuirks()`.

This is a deep-code quirk. `CheckQuirks()` already stores the resolved set in:

```cpp
State::Instance().gameQuirks = quirks;
```

`Dxgi_Spoofing.cpp` can consume the flag directly.

---

## 6.3 `OptiScaler/spoofing/Dxgi_Spoofing.cpp`

### 6.3.1 Preserve existing broad spoof behavior

Do not refactor the current `DxgiSpoofing` implementation into a new generalized identity framework merely to support this quirk.

In particular, leave the existing broad `DxgiSpoofing=true` path structurally and behaviorally unchanged as much as possible.

The production patch should add a small quirk-specific path alongside it.

### 6.3.2 Add a narrow quirk eligibility helper

Use the existing runtime state and config mechanisms.

Suggested shape:

```cpp
bool IsIntelReflexDxgiQuirkEligible()
{
    const auto* config = Config::Instance();

    if (!(State::Instance().gameQuirks & GameQuirk::FixSlReflexAvailabilityOnIntel))
        return false;

    if (!config->StreamlineSpoofing.value_or_default() || config->DxgiSpoofing.value_or_default() || SkipSpoofing())
        return false;

    return IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;
}

bool ShouldApplyIntelReflexGameIdentity(const std::string& caller)
{
    return IsIntelReflexDxgiQuirkEligible() && iequals(caller, State::Instance().gameExe);
}
```

Equivalent formatting/organization is acceptable.

Required gates are:

```text
FixSlReflexAvailabilityOnIntel quirk active
AND primary GPU vendor == Intel
AND StreamlineSpoofing == true
AND global DxgiSpoofing == false
AND SkipSpoofing() == false
AND caller module == current game executable
```

Do not hard-code `re9.exe` or `pragmata.exe` inside `Dxgi_Spoofing.cpp`; the quirk table owns executable selection.

Do not add a LUID comparison or a new adapter-selection policy. That is not part of the validated fix and is not an existing quirk convention.

### 6.3.3 Allow the quirk to install existing adapter hooks

Change `AttachToAdapter()` so that the adapter hooks are retained when either the normal DXGI features require them or this deep-code quirk requires them.

Suggested shape:

```cpp
void DxgiSpoofing::AttachToAdapter(IUnknown* unkAdapter)
{
    static bool logAdded = false;

    const bool normalDxgiHooksNeeded =
        Config::Instance()->DxgiSpoofing.value_or_default() || Config::Instance()->DxgiVRAM.has_value();
    const bool intelReflexQuirkNeeded = IsIntelReflexDxgiQuirkEligible();

    if (!normalDxgiHooksNeeded && !intelReflexQuirkNeeded)
    {
        if (!logAdded)
        {
            LOG_WARN("DxgiSpoofing and DxgiVRAM is disabled, skipping hooking");
            logAdded = true;
        }

        return;
    }

    // Keep the existing attachment logic unchanged below this point.
    // ...
}
```

Minor log wording cleanup is optional only if necessary for correctness. Do not turn this into a logging/refactor PR.

### 6.3.4 Apply the identity only to the game caller

For each successful `GetDesc*` call, preserve all existing behavior first, including:

- excluded caller handling;
- DXGI VRAM override;
- existing broad DXGI spoofing;
- the existing FSR4 AMD caller behavior in `hkGetDesc1`.

Then apply the selective identity only when the quirk helper returns true.

A small template/helper may be used to avoid duplicating the three identity assignments, but it must remain dedicated to this selective quirk rather than refactoring the broad spoof path.

Example helper:

```cpp
template <typename T> void ApplyIntelReflexGameIdentity(T* desc)
{
    const auto* config = Config::Instance();

    desc->VendorId = config->SpoofedVendorId.value_or_default();
    desc->DeviceId = config->SpoofedDeviceId.value_or_default();

    const auto spoofedName = config->SpoofedGPUName.value_or_default();
    std::memset(desc->Description, 0, sizeof(desc->Description));
    std::wcscpy(desc->Description, spoofedName.c_str());
}
```

Example call site:

```cpp
if (pDesc->VendorId != VendorId::Microsoft && ShouldApplyIntelReflexGameIdentity(caller))
    ApplyIntelReflexGameIdentity(pDesc);
```

Apply this consistently to:

- `hkGetDesc`
- `hkGetDesc1`
- `hkGetDesc2`
- `hkGetDesc3`

Keep the existing `VendorId::Microsoft` exclusion because the current broad DXGI spoofing code already avoids spoofing the Microsoft software adapter.

### 6.3.5 Do not double-apply when global DXGI spoofing is enabled

The eligibility helper must explicitly require:

```cpp
!Config::Instance()->DxgiSpoofing.value_or_default()
```

Therefore when a user intentionally enables global `Dxgi=true`, the existing broad spoofing path remains authoritative and this quirk-specific path is inactive.

---

## 7. Explicit Non-Goals / Files That Must Not Be Brought Over

The following experimental pieces must **not** appear in this production PR:

### POC config/UI selector

- `ReflexDxgiIdentityScope=Off|SlCommonOnly|GameOnly|SlCommonAndGame`
- `OptiScaler.ini` additions
- `Config.cpp` / `Config.h` plumbing for that setting
- `misc/ReflexDxgiIdentityScope.h`

### POC diagnostics

- `[ReflexDxgiPOC]` per-`GetDesc*` logs
- `misc/ReflexProviderDiag.cpp`
- `misc/ReflexProviderDiag.h`
- feature-aware PCL/Reflex diagnostic changes in `Streamline_Hooks.cpp`
- noisy temporary call-site logging

### Rejected / obsolete availability hypotheses

- forcing `slReflexGetState().lowLatencyAvailable = true`
- `NvAPI_D3D_GetSleepStatus` fallback changes
- global fake-NVAPI behavior changes
- forced `slIsFeatureSupported` results
- forced PCL/Reflex support return values
- PCL `field_8` manipulation

### Unrelated systems

- XeFG swapchain ownership/lifetime changes
- resize-hook changes
- REFramework compatibility changes
- anti-tamper changes
- executable memory patches
- global DXGI architecture refactor
- DXGI factory policy expansion unrelated to this exact quirk

---

## 8. Caller Visibility Invariant

The key production invariant is not merely that Reflex becomes visible.

The identity boundary must remain narrow.

### Must see configured spoofed NVIDIA identity

Only DXGI adapter `GetDesc*` calls originating from the running target game executable while all quirk eligibility conditions are true.

### Must continue seeing the real Intel identity

Unless another pre-existing OptiScaler feature independently changes their behavior:

- `sl.common.dll`
- `libxess.dll`
- `libxess_fg.dll`
- `libxell.dll`
- Intel graphics/runtime modules
- `dxgi.dll`
- `d3d12.dll`
- `d3d12Core.dll`
- Vulkan callers
- unrelated DLL callers

The production quirk must not turn into a hidden global `Dxgi=true` equivalent.

---

## 9. Required Runtime Validation

## 9.1 PRAGMATA / Intel

Use the normal intended configuration, with no POC selector:

```ini
[Spoofing]
StreamlineSpoofing=true
Dxgi=false
```

Relevant FG configuration for the reproduced path:

```text
FGInput=DLSSG
FGOutput=XeFG
```

Expected:

- quirk is detected automatically for `pragmata.exe`;
- Reflex option is available in game;
- DLSS upscaling remains available;
- DLSS-G input path remains functional;
- XeFG output remains functional;
- XeSS/XeFG/XeLL continue using the intended Intel path;
- global DXGI spoofing remains disabled;
- no broad NVIDIA adapter identity leakage;
- no new crash/hang behavior.

## 9.2 Resident Evil Requiem / Intel

Repeat the same validation for `re9.exe`.

The experimental `GameOnly` behavior has already been confirmed to work, but the final master-based minimal implementation must be validated independently before merge.

Expected outcome is the same as PRAGMATA: Reflex available with `Dxgi=false`, without broad DXGI identity spoofing.

---

## 10. Reflex Lifecycle Validation

The production fix should not merely make a menu item appear.

Verify that the game proceeds through its normal Reflex lifecycle after the DXGI capability gate is satisfied.

Where available in existing logs/diagnostics, confirm:

- PCL support query succeeds;
- Reflex support query succeeds;
- `slReflexGetState` is acquired/used;
- `slReflexSleep` is acquired/used;
- `slReflexSetOptions` is acquired/used;
- XeFG Reflex synchronization IDs are not permanently stuck at `0`.

Do not add the POC diagnostic framework back into production solely to obtain these checks. Use existing logs/debug instrumentation or temporary local instrumentation that is removed before the final PR if additional evidence is needed.

---

## 11. Negative / Regression Validation

At minimum check the following behavior matrix.

### AMD

- `FixSlReflexAvailabilityOnIntel` may be present by executable, but the selective DXGI identity path must not activate because the primary GPU is not Intel.
- Existing behavior remains unchanged.

### NVIDIA

- selective Intel path must not activate;
- native NVIDIA behavior remains unchanged.

### Dragon's Dogma 2 (`dd2.exe`)

- no new Reflex selective DXGI quirk;
- no behavioral change.

### Monster Hunter Wilds (`monsterhunterwilds.exe`)

- no new Reflex selective DXGI quirk;
- no behavioral change.

### Other games

- no target quirk => no selective game-caller identity spoofing.

### Global `Dxgi=true`

- existing broad DXGI spoofing remains authoritative;
- selective Reflex path is disabled;
- no double-application requirement is introduced.

### `StreamlineSpoofing=false`

- selective Reflex path is disabled.

### `SkipSpoofing()==true`

- selective Reflex path is disabled.

---

## 12. Performance Validation

The purpose of the selective quirk is specifically to avoid the broad behavior and overhead/regressions associated with global `Dxgi=true`.

Compare:

1. normal baseline: `Dxgi=false` without the production quirk behavior;
2. candidate: `Dxgi=false` + automatic target-game selective quirk.

The candidate should behave/perf like the normal `Dxgi=false` path while restoring Reflex.

Do not use global `Dxgi=true` as the only performance baseline.

`GetDesc*` is not expected to be a per-frame hot path, but avoid introducing unrelated policy work, generic caller routing, or broad refactors for every application.

---

## 13. Tests / Static Verification

Add focused tests only where the current repository test structure already provides a natural place for quirk/policy coverage.

Do not create a large new testing framework for this small production fix.

At minimum, verify in code review that the eligibility logic has explicit coverage for the following truth table:

| Quirk | GPU | SL spoof | Global DXGI | Skip spoof | Game caller | Apply selective identity |
|---|---|---|---|---|---|---|
| off | Intel | on | off | off | yes | no |
| on | AMD | on | off | off | yes | no |
| on | NVIDIA | on | off | off | yes | no |
| on | Intel | off | off | off | yes | no |
| on | Intel | on | on | off | yes | no |
| on | Intel | on | off | on | yes | no |
| on | Intel | on | off | off | no | no |
| on | Intel | on | off | off | yes | yes |

Run the normal formatting/build/test checks expected by the repository and report any environment limitation accurately.

---

## 14. Review Guardrails

During implementation/review, treat the following as blockers:

- production branch is based on `poc_reflex` instead of fresh `master`;
- POC selector/config files enter the diff;
- Streamline or NVAPI availability forcing is retained;
- `sl.common.dll` is spoofed by the production quirk;
- the quirk activates on AMD/NVIDIA;
- global `Dxgi=true` is silently enabled;
- caller filtering is removed or broadened beyond the current game executable;
- unrelated DXGI/XeFG/swapchain refactoring enters the PR;
- only one of the four adapter `GetDesc*` interfaces receives the fix without a specific reason;
- `AttachToAdapter()` still refuses to install `GetDesc*` hooks when the eligible quirk is the only reason hooks are needed;
- RE9 or PRAGMATA requires a user-facing diagnostic selector for the final behavior.

Do **not** block the PR for speculative hardening that is not used by existing OptiScaler quirk patterns and is not required by the reproduced issue. In particular, do not introduce a new LUID-based quirk restriction solely for this implementation.

---

## 15. Expected Final Diff

A good final PR should look approximately like this:

```text
OptiScaler/misc/Quirks.h
  + one deep-code quirk enum
  + add quirk to re9.exe
  + add quirk to pragmata.exe

OptiScaler/dllmain.cpp
  + one printQuirks entry

OptiScaler/spoofing/Dxgi_Spoofing.cpp
  + narrow Intel/SL/quirk eligibility helper
  + game-caller check
  + selective descriptor identity application to GetDesc/GetDesc1/GetDesc2/GetDesc3
  + allow AttachToAdapter hooks when this quirk is eligible
```

No config file changes.  
No new POC helper files.  
No Streamline hook changes.  
No NVAPI changes.  
No swapchain changes.

---

## 16. PR Description Guidance

Keep the PR description focused on the observable compatibility problem and confirmed narrow fix.

Suggested summary:

> Restore Reflex availability for RE9 and PRAGMATA on Intel GPUs by applying the configured NVIDIA DXGI adapter identity only to adapter-description queries originating from the affected game executable. The fix is implemented as an Intel-only game quirk and keeps global DXGI spoofing disabled, so Streamline/Intel runtime and unrelated DXGI callers continue to see the real adapter identity.

Suggested key points:

- affected games: `re9.exe`, `pragmata.exe`;
- Intel only;
- requires normal Streamline spoofing path;
- global DXGI spoof remains off;
- POC `GameOnly` behavior validated on both games;
- no Streamline support-result forcing;
- no NVAPI availability forcing;
- no POC config selector shipped.

---

## 17. Completion Criteria

Implementation is ready for PR review when all of the following are true:

- [ ] fresh branch created from latest `master`;
- [ ] `FixSlReflexAvailabilityOnIntel` added as a normal deep-code quirk;
- [ ] quirk assigned to `re9.exe` and `pragmata.exe` only;
- [ ] quirk is printed through existing `printQuirks()` behavior;
- [ ] selective path requires real primary Intel GPU;
- [ ] selective path requires Streamline spoofing enabled;
- [ ] selective path requires global DXGI spoofing disabled;
- [ ] selective path honors `SkipSpoofing()`;
- [ ] selective path requires caller module == running target game EXE;
- [ ] all four `GetDesc*` variants are handled;
- [ ] `AttachToAdapter()` installs the adapter hooks when the quirk is the only DXGI spoofing requirement;
- [ ] existing broad `Dxgi=true` behavior is not refactored or changed unnecessarily;
- [ ] no POC config/diagnostic framework enters production diff;
- [ ] no LUID-specific quirk policy is introduced;
- [ ] PRAGMATA production build validates Reflex with `Dxgi=false`;
- [ ] RE9 production build validates Reflex with `Dxgi=false`;
- [ ] AMD/NVIDIA and non-target regression checks pass;
- [ ] DD2/MHW behavior remains unchanged;
- [ ] performance remains comparable to the normal `Dxgi=false` baseline;
- [ ] formatting/build/tests complete according to repository policy;
- [ ] final diff remains limited to the intended production scope.

---

## 18. Final Implementation Principle

This PR is not a general DXGI spoofing redesign.

It is the production extraction of one experimentally confirmed compatibility fact:

> RE9 and PRAGMATA need their own game executable to see OptiScaler's configured NVIDIA DXGI adapter identity on Intel in order to enter the normal Reflex path; the rest of the process does not need that spoofed identity.

Implement exactly that boundary using the existing OptiScaler quirk model.