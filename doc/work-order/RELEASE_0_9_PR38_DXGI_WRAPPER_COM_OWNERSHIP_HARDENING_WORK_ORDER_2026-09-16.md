# Release 0.9 — PR38 DXGI Wrapper COM Ownership Hardening Work Order

**Status:** Implementation work order  
**Target branch:** `reframework-0.9`  
**Reviewed branch tip:** `b0385ad6b3a34f78a55ee055fcef13056e20bde1`  
**Parent implementation:** PR37, `b0385ad6b3a34f78a55ee055fcef13056e20bde1` (`fix: preserve outer XeFG frame transaction through EvaluateState (#37)`)  
**Audit source:** `doc/work-order/RELEASE_0_9_INTEL_MHW_E_ABORT_4004_STREAMLINE_XEFG_FRAME_ORDER_AUDIT_REPORT_2026-09-16.md`  
**Audit finding:** `SDLX-008` (DXGI-wrapper portion only)  
**Date:** 2026-09-16

---

## 1. Objective

Implement the smallest safe ownership correction for the **DXGI-wrapper portion of SDLX-008**.

The current `reframework-0.9` source contains multiple COM ownership-contract violations in the wrapped swapchain path:

- interfaces obtained with `QueryInterface` are released before their last use;
- Streamline proxy unwrapping returns an interface after dropping the QI reference that made the returned pointer valid;
- persistent `_real1` / `_real2` / `_real3` / `_real4` swapchain capability pointers are retained after their QI references are immediately released.

These are source-level lifetime defects independent of whether they caused the Intel MHW `E_ABORT 4004` capture.

PR38 must restore the invariant:

> Every interface reference acquired by `QueryInterface` remains owned until the final use of that interface pointer, and every owned reference is released exactly once.

This is an **ownership-only hardening PR**. It must not redesign frame ordering, queue selection, GPU synchronization, swapchain lifecycle, or REFramework integration.

---

## 2. Why PR38 is next

PR36 and PR37 now establish the intended Streamline-to-XeFG CPU transaction and correct the SDLX-015 caller/callee ownership violation.

Before relying on long-duration logging-OFF MHW A/B results, an independent crash-capable class of COM lifetime defects should be removed from the active wrapper path so that it does not contaminate causal testing.

The audit classified SDLX-008 as Critical source impact, but did **not** prove it was the captured E_ABORT root. Preserve that distinction in code comments, PR text, and validation conclusions.

---

## 3. Pinned current-source evidence

All source statements below are against:

```text
b0385ad6b3a34f78a55ee055fcef13056e20bde1
```

### 3.1 `WaitForGPUIdle()` releases the queue before later use

Current shape in `OptiScaler/wrapped/wrapped_swapchain.cpp`:

```cpp
ID3D12CommandQueue* queue = nullptr;

if (object->QueryInterface(IID_PPV_ARGS(&queue)) == S_OK)
{
    LOG_DEBUG("Command queue obtained for GPU idle wait");
    queue->Release();
}

...
queue->Signal(resizeFence, resizeFenceValue);
```

The QI reference is dropped before `Signal()`.

Important separate observation from the current source:

```cpp
static ID3D12Fence* resizeFence = nullptr;
static HANDLE resizeFenceEvent = nullptr;

if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
{
    // code that recreates resizeFence / resizeFenceEvent
}
```

Because the two static objects start as `nullptr`, the current first-initialization path appears unreachable within this file. That is a separate functional/fence issue related to SDLX-011.

**PR38 must not fix or activate that logic.** Only correct the queue-reference ownership so the code is valid if/when the path is reachable. Fence initialization, HRESULT handling, timeout policy, and fail-open/fail-closed behavior belong to a later PR.

### 3.2 `LocalPresent()` D3D11 path releases `device` before continued use

Current shape:

```cpp
ID3D11Device* device = nullptr;

if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
{
    isD3D11 = true;
    device->Release();

    ...
    State::Instance().currentD3D11Device = device;

    if (!State::Instance().DeviceAdapterNames.contains(device))
    {
        IDXGIDevice* dxgiDevice = nullptr;
        auto qResult = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        ...
    }
}
```

The same released pointer can later be used for `GetImmediateContext()` in the upscaler timing path.

This D3D11 instance is part of the same ownership defect class and should be corrected in PR38 even though the audit narrative focused mainly on the D3D12 path.

### 3.3 `LocalPresent()` D3D12 path releases the queue before unwrapping and use

Current shape:

