# Release 0.9 — PR38 Capcom DX12 Wrapper COM Ownership Hardening Work Order

**Status:** Implementation work order  
**Target branch:** `reframework-0.9`  
**Reviewed code tip:** `b0385ad6b3a34f78a55ee055fcef13056e20bde1`  
**Parent implementation:** PR37, `b0385ad6b3a34f78a55ee055fcef13056e20bde1` (`fix: preserve outer XeFG frame transaction through EvaluateState (#37)`)  
**Audit source:** `doc/work-order/RELEASE_0_9_INTEL_MHW_E_ABORT_4004_STREAMLINE_XEFG_FRAME_ORDER_AUDIT_REPORT_2026-09-16.md`  
**Audit finding:** `SDLX-008` — **DX12 / wrapped-swapchain portion only**  
**Primary target:** Capcom RE Engine DX12 games using REFramework + OptiScaler + XeFG  
**Date:** 2026-09-16

---

## 1. Objective

Implement the smallest safe COM ownership correction for the **active Capcom DX12 wrapper path** identified by audit finding `SDLX-008`.

The current `reframework-0.9` source contains several source-level lifetime defects in the DX12 wrapped-swapchain path:

- a D3D12 command queue obtained with `QueryInterface` is released before its final use;
- a D3D12 device obtained from the queue is released before `HookDevice()` and later local use;
- Streamline proxy unwrapping returns a real object after dropping the QI reference that made the returned pointer valid;
- persistent `_real1` / `_real2` / `_real3` / `_real4` DXGI swapchain capability pointers are kept after their QI references are immediately released;
- `WaitForGPUIdle()` releases its queried D3D12 command queue before a later `Signal()`.

PR38 must restore this invariant:

> Every COM interface reference acquired by `QueryInterface` on the Capcom DX12 path remains owned until its final use, and every owned reference is released exactly once.

This PR is **DX12-focused ownership hardening only**. It must not broaden into unrelated API cleanup.

---

## 2. Scope decision: Capcom DX12 only

The current target set for this investigation is Capcom RE Engine games running DX12. Therefore:

- **Do not modify the D3D11 branch in `LocalPresent()` in PR38.**
- **Do not add D3D11 validation requirements.**
- A similar D3D11 lifetime defect may exist, but it is not part of the current Capcom / REFramework / XeFG causal path and should not be mixed into this PR.

Keeping PR38 DX12-only preserves causal clarity for the Intel MHW `E_ABORT 4004` investigation and reduces unrelated regression surface.

---

## 3. Why PR38 is next

PR36 and PR37 now establish the intended Streamline-to-XeFG CPU transaction and fix the SDLX-015 caller/callee ownership violation.

Before relying on long-duration logging-OFF MHW A/B results, remove independent crash-capable COM lifetime defects from the active DX12 wrapper path so they do not contaminate causal testing.

The audit classified SDLX-008 as Critical source impact, but did **not** prove it caused the captured `E_ABORT 4004`. Preserve that distinction in code comments, PR text, and validation conclusions.

---

## 4. Pinned current-source evidence

All source statements below are against:

```text
b0385ad6b3a34f78a55ee055fcef13056e20bde1
```

### 4.1 `LocalPresent()` releases the D3D12 command queue before later use

Current shape in `OptiScaler/wrapped/wrapped_swapchain.cpp`:

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

The queue QI reference is dropped before:

- Streamline proxy unwrapping;
- `GetDevice()`;
- `State::currentCommandQueue` publication;
- local DX12 timing use.

The D3D12 device QI reference is likewise dropped before `HookDevice()` and before later local use.

### 4.2 `Util::CheckForRealObject()` returns a pointer after releasing its QI reference

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

`LocalPresent()` then treats the returned object as the usable D3D12 command queue.

The returned pointer therefore no longer owns the QI reference that established its lifetime.

### 4.3 `WaitForGPUIdle()` releases the D3D12 queue before later use

Current shape:

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

The QI reference is released before `Signal()`.

Important separate observation:

```cpp
static ID3D12Fence* resizeFence = nullptr;
static HANDLE resizeFenceEvent = nullptr;

if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
{
    // fence/event recreation logic
}
```

Because the static fence/event start as null, the initial creation path appears unreachable within this file. That is a separate functional issue related to SDLX-011.

**PR38 must not fix or activate that behavior.** Only make queue ownership valid if/when the current path executes.

### 4.4 Persistent wrapped-swapchain capability interfaces are retained after immediate Release

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

These members are later dereferenced throughout the wrapper lifetime for `Present1`, `ResizeBuffers1`, color-space operations, HDR metadata, latency, and other DXGI swapchain operations used by the DX12 path.

The base `_real` pointer may keep the underlying COM object alive in common implementations, but retaining a queried interface pointer after releasing its own QI reference does not satisfy the ownership contract.