```cpp
ID3D12CommandQueue* cq = nullptr;

if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
{
    cq->Release();

    ID3D12CommandQueue* realQueue = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cq, (IUnknown**) &realQueue))
        cq = realQueue;

    ...

    if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
    {
        device12->Release();
        ...
        State::Instance().currentD3D12Device = device12;
        D3D12Hooks::HookDevice(device12);
    }
}
```

The queried command queue is released before `CheckForRealObject`, `GetDevice`, and later upscaler-timing use.

The queried D3D12 device is likewise released before `HookDevice()` and before its pointer is published as a raw `State` alias.

### 3.4 `Util::CheckForRealObject()` returns a pointer after releasing its QI reference

Current implementation in `OptiScaler/Util.cpp`:

```cpp
auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

if (qResult == S_OK && *ppRealObject != nullptr)
{
    LOG_INFO("{} Streamline proxy found!", functionName);
    (*ppRealObject)->Release();
    return true;
}
```

`LocalPresent()` then treats that returned pointer as the D3D12 command queue.

The implementation therefore needs an ownership-aware unwrapping path. Do not fix this by performing `AddRef()` on the pointer after this helper returns; the helper has already dropped the QI reference and that would still rely on an invalid lifetime assumption.

### 3.5 Persistent wrapped-swapchain capability interfaces are retained after immediate Release

Current constructor:

```cpp
_real->QueryInterface(IID_PPV_ARGS(&_real1));
if (_real1 != nullptr)
    _real1->Release();

_real->QueryInterface(IID_PPV_ARGS(&_real2));
if (_real2 != nullptr)
    _real2->Release();

_real->QueryInterface(IID_PPV_ARGS(&_real3));
if (_real3 != nullptr)
    _real3->Release();

_real->QueryInterface(IID_PPV_ARGS(&_real4));
if (_real4 != nullptr)
    _real4->Release();
```

These members are later dereferenced throughout the wrapper lifetime for methods including:

- `IDXGISwapChain1`: `GetDesc1`, `Present1`, background color, rotation, etc.;
- `IDXGISwapChain2`: source size, latency, matrix transform, etc.;
- `IDXGISwapChain3`: backbuffer index, color space, `ResizeBuffers1`, etc.;
- `IDXGISwapChain4`: HDR metadata.

The base `_real` pointer may keep the underlying DXGI object alive in common implementations, but that does not satisfy the COM ownership contract for retained interface pointers. A queried interface intended for later use must retain its own acquired reference until that use is finished.

### 3.6 PR34/PR35 final-release ordering must remain intact

Current wrapper final release intentionally does the following:

```text
wrapper terminal-owner guard
  -> snapshot under wrapper local mutex
  -> release wrapper local mutex
  -> XeFG / REF retirement outside wrapper local mutex
  -> generation cleanup
  -> real->Release()
  -> delete wrapper
```

Do not move XeFG/REF retirement back under `_localMutex` and do not remove the reentrant terminal-zero handling added by PR34.

PR38 may adjust the release of owned capability interfaces, but the final underlying `_real` reference must remain valid until all capability references are no longer needed and the existing lifecycle retirement is complete.

---

## 4. Required pre-change audit

Before editing, the implementation agent must record in the PR description:

1. every current caller of `Util::CheckForRealObject()` at the PR base;
2. whether each caller expects a borrowed or owned returned pointer;
3. every QI-acquired pointer in `LocalPresent()` and the final point at which that pointer is used;
4. every `_real1.._real4` member use and the final wrapper-retirement path;
5. confirmation that no code path intentionally transfers COM release responsibility to another component.

Do not silently change `CheckForRealObject()` semantics globally unless all callers are migrated and ownership is proven.

---

## 5. Required implementation

### 5.1 Add an explicitly owned Streamline real-object query

Preferred approach: add a new helper with an ownership-explicit name, for example:

```cpp
bool QueryRealObjectOwned(std::string functionName, IUnknown* object, IUnknown** outRealObject);
```

Required contract:

- initialize `*outRealObject = nullptr` before querying;
- on successful return, the caller owns exactly one QI reference;
- the helper must **not** release that returned reference;
- on failure, the output must be null;
- document the ownership rule at the declaration.

Illustrative implementation:

```cpp
bool Util::QueryRealObjectOwned(std::string functionName, IUnknown* object, IUnknown** outRealObject)
{
    if (object == nullptr || outRealObject == nullptr)
        return false;

    *outRealObject = nullptr;

    if (streamlineRiid.Data1 == 0)
    {
        const auto iidResult = IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid);
        if (iidResult != S_OK)
            return false;
    }

    const auto qResult = object->QueryInterface(streamlineRiid, reinterpret_cast<void**>(outRealObject));
    if (qResult != S_OK || *outRealObject == nullptr)
    {
        *outRealObject = nullptr;
        return false;
    }

    LOG_INFO("{} Streamline proxy found!", functionName);
    return true; // caller owns the QI reference
}
```

If the existing `CheckForRealObject()` has other callers, preserve its old borrowed behavior or migrate every caller deliberately. Do not create an ambiguous API where ownership depends on an undocumented boolean parameter.

### 5.2 Make `LocalPresent()` QI references RAII-owned through final local use

Use `Microsoft::WRL::ComPtr` or an equivalently explicit RAII owner for transient QI references.

The intended shape is approximately:

```cpp
Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> queriedQueue;
Microsoft::WRL::ComPtr<IUnknown> realQueueOwner;
Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;

ID3D12CommandQueue* queueForUse = nullptr;
```

D3D11 path:

```cpp
if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(d3d11Device.GetAddressOf()))))
{
    auto* device = d3d11Device.Get();

    // All existing uses of device remain inside the lifetime of d3d11Device.
    State::Instance().currentD3D11Device = device; // existing raw alias semantics preserved
    ...
}
```

D3D12 path:

```cpp
if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(queriedQueue.GetAddressOf()))))
{
    queueForUse = queriedQueue.Get();

    IUnknown* ownedRealObject = nullptr;
    if (Util::QueryRealObjectOwned(__FUNCTION__, queriedQueue.Get(), &ownedRealObject))
    {
        realQueueOwner.Attach(ownedRealObject); // takes ownership of that one QI reference
        queueForUse = reinterpret_cast<ID3D12CommandQueue*>(realQueueOwner.Get());
    }

    State::Instance().currentCommandQueue = queueForUse; // preserve current raw alias semantics

    if (SUCCEEDED(queueForUse->GetDevice(IID_PPV_ARGS(d3d12Device.GetAddressOf()))))
    {
        State::Instance().currentD3D12Device = d3d12Device.Get();
        D3D12Hooks::HookDevice(d3d12Device.Get());
    }

    ...
}
```

The exact WRL syntax may differ, but the ownership rules must not.

Important constraints:

- the proxy queue QI reference must remain valid while it is used;
- an unwrapped real-object reference must remain owned while `queueForUse` points to it;
- the D3D12 device reference must remain owned through `HookDevice()` and all local uses;
- early returns, including the DXVK path, must not leak references;
- do not add per-frame explicit `Release()` calls that can be skipped on an early return; RAII is preferred.

`State::currentD3D11Device`, `State::currentD3D12Device`, and `State::currentCommandQueue` are currently raw/non-owning aliases. PR38 must **not** convert global `State` ownership wholesale. This PR guarantees local-use lifetime only; broader State lifetime is a separate architectural question.

### 5.3 Correct `WaitForGPUIdle()` queue ownership without changing its behavior

Use an owned local queue reference:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))))
{
    LOG_DEBUG("Command queue obtained for GPU idle wait");
}

if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
{
    ...
    queue->Signal(resizeFence, resizeFenceValue);
}
```

Do **not** in PR38:

- change the `resizeFence != nullptr && resizeFenceEvent != nullptr` gate;
- create the initial fence/event on a previously unreachable path;
- change timeout duration;
- add fail-closed resize behavior;
- redesign fence ownership;
- change queue selection.

Those belong to the SDLX-011 follow-up so the behavioral effect can be reviewed separately.

### 5.4 Retain `_real1.._real4` QI references for the wrapper lifetime

The constructor must no longer immediately release the successful capability QI references.

Preferred minimal approach: keep the existing raw members but make their ownership explicit and release them exactly once during final wrapper retirement.

For example, add a private helper:

```cpp
void WrappedIDXGISwapChain4::ReleaseRealInterfaceRefs() noexcept
{
    const auto releaseAndNull = [](auto*& value)
    {
        if (value != nullptr)
        {
            value->Release();
            value = nullptr;
        }
    };

    releaseAndNull(_real4);
    releaseAndNull(_real3);
    releaseAndNull(_real2);
    releaseAndNull(_real1);
}
```

Constructor shape:

```cpp
_real->QueryInterface(IID_PPV_ARGS(&_real1));
_real->QueryInterface(IID_PPV_ARGS(&_real2));
_real->QueryInterface(IID_PPV_ARGS(&_real3));
_real->QueryInterface(IID_PPV_ARGS(&_real4));
```

Do not call `Release()` immediately after those QIs.

Final release ordering must be:

```text
existing PR34/PR35 lifecycle retirement
  -> generation/state cleanup
  -> release/null _real1.._real4 owned capability refs
  -> release the final local `real` / `_real` reference
  -> delete wrapper