### 4.5 PR34 / PR35 wrapper final-release ordering must remain intact

Current final-release ordering is intentionally:

```text
wrapper terminal-owner / reentrancy guard
  -> snapshot under wrapper local mutex
  -> release wrapper local mutex
  -> XeFG / REF retirement outside wrapper local mutex
  -> generation/state cleanup
  -> final underlying real->Release()
  -> delete wrapper
```

PR38 must not move XeFG / REF retirement back under `_localMutex`, must not remove the PR34 terminal-zero reentrancy handling, and must not change the PR35 shutdown / pre-retire contract.

---

## 5. Required pre-change audit

Before editing, record in the PR description:

1. every current caller of `Util::CheckForRealObject()` on the PR base;
2. for each caller, whether the returned object is treated as borrowed or owned;
3. every QI-acquired pointer in the **DX12 branch** of `LocalPresent()` and its final local use;
4. every `_real1.._real4` use and the final wrapper-retirement path;
5. confirmation that no current DX12 caller intentionally transfers release responsibility into another component.

Do not silently change `CheckForRealObject()` semantics globally unless all callers are audited and migrated deliberately.

---

## 6. Required implementation

### 6.1 Add an explicitly owned Streamline real-object query

Preferred approach: add a new ownership-explicit helper, for example:

```cpp
bool QueryRealObjectOwned(std::string functionName, IUnknown* object, IUnknown** outRealObject);
```

Required contract:

- validate `object` and `outRealObject`;
- initialize `*outRealObject = nullptr` before querying;
- on success, the caller owns exactly one QI reference;
- the helper must **not** release the returned reference;
- on failure, output remains null;
- document the ownership contract in `Util.h`.

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

If the existing `CheckForRealObject()` has unrelated callers, preserve its existing semantics unless those callers are explicitly migrated and reviewed in the same PR.

### 6.2 Make the DX12 `LocalPresent()` references RAII-owned through final local use

Use `Microsoft::WRL::ComPtr` or equivalent explicit RAII for transient DX12 references.

Illustrative shape:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> queriedQueue;
Microsoft::WRL::ComPtr<IUnknown> realQueueOwner;
Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;

ID3D12CommandQueue* queueForUse = nullptr;

if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(queriedQueue.GetAddressOf()))))
{
    queueForUse = queriedQueue.Get();

    IUnknown* ownedRealObject = nullptr;
    if (Util::QueryRealObjectOwned(__FUNCTION__, queriedQueue.Get(), &ownedRealObject))
    {
        realQueueOwner.Attach(ownedRealObject);
        queueForUse = reinterpret_cast<ID3D12CommandQueue*>(realQueueOwner.Get());
    }

    if (State::Instance().currentCommandQueue == nullptr)
        State::Instance().currentCommandQueue = queueForUse;

    if (SUCCEEDED(queueForUse->GetDevice(IID_PPV_ARGS(d3d12Device.GetAddressOf()))))
    {
        State::Instance().currentD3D12Device = d3d12Device.Get();
        D3D12Hooks::HookDevice(d3d12Device.Get());
    }

    // Existing local DX12 uses occur while the RAII owners remain alive.
}
```

The exact implementation may differ, but these invariants are mandatory:

- the original queue QI reference remains alive while that queue pointer is used;
- if Streamline returns a real queue, the returned QI reference remains owned while `queueForUse` points to it;
- the D3D12 device reference remains alive through `HookDevice()` and every local use;
- early returns, including DXVK branches, must not leak owned references;
- do not add fragile manual per-frame `Release()` paths when RAII can express the ownership directly.

`State::currentCommandQueue` and `State::currentD3D12Device` are existing raw/non-owning aliases. PR38 must **not** redesign global `State` ownership. This PR guarantees the correctness of the active local transaction only; global lifetime architecture is separate work.

### 6.3 Correct `WaitForGPUIdle()` queue ownership without changing behavior

Use an owned local queue reference, for example:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))))
    LOG_DEBUG("Command queue obtained for GPU idle wait");

if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
{
    ...
    queue->Signal(resizeFence, resizeFenceValue);
}
```

Do **not** in PR38:

- change the `resizeFence != nullptr && resizeFenceEvent != nullptr` gate;
- create the first fence/event;
- change timeout duration;
- redesign fence ownership;
- change resize fail-open/fail-closed policy;
- change command-queue selection.

Those belong to the SDLX-011 follow-up.

### 6.4 Retain `_real1.._real4` QI references for the wrapper lifetime

Do not immediately release successful `_real1.._real4` QIs in the constructor.

Minimal acceptable direction:

```cpp
_real->QueryInterface(IID_PPV_ARGS(&_real1));
_real->QueryInterface(IID_PPV_ARGS(&_real2));
_real->QueryInterface(IID_PPV_ARGS(&_real3));
_real->QueryInterface(IID_PPV_ARGS(&_real4));
```

Add a single idempotent release helper, for example:

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

Required final-release ordering:

```text
existing PR34 / PR35 lifecycle retirement
  -> generation/state cleanup
  -> release/null _real1.._real4 owned capability refs
  -> release final underlying `real` reference
  -> delete wrapper
```

The destructor may defensively call the same idempotent helper, but the normal final-release path must null the members before `delete this` so the destructor cannot double-release.

---

## 7. Explicitly out of scope

PR38 must not modify:

- D3D11 `LocalPresent()` lifetime behavior;
- any D3D11 timing or device path;
- `Reflex_Hooks.cpp` static `device12` cache lifetime;
- SDLX-011 fence initialization / wait behavior;
- Streamline→XeFG frame-order logic from PR36 / PR37;
- `OwnedMutex` semantics;
- resource-map locking (`HasResource`, `GetResource`);
- frame-ring / ABA behavior;
- GPU queue/fence dependency design;
- REFramework code or ABI;
- PR34 / PR35 lifecycle policy;
- sleeps, logging padding, or timing heuristics.

If implementation uncovers a defect in one of these areas, document it for the next PR rather than expanding PR38.

---

## 8. Validation requirements

### 8.1 Static / source validation

Required:

```text
clang-format --dry-run --Werror <all changed C/C++ files>
git diff --check
```

Also verify:

- no QI-acquired DX12 queue is released before its last local use;
- no QI-acquired D3D12 device is released before `HookDevice()` / final local use;
- owned Streamline real-object results are released exactly once;
- `_real1.._real4` successful QIs remain owned until wrapper retirement;
- `_real1.._real4` are released/null exactly once before final underlying real release;
- PR34 reentrant final-release guard remains unchanged;
- PR35 REF/XeFG retirement ordering remains unchanged;
- no D3D11 source lines are modified unless required only for mechanical compilation and explicitly justified in the PR description.

### 8.2 Build validation

Required:

```text
MSBuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64
```

Record build result and resulting `OptiScaler.dll` SHA-256 in the PR description.

### 8.3 Focused runtime validation

When hardware/game access is available, run at minimum on one Capcom RE Engine DX12 title using the same REFramework + OptiScaler path:

- launch to gameplay;
- XeFG active;
- exercise at least one swapchain resize / resolution or display transition if safely reproducible;
- return to gameplay;
- clean exit;
- no wrapper final-release crash;
- no shutdown AV;
- no new Present hang;
- no COM ownership warning introduced by the changed path.

Intel MHW logging-OFF long-run causal testing remains a later acceptance step and must not be claimed as completed unless run on the PR38 binary.

---

## 9. Acceptance criteria

PR38 is ready for review only if all of the following are true:

1. DX12 `LocalPresent()` queue/device QI references remain owned through their final local use.
2. Streamline real-object unwrapping has an explicit owned-return contract.
3. `WaitForGPUIdle()` no longer releases its queried D3D12 queue before later use.
4. `_real1.._real4` QI references remain valid for the wrapper lifetime and are released exactly once during final retirement.
5. PR34 / PR35 lifecycle ordering is unchanged.
6. PR36 / PR37 transaction behavior is unchanged.
7. No D3D11 behavioral change is included.
8. No fence-initialization / SDLX-011 behavior change is included.
9. Release x64 build succeeds.
10. PR description clearly states that this is source-level COM lifetime hardening, **not proof that SDLX-008 caused E_ABORT 4004**.

---

## 10. Expected changed-file envelope

Expected files are approximately:

```text
OptiScaler/wrapped/wrapped_swapchain.cpp
OptiScaler/wrapped/wrapped_swapchain.h
OptiScaler/Util.cpp
OptiScaler/Util.h
```

A materially broader diff requires justification before merge.

---

## 11. Follow-up sequence

Do not merge these into PR38:

### PR39 — Reflex / DLSSG marker D3D12 device cache lifetime

Audit and repair the static `ID3D12Device* device12` cache in `Reflex_Hooks.cpp` separately because persistent cache ownership has a different lifetime contract from per-call wrapper temporaries.

### PR40 — SDLX-011 `WaitForGPUIdle()` functionality

Separately review:

- first fence/event creation;
- `CreateFence` / `CreateEvent` / `Signal` / `SetEventOnCompletion` HRESULT handling;
- timeout semantics;
- queue identity;
- resize fail-open policy;
- fence/event cleanup.

### After ownership hardening

Run the Intel MHW logging-OFF causal matrix against binaries that include PR36 + PR37 + the relevant DX12 ownership fixes, while keeping any later resource-map or GPU-dependency changes isolated into separate PRs.

---

## 12. PR title suggestion

```text
fix: harden DX12 wrapper COM ownership
```

The PR description must include the pre-change ownership audit, changed ownership contracts, build evidence, runtime status, and explicit out-of-scope list.