```

This order keeps the base real swapchain alive while the capability refs are released and preserves the existing terminal underlying `real->Release()` position.

The destructor should defensively call the same release helper so an abnormal future delete path cannot leak interfaces. Because the final-release path must null the members first, the destructor call should be a no-op in the normal path and must not double-release.

An equivalent `ComPtr` member implementation is acceptable only if it explicitly resets the capability refs **before** the final `real->Release()`; do not accidentally defer the real swapchain's final destruction until after lifecycle-complete logging or after wrapper deletion.

---

## 6. Required final ownership model

After PR38, the ownership model must be documented in the PR description as:

```text
LocalPresent transient QI
  queried interface ref
      -> RAII owner
      -> valid through final local use
      -> automatic release

Streamline real-object unwrap
  special QI
      -> caller-owned reference
      -> RAII owner in LocalPresent
      -> release after final queue use

Wrapped swapchain capability interfaces
  _real->QI(_real1.._real4)
      -> wrapper-owned refs
      -> valid for wrapper lifetime
      -> released once during terminal wrapper retirement
      -> base `real` released last
```

A raw pointer may still be published into existing `State` fields, but PR38 must not describe those aliases as owned references.

---

## 7. Explicitly out of scope

PR38 must **not** include:

- `Reflex_Hooks.cpp` static `device12` cache correction — track as the remaining SDLX-008 follow-up;
- the SDLX-011 `WaitForGPUIdle()` initialization / HRESULT / timeout redesign;
- `IFGFeature_Dx12::HasResource()` or `GetResource()` changes;
- frame-ring or frame-ID changes;
- GPU producer/consumer queue or fence changes;
- Streamline/DLSSG completion-fence waits;
- queue selection changes;
- `State` global raw-pointer-to-`ComPtr` conversion;
- PR34 final-release reentrancy changes;
- PR35 shutdown/pre-retire changes;
- PR36/PR37 frame-transaction changes;
- REFramework changes;
- Intel, NVIDIA, Capcom, or MHW-specific heuristics;
- sleeps, yields, logger delays, or logging-as-synchronization workarounds.

If implementation reveals that one of these is required for correctness, stop and document the dependency instead of silently expanding PR38.

---

## 8. Expected changed files

Expected:

```text
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/wrapped/wrapped_swapchain.h
OptiScaler/Util.cpp
OptiScaler/Util.h
```

Fewer files are acceptable if the implementation can prove equivalent ownership safely.

More files require an explicit justification in the PR description.

---

## 9. Mandatory static validation

Before opening the PR:

1. `clang-format --dry-run --Werror` on every changed C/C++ file;
2. `git diff --check`;
3. Release x64 build of `OptiScaler.sln`;
4. enumerate the final changed-file set;
5. verify there is no immediate `Release()` followed by later use of the same QI-acquired local pointer in `LocalPresent()`;
6. verify `WaitForGPUIdle()` holds its queue reference through `Signal()` while preserving the existing fence gate exactly;
7. verify every successful `QueryRealObjectOwned` call transfers one owned reference into an RAII object and releases it once;
8. verify `_real1.._real4` successful QIs are balanced by exactly one terminal release each;
9. verify `_real1.._real4` are reset before final `real->Release()`;
10. verify PR34's `_finalReleaseInProgress` and reentrant-final-release logic is byte-for-byte unchanged unless formatting context forces movement;
11. verify PR35 shutdown/pre-retire behavior is unchanged;
12. verify PR36/PR37 transaction code is untouched.

Do not use aggregate AddRef/Release counters as proof of correct ownership. Per-reference lifetime is the invariant.

---

## 10. Required runtime validation for PR38

PR38 is wrapper/lifetime hardening, so runtime coverage must emphasize Present, resize, and teardown rather than claiming the E_ABORT root is solved.

### 10.1 Intel MHW + REF + XeFG

Minimum focused smoke:

- 2 independent launches;
- OptiScaler logging OFF for at least one run;
- reach active XeFG gameplay;
- perform at least one resize / display-mode transition if practical;
- clean exit;
- no Fatal D3D error;
- no REF exception dump;
- no Present hang;
- no shutdown hang.

### 10.2 NVIDIA MHW regression

At least 3 clean exits preserving PR34/PR35 behavior.

Specifically verify no regression in:

```text
final_release_begin
final_release_fg_retire_begin
ref_pre_retire_* or shutdown skip as applicable
final_release_fg_retire_complete
final_release_complete
```

Do not require exact refcount values to match old logs; PR38 intentionally retains capability references until terminal release. Instead verify that all wrapper-owned capability refs are released before the final base real-swapchain release and that shutdown completes cleanly.

### 10.3 Generic DX12 non-REF smoke

At least one DX12 title or test path without REFramework:

- Present works;
- overlay path works if enabled;
- resize works;
- clean exit.

### 10.4 DX11 smoke

Because PR38 also fixes the newly confirmed D3D11 `LocalPresent()` release-before-use pattern, run at least one DX11 title/path:

- Present works;
- adapter/device discovery still works;
- upscaler timing path does not crash if exercised;
- clean exit.

### 10.5 Full Intel MHW causal matrix remains deferred

Do not claim the required `5 x >=10 min` logging-OFF E_ABORT causal matrix as complete solely from PR38.

The Reflex marker-device ownership defect remains outside PR38 and should be corrected in the next ownership follow-up before treating SDLX-008 as fully closed.

---

## 11. PR description requirements

The PR body must include:

- exact base SHA;
- exact head SHA;
- changed files;
- pre-change ownership audit for every touched QI result;
- `CheckForRealObject()` caller inventory;
- final ownership table showing acquire site, owner, final use, and release site;
- confirmation that `WaitForGPUIdle()` fence behavior was not activated or redesigned;
- confirmation that PR34/35/36/37 behavior is untouched;
- build/format results;
- runtime tests actually performed;
- tests not performed explicitly marked `NOT_EXERCISED`;
- statement that PR38 does not prove the MHW E_ABORT root cause.

Recommended PR title:

```text
fix: retain DXGI wrapper COM references through final use
```

---

## 12. Review blockers

Do not merge PR38 if any of the following is true:

- a QI result is still released before its final dereference;
- the new real-object helper returns a pointer without a retained QI reference;
- an owned real-object reference can leak on an early return;
- `_real1.._real4` remain borrowed after the constructor;
- capability-interface refs survive past final base `real->Release()` unintentionally;
- capability refs can be double-released by both final release and destructor;
- `WaitForGPUIdle()` behavior changes beyond ownership;
- the PR changes GPU queue selection or fence ordering;
- the PR changes PR34/PR35/PR36/PR37 lifecycle/transaction behavior;
- the implementation broadens into global `State` ownership without a separate design review.

---

## 13. Follow-up sequence after PR38

PR38 intentionally does not close all audit findings.

Recommended next sequence:

```text
PR38
  DXGI wrapper COM ownership
      -> LocalPresent D3D11/D3D12
      -> owned Streamline real-object unwrap
      -> WaitForGPUIdle queue reference only
      -> _real1.._real4 wrapper refs

PR39
  Reflex/DLSSG marker D3D12 device ownership
      -> remove released static device12 cache
      -> define bounded cache/reset lifetime

PR40 (or separate focused work)
  SDLX-011 GPU-idle helper behavior
      -> first initialization
      -> HRESULT checks
      -> timeout semantics
      -> fail-open/fail-closed decision

Then
  Intel MHW logging-OFF causal A/B
```

Do not combine the above merely to reduce PR count. Each phase changes a different correctness contract and must remain independently reviewable.

---

## 14. Definition of done

PR38 is complete when:

1. all touched DXGI-wrapper QI results are owned through final use;
2. Streamline real-object unwrapping has an explicit caller-owned reference contract;
3. `LocalPresent()` no longer dereferences D3D11/D3D12 interfaces after dropping its own QI reference;
4. `WaitForGPUIdle()` no longer drops its queue QI reference before later queue use, without otherwise changing behavior;
5. `_real1.._real4` remain valid under owned references for the wrapper lifetime and are released exactly once before the final base real-swapchain release;
6. PR34/PR35/PR36/PR37 behavior is preserved;
7. Release x64 build and formatting pass;
8. focused DX11, DX12, and MHW regression coverage is reported honestly;
9. no claim is made that PR38 alone fixes or proves the Intel MHW `E_ABORT 4004` root cause.
