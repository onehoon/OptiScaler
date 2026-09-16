# Release 0.9 — SDLX-006 / SDLX-007 GPU Queue, Fence, and Resize-Queue Synchronization Audit Report

- Audit date: 2026-09-16 (KST)
- Target branch: reframework-0.9
- Audit instruction: doc/work-order/RELEASE_0_9_SDLX_006_007_GPU_QUEUE_FENCE_SYNCHRONIZATION_AUDIT_INSTRUCTION_2026-09-16.md
- Audit mode: source and public-contract audit only; no implementation or behavior change
- Final audited OptiScaler tip: cb69ca79e31abd72c4083aa247491179c6c9fc7e
- Pinned source baseline: 036aae89e31447b21e1cab4e8d2c5c72421f9dc7 (PR38 merge)
- Result: the source/API audit is complete. The Intel/XeFG internal GPU-consumer boundary remains explicitly UNPROVEN where the available artifacts do not expose it.

## 1. Executive verdict

| Goal | Verdict | Evidence |
|---|---|---|
| SDLX-006 — producer resource ordering to XeFG | RUNTIME-DEPENDENT CANDIDATE | The active Streamline path calls XeFG D3D12TagFrameResource and sets a CPU readiness bit without an OptiScaler producer-to-consumer GPU fence. A producer command list can be tagged and marked ready before the application submits it. The public XeFG contract requires tag command lists to be submitted before Present, but the current source does not enforce or observe that precondition. The available MHW artifacts do not identify the resource producer queue or a failing frame's GPU order. |
| SDLX-007 — queue identity, ResizeBuffers1, and generation ownership | SAFE BUT FRAGILE | XeFG's public contract says it retains the initialization queue and ignores queues passed through ResizeBuffers1, so the fact that ResizeBuffers1 does not update XeFG's owned queue is not by itself a defect. However, OptiScaler maintains several raw/owned queue aliases, only inspects ppPresentQueue[0] in the wrapper, does not record the complete array, and has a single SetCommandQueue path that ignores resource type. The topology is not globally proven safe. |
| MHW E_ABORT 4004 causal confidence | INSUFFICIENT EVIDENCE / UNPROVEN | CrashReport.txt contains Fatal D3D error (7, E_ABORT, 0x80004004) with an access violation, but that crash artifact has no matching OptiScaler.log. Four paired MHW sessions show successful ResizeBuffers1/Present paths and no logged DXGI/device-removal failure. They do show real distinct same-device XeFG queue topology, but no resource-level producer/fence correlation. |
| Code change before a new A/B | DIAGNOSTIC ONLY | No synchronization fix is justified from the current evidence. The next action should be a no-behavior-change buffered diagnostic that correlates one resource/frame through tagging, command-list execution, queue identities, readiness, SetPresentId, Dispatch, and Present. |

The audit therefore does not promote SDLX-006 or SDLX-007 directly to a confirmed MHW root cause. It does establish two source-level hazards: CPU readiness is not GPU completion, and queue ownership is represented by multiple aliases with no universal identity/generation proof.

## 2. Scope, method, and evidence boundary

### 2.1 Scope

The audit covered:

- Streamline tag entry points and DLSSG state forwarding;
- Sl_Inputs_Dx12::reportResource;
- XeFG_Dx12::SetResource, Dispatch, Present, SetPresentId, and command-queue ownership;
- ResTrack_Dx12 command-list registration, Close, ExecuteCommandLists, and readiness path;
- FGHooks resize fence and generation lifecycle;
- WrappedIDXGISwapChain4::ResizeBuffers1 and LocalPresent queue handling;
- State::currentCommandQueue and related aliases;
- checked-in Streamline and XeSS/XeFG headers and developer documentation;
- companion REFramework source and the supplied MHW runtime artifacts;
- all relevant CreateFence, Signal, Wait, SetEventOnCompletion, GetCompletedValue, ExecuteCommandLists, and D3D12TagFrameResource search hits.

No source, configuration, REFramework, log, or binary was modified during the audit. No synchronization, wait, queue substitution, sleep, logging, or mutex change was implemented.

### 2.2 Evidence labels

- SOURCE-CONFIRMED: directly established by the audited source.
- API-CONTRACT-CONFIRMED: directly stated by a checked-in or official API contract.
- RUNTIME-OBSERVED: present in a supplied runtime artifact.
- RUNTIME-CANDIDATE: runtime topology supports a condition, but the causal frame edge is not captured.
- THEORETICAL: reachable from source/API reasoning but not observed in these sessions.
- NOT_EXERCISED: the artifact did not exercise the required transition.
- INSUFFICIENT EVIDENCE: a required identity, ordering, or vendor behavior is not observable.
- NOT REACHABLE: the searched path is not called by the target path as currently wired.

### 2.3 Session boundaries

The analyzer was run separately for each directory. The recursive Release-09/mhw directory was not treated as one timeline; its child sessions were kept independent.

The following potential work-order paths were absent at audit time:

- C:\GoogleDrive\ref-xefg\0914\mhw
- C:\GoogleDrive\ref-xefg\Intel\P3.1\dd2
- C:\GoogleDrive\ref-xefg\Intel\P3.1\mhw

The available runtime set was:

- C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004
- C:\GoogleDrive\ref-xefg\Release-09\mhw
- C:\GoogleDrive\ref-xefg\Release-09\mhw\Intel
- C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더
- C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더 (2)
- C:\GoogleDrive\ref-xefg\Release-09\mhw\새 폴더 (3)

## 3. Revision and binary matrix

| Layer | Revision / identity | Use in this audit | Boundary |
|---|---|---|---|
| Pinned OptiScaler source baseline | 036aae89e31447b21e1cab4e8d2c5c72421f9dc7 | PR38 merged source baseline | Source-of-truth baseline requested by the instruction |
| Final audited OptiScaler tip | cb69ca79e31abd72c4083aa247491179c6c9fc7e | Contains the audit instruction and the PR38 source baseline | The post-baseline change is documentation-only |
| PR37 source history | b0385ad6 | Confirms the outer transaction work is already in the baseline | Not reimplemented or changed |
| REFramework source inspected | e4a1a1d1, master/origin/master in D:\repo\REFramework | Current companion source for hook and identity semantics | Not the exact runtime build in the logs |
| REFramework runtime logs | c6704372c5808ea0c49059a23afa4ee12cc39e8d | Runtime binding, queue identity, resize, and Present evidence | Historical/runtime identity; cannot silently stand in for current REFramework source |
| Checked-in XeSS/XeFG contract | external/xess submodule 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0, v3.0.2 | Exact local headers and developer guide used for XeFG conclusions | Loaded game DLL ABI equivalence was not independently proven |
| Checked-in Streamline contract | external/streamline headers at the audited tip | DLSSG fence and resource-tag semantics | Streamline implementation internals are not checked in |
| MHW OptiScaler runtime builds | e9ca4685, ac1e48fa, b0621c37, d6b0132f; all report 0.9.5-pre4 | Resource/tag/Present/Resize evidence | These are not the pinned source commit labels |
| E_ABORT crash module set | dxgi.dll v0.9.5.4; libxell.dll v1.3.2.10; libxess_fg.dll v1.3.1.78; Streamline v2.7.32.0 | Crash provenance | No matching OptiScaler.log was present |

Relevant source history from the audited branch is:

~~~text
cb69ca79 docs: add SDLX-006/007 GPU queue synchronization audit instruction
036aae89 fix: harden DX12 wrapper COM ownership (#38)
f7492509 docs: scope PR38 COM hardening to Capcom DX12
1d1918a8 docs: add PR38 DXGI wrapper COM ownership work order
b0385ad6 fix: preserve outer XeFG frame transaction through EvaluateState (#37)
~~~

## 4. Runtime evidence

### 4.1 Session health and feature coverage

Session health and feature coverage are intentionally separate. A session can have healthy Present/Resize behavior while not exercising changed binding, long minimize, or a resource-level synchronization transition.

| Session | Pair status | Runtime identity | Duration | ResizeBuffers1 | Present/DXGI errors | Session health | Changed binding | Long minimize |
|---|---|---|---:|---:|---|---|---|---|
| E-abort 4004 | Incomplete; no OptiScaler.log | REFramework c6704372 | 6m51.3s | 6 enter / 6 success | No matching error in re2_framework_log | BLOCKED because the pair is incomplete | NOT_EXERCISED | NOT_AVAILABLE |
| mhw direct | Paired | Opti e9ca4685; REFramework c6704372 | 2m47.2s | 6 / 6 success | No nonzero Present result or material DXGI/device-removal marker | PASS | NOT_EXERCISED | NOT_EXERCISED; maximum Present1 gap 1.236s |
| mhw/Intel | Paired | Opti ac1e48fa; REFramework c6704372 | 1m46.1s Opti / 1m58.8s REF | 6 / 6 success | No nonzero Present result or material DXGI/device-removal marker | PASS | NOT_EXERCISED | NOT_EXERCISED; maximum gap 1.351s |
| mhw/새 폴더 | Incomplete; no REFramework log | Opti b0621c37 | 45.6s | Pair coverage blocked | No nonzero Opti Present result | BLOCKED; resize coverage cannot be paired | BLOCKED | NOT_EXERCISED |
| mhw/새 폴더 (2) | Paired; crash dump also present | Opti ac1e48fa; REFramework c6704372 | 1m22.4s | 6 / 6 success | No nonzero Present result or material DXGI/device-removal marker | PASS | NOT_EXERCISED | NOT_EXERCISED; maximum gap 0.826s |
| mhw/새 폴더 (3) | Paired | Opti d6b0132f; REFramework c6704372 | 1m13.8s Opti / 45.9s REF | 6 / 6 success | No nonzero Present result or material DXGI/device-removal marker | PASS | NOT_EXERCISED | NOT_EXERCISED; maximum gap 0.948s |

No session exercised the required changed-binding transition. The available logs therefore cannot prove queue replacement behavior during a real G to G+1 resource/present transition.

### 4.2 E_ABORT crash evidence

C:\GoogleDrive\ref-xefg\Release-09\E-abort 4004\2026_0916_0005\CrashReport.txt records:

- ExceptionCode C0000005, EXCEPTION_ACCESS_VIOLATION;
- ExceptionAddress 0x000000014CD94BF0;
- an unsymbolized stack consisting of MonsterHunterWilds.exe frames followed by KERNEL32.DLL and ntdll.dll;
- Message: Fatal D3D error (7, E_ABORT, 0x80004004);
- CPU/GPU identity: GenuineIntel Intel Arc G3 Extreme;
- MHW 1.42.0.2;
- dxgi.dll v0.9.5.4, libxell.dll v1.3.2.10, libxess_fg.dll v1.3.1.78, Streamline v2.7.32.0.

The E_ABORT artifact is real crash evidence, but the directory has no OptiScaler.log. The paired REFramework log in the same top-level collection is a healthy lifecycle log, not a resource/fence trace for the crash instruction. The dump could not be symbolically re-analyzed in this environment because cdb, dumpchk, windbg, and procdump were not available.

### 4.3 Resource and Present evidence in the direct MHW session

C:\GoogleDrive\ref-xefg\Release-09\mhw\OptiScaler.log is a 2m47.2s 0.9.5-pre4 session with:

- 30,739 XeFG_Dx12::SetResource entries;
- 17,889 Streamline reportResource entries;
- 12,115 D3D12TagFrameResource entries;
- 21,480 Dispatch entries;
- 4,708 XeFG Present entries;
- 9,424 hooked Present1 entries;
- 26,751 FGPresent entries;
- 12,553 LocalPresent entries;
- 11 FGchanged transitions;
- no nonzero LocalPresent result in the analyzer's result map.

Resource type IDs match the checked-in Streamline constants:

- 0 = Depth;
- 1 = MotionVectors / Velocity;
- 2 = HUDLessColor;
- 23 = UIColorAndAlpha;
- 50 = BidirectionalDistortionField.

The log contains eValidUntilPresent reports for Depth and Velocity at frame 457 and later, and successful XeFG tags for Depth and Velocity at frame 475. It does not log the resource pointer, command-list pointer, producer queue, fence value, or GPU completion. It therefore demonstrates the CPU pipeline but not the GPU happens-before relation.

Representative frame 475 timeline:

~~~text
21:48:24.769928  hkslSetConstants frame 475
21:48:24.770046  XeFG_Dx12::EvaluateState
21:48:24.771841  markPresent frame 475
21:48:24.772184  hkslSetTagForFrame frame 475
21:48:24.772250  D3D12TagFrameResource frame 475, Depth, SUCCESS
21:48:24.772295  D3D12TagFrameResource frame 475, Velocity, SUCCESS
21:48:24.772413  hkslEvaluateFeature frame 475
21:48:24.773060  FGHooks::hkFGPresent
21:48:24.773129  XeFG_Dx12::Present
21:48:24.773139  XeFG_Dx12::Dispatch
21:48:24.773286  Dispatch Result: Ok
21:48:24.773814  LocalPresent Calling original present
21:48:24.774000  LocalPresent Original present result: 0
~~~

The missing interval is the command-list submission and GPU execution interval. It is not valid to infer that interval from the success of TagFrameResource, Dispatch, or Present.

### 4.4 Runtime queue identity evidence

The REFramework runtime source records queue COM identity, device COM identity, and D3D12_COMMAND_QUEUE_DESC before classifying the relation. In all four paired MHW sessions, the source-generated result is real distinct same-device topology, not merely two formatted pointer values.

| Session | Init queue / COM identity | Presentation queue / COM identity | Device identity | Queue descriptors | Relation |
|---|---|---|---|---|---|
| mhw direct | 0x90ad1ee0 / 0x90ad1eb0 | 0x6e8cbca0 / 0x6e8cbc70 | 0x59cd37b0 | both direct, flags 0, node 1; priority 0 vs 100 | distinct_same_device |
| mhw/Intel | 0x85189040 / 0x85189010 | 0x938d1880 / 0x938d1850 | 0x59b97ae0 | both direct, flags 0, node 1; priority 0 vs 100 | distinct_same_device |
| mhw/새 폴더 (2) | 0x71225620 / 0x712255f0 | 0x71226640 / 0x71226610 | 0x5b275010 | both direct, flags 0, node 1; priority 0 vs 100 | distinct_same_device |
| mhw/새 폴더 (3) | 0x70a3c360 / 0x70a3c330 | 0x70a3e900 / 0x70a3e8d0 | 0x8a1e8a0 | both direct, flags 0, node 1; priority 0 vs 100 | distinct_same_device |

For the direct MHW session, the runtime records the external binding queue as 0x6e8cbca0 and keeps it unchanged across the six successful ResizeBuffers1 returns. The queue and binding generation in those ResizeLifecycle records remain 0x6e8cbca0 and generation 1.

This proves that at least the REFramework-observed initialization and internal swapchain/presentation queue observations are different D3D12 queue objects on the same device. It does not prove that every Depth, Velocity, or HUD-less producer uses either of those queues, and it does not expose a fence edge.

## 5. Public API and vendor contract findings

### 5.1 D3D12 queue ordering

Microsoft's ExecuteCommandLists contract states that successive ExecuteCommandLists calls on one queue order the first workload before the second, while the API call itself is a submission operation rather than a CPU-side GPU-completion wait. A different queue requires an explicit queue/fence relationship or an equivalent documented vendor dependency.

The relevant official references are:

- [ID3D12CommandQueue::ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists)
- [ID3D12CommandQueue::Wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait)
- [Executing and synchronizing command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists)

The audited OptiScaler resource path has no active producer-to-XeFG Signal/Wait pair. No CPU return from ExecuteCommandLists is treated as GPU completion in source; instead, the boolean SetResourceReady state is used for Dispatch eligibility.

### 5.2 DXGI ResizeBuffers1

The official [IDXGISwapChain3::ResizeBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1) contract defines pCreationNodeMask and ppPresentQueue as arrays of total size BufferCount. Each present queue corresponds to the matching node mask, and Present rotates through the queue array. A first-element-only observation cannot prove the queue used for every back buffer.

The checked source:

- forwards pCreationNodeMask and ppPresentQueue unchanged in FGHooks::hkResizeBuffers1;
- forwards them unchanged in WrappedIDXGISwapChain4::ResizeBuffers1;
- does not iterate or log ppPresentQueue[i] and pCreationNodeMask[i];
- dereferences *ppPresentQueue in the wrapper before checking ppPresentQueue itself;
- does not publish an XeFG owned-queue change from ResizeBuffers1.

The general [IDXGISwapChain::ResizeBuffers](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers) contract also requires direct and indirect back-buffer references, including command lists using those resources, to be released or otherwise made safe before resize.

The public documentation page describes the array contract but does not provide a blanket nullability guarantee for the wrapper's pointer in the exact game call. Consequently, a null ppPresentQueue case is not assumed safe. If the wrapper is called with ppPresentQueue == nullptr, the source dereference is an unsafe branch; whether that case is reachable in the target MHW call is NOT_EXERCISED.

### 5.3 Streamline resource tagging

The checked-in Streamline contract states:

- D3D12 native resource and resource state are mandatory;
- eOnlyValidNow means the resource may change, be destroyed, or be reused after the tag is provided;
- eValidUntilPresent means it must remain unchanged until the matching present;
- eValidUntilEvaluate is valid only through slEvaluateFeature;
- for slSetTag and slSetTagForFrame, payload generating a tagged resource must already be submitted to the provided command buffer or to another command buffer guaranteed by the host to execute before the provided command buffer.

This is a host ordering precondition. The hook passes cmdBuffer into reportResource but does not prove that the producer payload has been submitted or that a queue ordering edge exists.

### 5.4 XeSS/XeFG resource and queue contract

The checked-in XeFG D3D12 header and developer guide state:

- the queue passed to xefgSwapChainD3D12InitFromSwapChainDesc is the application queue used to present;
- the application must use the proxy swapchain returned by xefgSwapChainD3D12GetSwapChainPtr;
- XeSS-FG stores a reference to the initialization queue and executes interpolation work using that queue;
- D3D12TagFrameResource's pCmdList is optional and is required only when the resource is not valid until the next present;
- RV_ONLY_NOW causes XeFG to record a copy to the supplied command list and allows reuse after that tag command list has been submitted;
- RV_UNTIL_NEXT_PRESENT keeps the resource alive through the matching present;
- all tag command lists must be submitted before Present;
- SetPresentId must be called before Present;
- XeFG ignores queues supplied through IDXGISwapChain3::ResizeBuffers1 and substitutes an internally managed queue for that resize operation.

The local contract source is:

- external/xess/inc/xess_fg/xefg_swapchain_d3d12.h:176-256
- external/xess/doc/xess_fg_developer_guide_english.md:325-337
- external/xess/doc/xess_fg_developer_guide_english.md:699-761
- external/xess/doc/xess_fg_developer_guide_english.md:1014-1029
- external/xess/doc/xess_fg_developer_guide_english.md:1056-1065
- external/xess/doc/xess_fg_developer_guide_english.md:1096-1108

The contract proves the required application boundary, but it does not expose a client fence handle that OptiScaler can use to bridge an arbitrary producer queue to an arbitrary internal consumer queue. It is therefore not a proof that an unsubmitted or cross-queue producer list is safe.

## 6. Mandatory trace A — Streamline DLSSG completion fence

### 6.1 Exact definition and meaning

The checked-in external/streamline/sl_dlss_g.h defines DLSSGState as struct version 4. The relevant fields are:

- void* inputsProcessingCompletionFence;
- uint64_t lastPresentInputsProcessingFenceValue.

The header says this is an SL DLSS-G plugin-internal fence and associated value. The client must wait before modifying or destroying tagged resources from the corresponding previously presented frame on a non-presenting queue. On the client's presenting queue the wait is recommended but not required, unless eBlockNoClientQueues is enabled. The header further requires slDLSSGGetState to be called on the present thread to retrieve the value for the inputs consumed by frame generation.

The same header says eBlockNoClientQueues is currently supported only on Vulkan; D3D clients use the presenting-queue blocking mode. The audit found no OptiScaler code that changes queueParallelismMode.

Ownership and direction are therefore:

| Party | Responsibility proven by contract |
|---|---|
| Streamline/DLSS-G plugin | Owns the plugin-internal fence semantics and advances/signals it internally; the exact binary signal site is not public in the checked-in headers |
| Host/client | Calls slDLSSGGetState on the present thread and waits on the returned fence/value before modifying or destroying an old DLSS-G tagged resource on a non-presenting queue |
| OptiScaler in this branch | Forwards/copies the pointer and value; does not signal, wait, store, or consume them for XeFG |

The fence describes DLSS-G's completion of reading previously presented inputs. It is not documented as a producer-completion fence that makes new game Depth/Velocity/HUD-less writes visible to XeFG.

### 6.2 Complete hkslDLSSGGetState trace

Repository-wide search found no application-side or OptiScaler-side caller that later reads either field. The dynamic path is:

~~~text
Streamline/plugin asks slGetPluginFunction("slDLSSGGetState")
  -> StreamlineHooks::hkdlssg_slGetPluginFunction
    -> returns StreamlineHooks::hkslDLSSGGetState
      -> hkslDLSSGGetState captures caller structVersion
        -> for struct versions below 4:
             calls o_slDLSSGGetState into a local newer DLSSGState
             copies the version-appropriate fields back
             copies inputsProcessingCompletionFence/value for version >= 3
        -> for version 4:
             forwards directly to o_slDLSSGGetState
        -> optionally overrides VRAM estimate and numFramesActuallyPresented
        -> returns to the Streamline caller
~~~

Source references:

- OptiScaler/hooks/Streamline_Hooks.cpp:772-835
- OptiScaler/hooks/Streamline_Hooks.cpp:947-960
- external/streamline/sl_dlss_g.h:149-197

No later read, wait, Signal, or queue association exists in the repository. Consuming this fence inside XeFG as a producer-to-XeFG wait would be semantically unjustified from the contract. It protects a different lifecycle edge: DLSS-G finished reading old resources.

The direct MHW OptiScaler log contains repeated Streamline warnings that slDLSSGGetState must be synchronized with the present thread, followed by OptiScaler hkslDLSSGGetState Status: eOk. This confirms that the API's present-thread synchronization warning was emitted, but it does not expose the fence pointer/value or establish an E_ABORT cause.

**Conclusion for SDLX-006-C:** the DLSSG completion fence is not a proven missing producer fence. Treating it as one would risk reversing the intended dependency direction.

## 7. Mandatory trace B — resource production to XeFG

### 7.1 Common Streamline entry path

The hooked entry points are:

- hkslSetTag;
- hkslSetTagForFrame;
- hkslEvaluateFeature.

For DX12 DLSSG resource types, each hook calls Sl_Inputs_Dx12::reportResource with:

- the Streamline ResourceTag;
- cmdBuffer cast to ID3D12GraphicsCommandList;
- frame token 0 or the supplied frame token.

The hook then calls the original Streamline function. The hook does not submit the command list and does not obtain the D3D12 command queue associated with cmdBuffer.

Source references:

- OptiScaler/hooks/Streamline_Hooks.cpp:147-229
- OptiScaler/hooks/Streamline_Hooks.cpp:231-307
- OptiScaler/hooks/Streamline_Hooks.cpp:309-340

Every reportResource call begins a ScopedStreamlineFGTransaction with owner 2 and calls CheckForFrame. The transaction protects CPU state and resource-map mutation; it is not a GPU fence or a D3D12 queue wait.

### 7.2 Depth

For a Depth tag:

1. hkslSetTag, hkslSetTagForFrame, or hkslEvaluateFeature receives the ResourceTag and cmdBuffer.
2. reportResource copies:
   - native resource to Dx12Resource::resource;
   - cmdBuffer to Dx12Resource::cmdList;
   - ResourceTag::state to Dx12Resource::state;
   - eValidUntilPresent to UntilPresent, other supported lifecycles to ValidNow unless downgraded;
   - frame token to a ring frameIndex.
3. reportResource maps the Streamline type to FG_ResourceType::Depth and calls XeFG_Dx12::SetResource.
4. SetResource stores the resource, state, validity, extent, and command-list pointer in _frameResources[fIndex][Depth].
5. If needed, optional flip/copy/barrier work is recorded into the supplied command list or an OptiScaler helper list.
6. SetResource creates the XeFG resource parameter and calls D3D12TagFrameResource(_swapChainContext, fResource->cmdList, frameId, &resourceParam).
7. On a successful tag, SetResource calls SetResourceReady(type, fIndex).
8. Dispatch later checks IsResourceReady(Depth, fIndex).

The critical ordering is CPU-visible, not GPU-visible:

~~~text
R1 reportResource(Depth, frame N, command list C)
R2 XeFG::SetResource
R3 D3D12TagFrameResource(C)
R3b SetResourceReady(Depth)          [after TagFrameResource returns, before producer Execute is observed]
R4 ResTrack registration             [not called by current Streamline/XeFG path]
R5 hkExecute match                   [conditional dormant/other caller path only]
R6 original ExecuteCommandLists      [application-controlled; not logged in this session]
R8 Dispatch reads IsResourceReady
R9 XeFG interpolation consumes input [internal GPU boundary not exposed]
R10 proxy Present/Present1
~~~

### 7.3 Velocity / MotionVectors

Velocity follows the same path as Depth, with these type-specific operations:

- Streamline kBufferTypeMotionVectors maps to FG_ResourceType::Velocity;
- mvsWidth and mvsHeight are updated in Sl_Inputs_Dx12;
- XeFG GetResourceData maps Velocity to XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
- duplicate Velocity resources for the same ring slot are rejected;
- SetResourceReady(Velocity, fIndex) is reached after successful TagFrameResource.

No resource-level queue or fence is captured for Velocity. Depth and Velocity may be tagged in the same Streamline call, but the source does not prove that the underlying producing work is on the same command list or queue.

### 7.4 HUD-less color

HUDLessColor enters through the same three Streamline hooks and is stored as FG_ResourceType::HudlessColor.

Differences:

- HUD-less can be deferred to Dispatch when it is not immediately valid;
- XeFG_Dx12::Dispatch changes non-ValidNow HUD-less validity to UntilPresentFromDispatch and calls SetResource again;
- SetResource maps it to XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
- Dispatch does not require HUD-less readiness for its initial Depth/Velocity gate;
- Distortion is not supported by XeFG::SetResource and is rejected after logging;
- UI is stored and may be submitted through an OptiScaler helper command list.

The direct MHW log shows HUD-less reports, but it does not provide a corresponding resource pointer, command list, queue, or successful tag chain sufficient to prove GPU ordering. HUD-less therefore remains NOT_EXERCISED for the required resource-level synchronization proof.

### 7.5 Command-list tracking call graph

The repository-wide call graph for ResTrack_Dx12::SetResourceCmdList is:

~~~text
ResTrack_Dx12::SetResourceCmdList
  -> definition in OptiScaler/resource_tracking/ResTrack_dx12.cpp:2260
  -> declaration in ResTrack_dx12.h:589
  -> commented historical call in FSRFG_Dx12.cpp
  -> no active call from Sl_Inputs_Dx12::reportResource
  -> no active call from XeFG_Dx12::SetResource
~~~

Therefore the current Streamline/XeFG path does not register its captured resource cmdList in _resourceCommandList through this helper.

The conditional ResTrack path itself is:

~~~text
SetResourceCmdList(type, C)
  -> canonicalize C through the Streamline private IID when possible
  -> _resourceCommandList[fIndex][type] = real C

hkClose(C)
  -> exact pointer match against _resourceCommandList[fIndex]
  -> moves matched type to _resCmdList[fIndex]
  -> calls original Close

hkExecuteCommandLists(Q, lists)
  -> exact pointer match against _resCmdList[fIndex]
  -> SetResourceReady(type)
  -> erases matched type
  -> calls original ExecuteCommandLists(Q, lists)
  -> SetCommandQueue(type, Q)
~~~

Source references:

- OptiScaler/resource_tracking/ResTrack_dx12.cpp:625-698
- OptiScaler/resource_tracking/ResTrack_dx12.cpp:1607-1643
- OptiScaler/resource_tracking/ResTrack_dx12.cpp:2205-2274
- OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1821-1847

The ResTrack path, when externally registered, also marks readiness before the original ExecuteCommandLists call returns. It still has no GPU completion wait. It is not the active registration route for the current Streamline/XeFG path.

### 7.6 R1-R10 status matrix

| Step | Source result | Evidence |
|---|---|---|
| R1 reportResource entered | Confirmed | hkslSetTag / hkslSetTagForFrame / hkslEvaluateFeature call reportResource |
| R2 D3D12TagFrameResource | Confirmed | XeFG_Dx12::SetResource calls it for Depth, Velocity, HUD-less/UI when the path reaches the tag block |
| R3 SetResourceReady from SetResource | Confirmed for Depth/Velocity and ValidNow/dispatch-deferred paths | SetResourceReady follows tag success in XeFG_Dx12::SetResource |
| R4 tracked command list match | Not reachable from the active Streamline/XeFG registration path | SetResourceCmdList has no active caller in this chain |
| R5 SetResourceReady from Execute hook | Conditional only | Requires an externally registered _resCmdList entry |
| R6 original ExecuteCommandLists | Confirmed as the terminal call of hkExecuteCommandLists | The hook calls original after CPU readiness handling; no active resource correlation in current logs |
| R7 SetCommandQueue(type, Q) | Conditional and source-confirmed | Only found-list path invokes it; XeFG implementation ignores type |
| R8 Dispatch reads IsResourceReady | Confirmed | Dispatch gates on Depth and Velocity readiness |
| R9 XeFG/vendor consumes resource | API boundary confirmed; exact GPU edge unproven | XeFG submits interpolation from proxy Present, but exact internal consumer visibility is opaque |
| R10 Present/Present1 | Confirmed | FGHooks calls fg->Present before forwarding proxy Present/Present1; runtime shows successful returns |

## 8. Queue identity matrix

The following labels are kept separate. A same-device result is not treated as same-queue.

| Queue label | Source variable/API | Acquisition and ownership | Device / descriptor / generation | Writers or replacements | Consumers | Can differ from producer? |
|---|---|---|---|---|---|---|
| Qfactory/create | FGHooks candidateQueue | QI from CreateSwapChain pDevice; local ComPtr | Device is associated through the queue; generation published after successful creation | New lifecycle creation can replace it | XeFG create/init and FGHooks publish | Yes |
| Qxefg-init | XeFG queueCandidate passed to D3D12InitFromSwapChainDesc | CheckForRealObject resolves Streamline proxy to real queue; ComPtr owns candidate during init | Public XeFG contract stores this queue; generation at successful swapchain publish | Replaced only on new XeFG lifecycle; not by ResizeBuffers1 per public contract | XeFG internal interpolation according to checked-in guide | Yes, runtime REF observes a separate internal presentation/create queue |
| Qxefg-owned | XeFG_Dx12::_ownedGameCommandQueue and raw _gameCommandQueue | ComPtr assignment in CommitGameCommandQueue | Initially Qxefg-init; no independent descriptor capture | SetCommandQueue can replace it; ReleaseSwapchain resets it | XeFG UI/SC helper ExecuteCommandLists and any code using GetCommandQueue | Yes |
| Qresource(type) | ResTrack hkExecuteCommandLists queue | Queue object on which the exact tracked list is observed | Queue descriptor not captured by OptiScaler ResTrack; active Streamline route does not register | A different queue can be observed for another tracked type | Conditional readiness and SetCommandQueue | Yes |
| Qstate | State::currentCommandQueue | Raw alias; set by DXGI factory, LocalPresent if null, and first ppPresentQueue in wrapper ResizeBuffers1 | No COM ownership or generation in the field | Can be overwritten by wrapper/resource paths | Other discovery and timing paths | Yes |
| Qfg-resize | FGHooks file-static currentCommandQueue | ComPtr candidate published by PublishResizeSync | Carries currentCommandQueueGeneration; fence created on current State device | Reset only on generation retirement; not updated by ordinary ResizeBuffers1 | FGHooks WaitForGPUIdle and old lifecycle wait | Yes |
| Qresize[i] | ResizeBuffers1 ppPresentQueue[i] | Input array forwarded to real DXGI | Each element corresponds to BufferCount/node mask by DXGI contract; source does not inspect values | Caller/driver can provide per-buffer entries | DXGI Present/back-buffer rotation | Yes |
| Qwrapped-device | WrappedIDXGISwapChain4::_device | Raw object accepted at wrapper construction; in ResizeBuffers1 assigned from *ppPresentQueue[0] when XeFG active | It is an IUnknown-shaped field, not an owned queue reference | Overwritten only by the first present queue element in this path | Wrapper WaitForGPUIdle query and downstream real call | Yes |
| Qlocal-present | LocalPresent queriedQueue, realQueueOwner, queueForUse | QI from pDevice; QueryRealObjectOwned may return underlying real queue | Device queried from queue; State updated only when current alias is null | LocalPresent uses the per-call queue for timing and device capture | LocalPresent/original Present call | Yes |
| Qref-binding | REFramework D3D12Hook m_command_queue / ExternalBind queue | Current REF source binds a candidate queue; runtime logs source and identity | Current source uses queue identity, device identity, GetDesc; runtime c670 logs generation | Rebinding/lifecycle can replace it | REF Present/Resize monitoring and renderer callbacks | Yes |
| Qvendor-present | XeFG internal queue, if any | No client-facing pointer in XeFG API; REF's presentation_queue is captured during internal factory creation | Public guide documents init queue for interpolation and separately describes internally managed presentation behavior for other paths | Vendor controlled | Internal frame-generation/presentation work | UNPROVEN for the exact Intel MHW consumer |

### 8.1 Canonicalization result

OptiScaler's Util::CheckForRealObject, Util::QueryRealObjectOwned, IFGFeature_Dx12::CheckForRealObject, and ResTrack_Dx12::CheckForRealObject use the Streamline private IID ADEC44E2-61F0-45C3-AD9F-1B37379284FF to resolve a proxy to an underlying object.

That is useful but limited:

- it canonicalizes Streamline proxy versus real object where the interface is exposed;
- CheckForRealObject releases the queried interface and leaves a borrowed pointer;
- QueryRealObjectOwned keeps the queried reference in the caller;
- it does not perform a universal IUnknown identity comparison for every State, FGHooks, wrapper, and ResizeBuffers1 alias;
- it does not prove queue equivalence merely because two queues share a D3D12 device.

The current REFramework XeFGDiscovery source does perform the stronger identity check for its own observation: it obtains IUnknown identities for both queues, obtains device identities, and calls GetDesc. The runtime distinct_same_device results are consequently real queue identity evidence for the observed pair.

### 8.2 Queue-type and node result

Where descriptor data is available in REFramework, the observed init and presentation queues are both direct queues with flags 0 and node mask 1. The presentation queue has priority 100 while the init queue has priority 0 in the runtime samples.

OptiScaler's ResTrack hook-created queue in HookToQueue is a separate instrumentation queue configured as direct, normal priority, flags none, node mask 0. It exists to obtain a vtable and install the ExecuteCommandLists detour; it is not evidence that game resources use that queue.

OptiScaler itself does not call GetDesc on the producer queue observed in hkExecuteCommandLists. A resource-level Type/Priority/Flags/NodeMask table therefore cannot be reconstructed from OptiScaler.log.

### 8.3 Global queue overwrite

IFGFeature_Dx12 exposes one _gameCommandQueue. XeFG_Dx12::SetCommandQueue is:

~~~text
void XeFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue)
{
    CommitGameCommandQueue(queue);
}
~~~

The type parameter is ignored. If the conditional ResTrack path observes Depth on QA and Velocity on QB, the later call replaces the global XeFG queue with QB. The source has no per-resource queue map and no check that QA == QB or that either queue is the initialization queue.

This is a source-confirmed conditional fragility. It is not reached by the active Streamline/XeFG registration chain in this audit because SetResourceCmdList has no active caller there. A future path that registers different resource types would make the overwrite reachable.

## 9. Fence and dependency matrix

| Fence / mechanism | Creator | Signal point | Waiter / wait point | CPU or GPU wait | Generation / lifetime | Orders frame resources? |
|---|---|---|---|---|---|---|
| Streamline DLSSG inputsProcessingCompletionFence | DLSS-G plugin internal | Internal vendor signal; exact point opaque | Host client after present, before modifying/destroying old tagged resources on a non-presenting queue | Contract is client wait; OptiScaler performs no wait | Plugin/previous-present resource lifecycle | Orders old DLSS-G input reuse, not new game producer to XeFG consumer |
| FGHooks resizeFence | FGHooks::PublishResizeSync creates it from State::currentD3D12Device | FGHooks::WaitForGPUIdle calls current queue->Signal | FGHooks polls GetCompletedValue, then SetEventOnCompletion + WaitForSingleObject | CPU wait on a fence signaled by the supplied queue | Associated with currentCommandQueueGeneration | Resize/old-lifecycle idle only; not a per-frame producer dependency |
| Wrapped swapchain resizeFence | wrapped_swapchain.cpp static helper | Intended queue->Signal inside WaitForGPUIdle(IUnknown*) | Intended CPU event wait | CPU wait if the function body is entered | File-static; no generation association | No |
| Wrapped resize helper actual behavior | Same helper | No signal in normal initial state | No wait | No-op because the outer condition requires resizeFence and resizeFenceEvent to be non-null before creating them, and no other assignment initializes them in this file | Static values remain null | Source-confirmed gap in this helper; FGHooks may provide a separate resize wait |
| ResTrack SetResourceReady | No fence; map/set boolean | CPU call after TagFrameResource or before original Execute in conditional hook | Dispatch reads IsResourceReady | Neither CPU nor GPU completion | Ring index only | No |
| XeFG D3D12TagFrameResource | XeFG library | No public client fence signal exposed | XeFG manages resource lifetime according to validity and Present | API contract requires host to submit tag lists before Present | Present ID and resource validity | May record RV_ONLY_NOW copy; not a documented cross-queue fence |
| ImGui D3D12 fence | ImGui backend | ImGui command queue | ImGui command queue or CPU event | Unrelated to MHW FG input path | ImGui backend frame lifecycle | No |
| DX11-on-12 / Vulkan-on-12 fences | Upscaler adapters | Adapter copy/feature queues | Adapter waits | GPU or CPU depending on adapter | Adapter frame lifecycle | Not the active Streamline XeFG resource path |
| Hudfix D3D12 fence | Hudfix_Dx12 | Hudfix-owned queue/commands | Hudfix path | Unrelated to XeFG producer proof | Hudfix lifecycle | No |

Repository-wide search found no active ID3D12CommandQueue::Wait in the XeFG Streamline resource path. The Wait calls found in the repository are ImGui, adapter/upscaler, or other feature-specific paths. The only direct Signal/SetEventOnCompletion/GetCompletedValue pair in FGHooks and the wrapper is resize-oriented.

### 9.1 Cross-queue proof

For a required edge with Qproducer != Qconsumer, the exact proof would be one of:

~~~text
Qproducer->Signal(F, N)
Qconsumer->Wait(F, N)
~~~

or an explicitly documented vendor API that guarantees the same dependency from the submitted command list/queue.

The audited source contains neither an OptiScaler producer Signal/consumer Wait pair nor a public XeFG client fence that supplies this edge. The DLSSG completion fence points to old-input reuse after a previous present, not to producer completion before XeFG reads a new input. The public XeFG docs require host submission before Present, but do not document an arbitrary cross-queue implicit wait that OptiScaler can rely on.

**Result:** cross-queue producer-to-XeFG GPU ordering is UNPROVEN.

### 9.2 Same-queue proof

Same-queue ordering would be sufficient only if all of the following were true:

1. the exact Depth/Velocity/HUD-less producer queue equals the queue used for the XeFG consumer;
2. the queue identity remains stable from producer recording through Present;
3. the producer command list is submitted before the proxy Present;
4. no queue replacement or resize transition changes the owner mid-frame;
5. no vendor-internal consumer queue bypasses the producer queue without a documented edge.

The runtime disproves the broad assumption that every observed XeFG queue is one queue: init and presentation observations are distinct_same_device in all paired MHW sessions. The logs do not identify resource producer queues or the exact internal consumer queue, so a same-queue proof cannot be completed for Depth, Velocity, or HUD-less.

## 10. XeFG consumption and Present path

### 10.1 Initialization and owned queue

XeFG_Dx12::CreateSwapchain and CreateSwapchain1:

1. resolve the incoming queue through CheckForRealObject;
2. store it in queueCandidate;
3. pass queueCandidate to D3D12InitFromSwapChainDesc;
4. obtain the proxy swapchain through D3D12GetSwapChainPtr;
5. call CommitGameCommandQueue(queueCandidate.Get());
6. store _swapChain and hwnd.

Source references:

- OptiScaler/framegen/xefg/XeFG_Dx12.cpp:469-685
- OptiScaler/framegen/xefg/XeFG_Dx12.cpp:688-870

The public XeFG guide says this initialization queue is retained and used for interpolation. ReleaseSwapchain resets _gameCommandQueue and _ownedGameCommandQueue after the context and helper objects are released.

### 10.2 TagFrameResource, validity, and sentinel

For normal resources, SetResource passes fResource->cmdList to D3D12TagFrameResource. For old SDK versions below 1.2.2, the code substitutes the sentinel pointer 1 when an UntilPresent resource has no command list:

~~~text
if (fResource->cmdList == nullptr &&
    resourceParam.validity == XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT)
{
    fResource->cmdList = (ID3D12GraphicsCommandList*)1;
}
~~~

The same pattern is used for the backbuffer region tag in Dispatch. The public API says pCmdList is optional when the resource is valid until next present and that only the backbuffer region is used for a backbuffer tag. The sentinel is an OptiScaler compatibility workaround, not a documented synchronization primitive. It creates no producer queue edge and does not make a command list submitted.

For RV_ONLY_NOW, XeFG's guide documents a copy recorded to the supplied command list and reuse after that list has been submitted. For RV_UNTIL_NEXT_PRESENT, the resource must remain unchanged through the matching Present. Neither statement makes CPU SetResourceReady equivalent to GPU completion.

### 10.3 Dispatch, helper lists, and Present

XeFG_Dx12::Present:

- optionally records UI and swapchain composition work;
- closes and executes _uiCommandList[fIndex] and _scCommandList[fIndex] using _gameCommandQueue;
- advances _fgFramePresentId;
- calls Dispatch.

XeFG_Dx12::Dispatch:

- obtains the dispatch ring index;
- requires Depth and Velocity IsResourceReady;
- tags delayed HUD-less/Distortion resources as applicable;
- calls TagFrameConstants;
- calls SetPresentId;
- tags the backbuffer region;
- returns to FGHooks.

FGHooks::FGPresent then forwards the proxy Present/Present1 call. The public XeFG guide states that interpolation shaders are submitted when the application calls Present on the proxy swapchain.

The following are therefore proven:

- OptiScaler-owned UI/SC helper lists execute on the current _gameCommandQueue;
- XeFG initialization receives Qxefg-init;
- SetPresentId occurs before the proxy Present in the normal path;
- the vendor performs its interpolation submission at/through Present.

The following are not proven:

- that every resource producer submitted to the same queue as _gameCommandQueue;
- that a command list supplied to TagFrameResource was already submitted;
- that the REFramework presentation_queue is the actual interpolation consumer queue;
- that a hidden Intel queue waits for all arbitrary game producer queues;
- that the sentinel pointer has any synchronization meaning.

## 11. ResizeBuffers1 and generation matrix

### 11.1 Creation and generation G

For a new XeFG lifecycle:

~~~text
FGHooks::CreateSwapChain / CreateSwapChainForHwnd
  -> candidateQueue acquired from pDevice
  -> XeFG_Dx12::CreateSwapchain*
  -> real queue canonicalization
  -> XeFG D3D12InitFromSwapChainDesc(queueCandidate)
  -> D3D12GetSwapChainPtr
  -> CommitGameCommandQueue(queueCandidate)
  -> nextFGSwapchainGeneration increments
  -> currentFGSwapchainGeneration publishes G
  -> WrappedIDXGISwapChain4::BindFGGeneration(G)
  -> FGHooks::PublishResizeSync(candidateQueue, G, previousGeneration)
~~~

At this point:

- Qxefg-init and Qxefg-owned are the canonicalized initialization queue;
- Qfg-resize is the same candidate queue in a separate ComPtr;
- State::currentCommandQueue is a raw alias set by surrounding DXGI creation logic;
- REFramework may observe a separate internal presentation/create queue;
- all of these are same-device only until identity comparison proves more.

### 11.2 Game-originated ResizeBuffers1

The source-level layers are:

~~~text
WrappedIDXGISwapChain4::ResizeBuffers1
  -> optional first-element State::currentCommandQueue = *ppPresentQueue[0]
  -> _device = State::currentCommandQueue
  -> wrapper WaitForGPUIdle(_device) [currently no-op in this file]
  -> forwards exact arrays to _real3->ResizeBuffers1
  -> enumerates resulting buffers

FGHooks::hkResizeBuffers1, if the proxy vtable hook is active
  -> OwnedLockGuard owner 6678
  -> PauseFG
  -> WaitForGPUIdle(FGHooks::currentCommandQueue)
  -> forwards exact arrays to original ResizeBuffers1
  -> clears FGResizing and cross-entry skip flag

Underlying real IDXGISwapChain3::ResizeBuffers1
  -> DXGI consumes the complete ppPresentQueue[] / pCreationNodeMask[] contract

XeFG library
  -> public guide says ResizeBuffers1 queues are ignored for XeFG ownership
~~~

The two skip flags are cross-method recursion guards:

- hkResizeBuffers sets _skipResize1 before forwarding ResizeBuffers, so a nested ResizeBuffers1 can take its skip branch;
- hkResizeBuffers1 sets _skipResize before forwarding ResizeBuffers1, so a nested ResizeBuffers can take its skip branch;
- the wrapper method does not set these flags.

The exact combination of wrapper and FGHooks hooks depends on which swapchain interface/vtable is called in the process. The source proves each individual forwarding path, but not that every application call traverses both layers.

### 11.3 Owner/cache update table after a successful ResizeBuffers1

| Owner/cache | Update on source path | Status after ResizeBuffers1 |
|---|---|---|
| WrappedIDXGISwapChain4::_device | Set to *ppPresentQueue[0] only when the pointer is non-null and XeFG is active | UPDATED to first element only; no array proof |
| State::currentCommandQueue | Same first-element assignment in wrapper; otherwise may remain old raw alias | UPDATED or UNCHANGED depending on layer; source does not canonicalize the entire array |
| FGHooks::currentCommandQueue | Not changed by hkResizeBuffers1 | UNCHANGED intentionally as the published lifecycle/resize queue |
| FGHooks::currentCommandQueueGeneration | Not changed by ordinary resize | UNCHANGED at G |
| XeFG::_ownedGameCommandQueue | Not changed by ResizeBuffers1 | UNCHANGED intentionally under XeFG public contract |
| IFGFeature_Dx12::_gameCommandQueue | Same pointer as XeFG owned raw alias | UNCHANGED unless a conditional SetCommandQueue path runs later |
| REFramework external binding queue | Runtime source binding is lifecycle-owned; no OptiScaler resize publication | UNCHANGED in observed sessions |
| current FG swapchain generation | No increment on ordinary ResizeBuffers1 | UNCHANGED at G |
| ResTrack resource maps | Only current ring slot is cleared by ClearPossibleHudless; no generation field | RING-RESET-DEPENDENT, not generation-proven |

This is the central SDLX-007 result: a wrapper/global alias may change while the XeFG-owned queue correctly remains the initialization queue by contract. That divergence is expected for XeFG's ResizeBuffers1 behavior, but the source provides no universal identity record tying the array, State alias, FGHooks fence queue, and all future producer queues together.

### 11.4 ppPresentQueue array and node-mask gap

For BufferCount 3, DXGI's contract requires three corresponding queue/node entries. The source forwards them but records only the first queue in the wrapper assignment and does not record any queue values in FGHooks. The REFramework runtime logs in c670 record the pointer to the array and the first node-mask value, not the contents of all entries. Therefore:

- ppPresentQueue[0] is runtime/source-candidate evidence only;
- ppPresentQueue[1] and ppPresentQueue[2] are INSUFFICIENT EVIDENCE;
- node mask correspondence for every entry is INSUFFICIENT EVIDENCE;
- queue change from the previous generation is NOT_EXERCISED.

### 11.5 Generation rollover answers

1. Can a Qfg-resize queue from G remain while resources/present belong to G+1?
   Yes, source permits separate lifecycle owners until a new creation/release publishes and retires the generation. The exact ordinary-resize path is not a G+1 transition because generation increments only on new lifecycle creation.

2. Can WaitForGPUIdle signal a queue that no longer owns the backbuffers?
   The FGHooks helper signals its published lifecycle queue, not necessarily a new ResizeBuffers1 array element. Whether that is safe depends on the application/queue contract and the preceding lifecycle decision. The wrapper helper is separately a no-op as currently written. Exact wrong-queue occurrence is NOT_EXERCISED.

3. Can a post-resize resource submission overwrite XeFG's queue with an old-generation queue?
   The conditional SetCommandQueue path can overwrite the global queue with whatever queue the Execute hook observes; it carries no generation. The active Streamline path does not register through SetResourceCmdList, so target reachability is RUNTIME-CANDIDATE, not observed.

4. Can an old command list be recognized after frame/generation rollover?
   ResTrack uses ring-slot and pointer maps, not a generation key. ClearPossibleHudless clears only the current slot and records not-closed/not-executed pointers in _notFoundCmdLists. A generation-safe proof is absent.

5. Does preserved-swapchain mode change the answer?
   Yes. XeFG may call ResizeBuffers on the existing proxy lifecycle instead of destroying/recreating it. That keeps the generation and owned init queue while backbuffer state changes. The public XeFG contract still says ResizeBuffers1 queues do not replace its owned queue. The resource/fence relationship across this preserved lifecycle is not directly logged.

## 12. Hypothesis matrix

| Hypothesis | Status | Resolution |
|---|---|---|
| H1 — CPU-ready-before-submit permits XeFG consumption before producer submission | RUNTIME-DEPENDENT CANDIDATE | SOURCE-CONFIRMED CPU sequence: SetResourceReady follows TagFrameResource and precedes any active producer Execute observation. If Present/Dispatch runs before the producer list is submitted, the source has no guard. The runtime does not capture the race. |
| H2 — CPU-ready-after-submit still does not imply GPU completion | INSUFFICIENT EVIDENCE | The statement is true for a different consumer queue unless Signal/Wait or a documented implicit edge exists. The current source has no such edge, but the exact vendor consumer queue is opaque. |
| H3 — DLSSG completion fence is the missing producer dependency | INSUFFICIENT EVIDENCE | Rejected as a direct assumption. The header defines it as plugin-internal completion for previously presented input reuse, not a producer-completion fence for new XeFG input visibility. |
| H4 — passing producer command list into D3D12TagFrameResource is sufficient | INSUFFICIENT EVIDENCE | The API documents pCmdList as resource-lifetime/copy support and requires the host to submit tag command lists before Present. It does not document that passing an unsubmitted list establishes a cross-queue wait. |
| H5 — all MHW resources are produced and consumed on one queue | RUNTIME-DEPENDENT CANDIDATE | The broad one-queue invariant is not established. REFramework observed distinct init/presentation COM identities on the same device in every paired MHW session. Resource-specific producer queues are not in OptiScaler logs. |
| H6 — multiple resource queues are safely represented by one global _gameCommandQueue | SAFE BUT FRAGILE | SetCommandQueue ignores type and overwrites one global queue. The conditional path is unsafe to rely on for distinct resource queues, but the current Streamline/XeFG path does not actively register through it. |
| H7 — ResizeBuffers1 changes present queue but XeFG owned queue remains old | SAFE BUT FRAGILE | The owned queue remaining the init queue is explicitly documented XeFG behavior, not automatically a defect. The wrapper and State aliases can nevertheless diverge because only ppPresentQueue[0] is observed and no full-array identity is retained. |
| H8 — E_ABORT queue difference is wrapper/proxy identity only | SAFE BUT FRAGILE | The identity-only explanation is rejected for the observed REF pair: XeFGDiscovery compares IUnknown and device identities and reports distinct_same_device. This topology still does not prove that the queue caused E_ABORT. |
| H9 — FGHooks resize fence establishes frame-resource ordering | SAFE BUT FRAGILE | It orders work submitted to the FGHooks resize queue before resize/release. It is not a per-frame producer-to-XeFG fence and is not connected to the Streamline resource command list. |
| H10 — vendor Present path internally waits for game work | INSUFFICIENT EVIDENCE | The public guide proves vendor submission at proxy Present and the init queue contract, but does not expose an arbitrary cross-queue wait for all game producer work. Runtime has no fence or wait telemetry. |
| H11 — eOnlyValidNow / command-list resources are stricter than eValidUntilPresent | SAFE BY DOCUMENTED API CONTRACT | Streamline and XeFG contracts both define stricter lifetime/submission obligations for volatile/ONLY_NOW resources. Current code passes the command list but does not prove its submission order. |
| H12 — queue-generation change can occur without resource/frame-generation reset | RUNTIME-DEPENDENT CANDIDATE | Generation increments on lifecycle creation, not ordinary ResizeBuffers1. Ring-slot resource state and conditional queue publication lack a generation key. A real stale G to G+1 transition was not exercised. |

## 13. Findings

### SDLX-006-A — CPU readiness is a boolean, not GPU completion

- Status: RUNTIME-DEPENDENT CANDIDATE
- Severity / impact: High under a pre-submit or cross-queue interleaving; potentially stale/invalid input consumption.
- Confidence: SOURCE-CONFIRMED for the CPU ordering; runtime causality not proven.
- Exact source: OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1821-1847; OptiScaler/framegen/IFGFeature.cpp:312-319.
- Preconditions: XeFG is active, Depth/Velocity is accepted, TagFrameResource succeeds, and Present/Dispatch can run before the producing command list is submitted or completed.
- Reachable sequence: reportResource -> SetResource -> TagFrameResource -> SetResourceReady -> Present -> Dispatch -> vendor consumption.
- Contract gap: SetResourceReady has no D3D12 fence semantics. XeFG requires the host to submit all tag command lists before Present.
- Consequence: Dispatch may accept a CPU-ready resource while its producer GPU payload is not yet visible to the consumer.
- Current MHW reachability: Candidate only. The logs show tags before Dispatch but omit command-list Execute entry/return and GPU completion.
- Runtime evidence: direct MHW frame 475 shows successful Depth/Velocity tags at 21:48:24.772250/295 and Dispatch at 21:48:24.773286.
- Counter-evidence: successful Dispatch/Present proves the sampled frame did not fail visibly; it does not prove the ordering.
- Next step: add buffered event correlation for the exact producer command list and its ExecuteCommandLists queue; do not add a wait in this audit.

### SDLX-006-B — No proven producer-to-XeFG cross-queue edge

- Status: INSUFFICIENT EVIDENCE
- Severity / impact: High if Qproducer differs from the actual XeFG consumer queue.
- Confidence: SOURCE-CONFIRMED absence of an OptiScaler Signal/Wait; vendor boundary unproven.
- Exact source: OptiScaler/hooks/Streamline_Hooks.cpp:213-227, 290-305, 315-338; OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1821-1847; repository-wide fence search.
- Preconditions: producer work is submitted to a queue different from XeFG's effective consumer queue.
- Reachable sequence: tagged resource -> no app-level Signal/Wait -> proxy Present -> internal vendor work.
- Contract gap: no checked-in XeFG client fence/API proves the cross-queue dependency; Streamline's DLSSG fence has opposite lifecycle semantics.
- Consequence: same-device queue ownership can be mistaken for ordering.
- Current MHW reachability: Candidate. REF proves distinct init/presentation topology, but OptiScaler does not log resource producer queues.
- Runtime evidence: all four paired MHW sessions report distinct_same_device for init/presentation queues.
- Counter-evidence: XeFG public documentation states it retains the init queue and submits interpolation using it; this could make a properly submitted same-queue path safe, but it does not identify each game resource queue.
- Next step: diagnostic-only queue/fence correlation and exact vendor contract check for the loaded libxess_fg build.

### SDLX-006-C — DLSSG completion fence is forwarded but not consumed

- Status: SAFE BUT FRAGILE
- Severity / impact: Medium as a contract misunderstanding; high only if a future fix uses it in the wrong direction.
- Confidence: SOURCE-CONFIRMED and API-CONTRACT-CONFIRMED.
- Exact source: external/streamline/sl_dlss_g.h:55-64, 149-197; OptiScaler/hooks/Streamline_Hooks.cpp:772-835.
- Preconditions: an engineer treats inputsProcessingCompletionFence as a new-resource producer fence.
- Reachable sequence: hkslDLSSGGetState -> copy/forward pointer/value -> no later OptiScaler read.
- Contract: the plugin-internal fence protects reuse/modification of previously presented DLSS-G inputs, especially on a non-presenting client queue.
- Consequence: adding a wait on it at XeFG input tag time could wait on the wrong event or leave the producer dependency unresolved.
- Current MHW reachability: The field is queried; OptiScaler does not consume it.
- Runtime evidence: Streamline warning about present-thread synchronization is emitted; field values are not logged.
- Next step: preserve the field as an observed application-facing contract until its loaded Streamline binary semantics are independently verified.

### SDLX-007-A — Type-blind global queue publication

- Status: SAFE BUT FRAGILE
- Severity / impact: High if distinct resource queues enter the conditional ResTrack path; queue submissions may go to the wrong global owner.
- Confidence: SOURCE-CONFIRMED conditional behavior.
- Exact source: OptiScaler/framegen/IFGFeature_Dx12.h:91-108; OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1855-1863; ResTrack_Dx12::hkExecuteCommandLists:660-689.
- Preconditions: SetResourceCmdList is called for a resource type and matching command list is observed by hkExecuteCommandLists; two types use different queues.
- Reachable sequence: QA matches Depth -> SetCommandQueue(Depth, QA) -> QB matches Velocity -> SetCommandQueue(Velocity, QB) -> _gameCommandQueue == QB.
- Contract gap: source has one global queue but no per-resource queue proof; D3D12 permits distinct direct queues on the same device.
- Consequence: OptiScaler-owned UI/SC helper lists later execute on the last queue, and queue identity no longer represents all resource producers.
- Current MHW reachability: Not observed in the current Streamline/XeFG call graph because SetResourceCmdList has no active caller.
- Runtime evidence: no resource-level Execute/queue logs.
- Next step: first determine whether a future intended registration path is required; if so, log type-to-queue identity before considering a narrowly scoped implementation.

### SDLX-007-B — ResizeBuffers1 observes only first queue element

- Status: SAFE BUT FRAGILE
- Severity / impact: Medium to High for multi-node/AFR or queue replacement paths; wrong alias/fence selection.
- Confidence: SOURCE-CONFIRMED; actual multi-entry MHW values unproven.
- Exact source: OptiScaler/wrapped/wrapped_swapchain.cpp:1214-1284; OptiScaler/hooks/FG_Hooks.cpp:872-1113.
- Preconditions: ppPresentQueue contains more than one meaningful entry or changes across resize.
- Reachable sequence: wrapper dereferences *ppPresentQueue -> updates State/current wrapper device from element 0 -> forwards complete array -> FGHooks waits on its old published queue -> XeFG owned queue remains init queue.
- Contract gap: DXGI defines an array of total size BufferCount and rotates through the entries.
- Consequence: State and wrapper aliases cannot be used as a complete representation of the queue used for every back buffer.
- Current MHW reachability: Runtime had BufferCount 3 but did not capture array contents; no multi-entry transition was exercised.
- Runtime evidence: six successful ResizeBuffers1 calls per paired session; binding queue and generation remained stable in REF logs.
- Counter-evidence: XeFG explicitly ignores ResizeBuffers1 queues for its internal ownership, so owned-queue non-update is expected.
- Next step: diagnostic array capture with node masks and COM identity, without changing forwarding behavior.

### SDLX-007-C — Wrapped WaitForGPUIdle helper cannot initialize its own fence

- Status: CONFIRMED DEFECT
- Severity / impact: Medium as a resize-only safety gap; not proven to be the E_ABORT cause.
- Confidence: SOURCE-CONFIRMED.
- Exact source: OptiScaler/wrapped/wrapped_swapchain.cpp:50-99.
- Preconditions: a resize path relies only on this translation unit's WaitForGPUIdle(IUnknown*).
- Reachable sequence: static resizeFence and resizeFenceEvent start null -> outer condition at line 66 requires both non-null -> fence creation at lines 82-83 is unreachable -> no Signal or CPU wait occurs.
- Contract gap: ResizeBuffers requires outstanding back-buffer and command-list use to be safe; this helper provides no wait in its current state.
- Consequence: the wrapper's apparent resize wait is not a real synchronization edge.
- Current MHW reachability: Mitigated or bypassed if FGHooks::WaitForGPUIdle is also reached; exact layer coverage is runtime-dependent.
- Runtime evidence: log text says Command queue obtained for GPU idle wait, but no fence value/Signal completion is logged.
- Counter-evidence: FGHooks owns a separate initialized resize fence and waits on its own published queue.
- Next step: isolate this helper in a separate, explicitly authorized resize audit; do not fix it in the current SDLX-006/007 audit.

### SDLX-007-D — Resize and queue alias state lacks a universal generation key

- Status: RUNTIME-DEPENDENT CANDIDATE
- Severity / impact: Medium; stale queue/resource association after lifecycle rollover.
- Confidence: SOURCE-CONFIRMED structural gap; runtime transition unexercised.
- Exact source: OptiScaler/State.h:300-320; OptiScaler/hooks/FG_Hooks.cpp:43-81, 147-269, 872-1113; OptiScaler/resource_tracking/ResTrack_dx12.cpp:2232-2256.
- Preconditions: a queue or command list from G survives while the swapchain/resource path is G+1, or a post-resize list is matched only by ring slot/pointer.
- Reachable sequence: new generation publishes FGHooks queue/generation -> ordinary resize leaves generation unchanged -> ring/resource maps use slot only -> conditional queue publication has no generation argument.
- Contract gap: source metadata does not tie resource readiness, command-list pointer, queue identity, and generation into one key.
- Consequence: an old pointer can be interpreted in a new lifecycle if cleanup/retirement ordering is incomplete.
- Current MHW reachability: Not observed; all paired logs remained generation 1 and showed no changed-binding event.
- Next step: capture generation on every resource/queue event before designing any cleanup or ownership change.

## 14. What was disproven or must not be reused

1. “Same device means same queue” is false. Runtime QueueIdentity evidence shows distinct COM identities with the same device identity in every paired MHW session.
2. “Different raw pointers automatically mean different queues” is also false in general. The runtime conclusion is strong here only because REFramework performed IUnknown identity, device identity, and descriptor comparisons.
3. “The DLSSG completion fence is automatically the missing producer fence” is not supported by the contract. Its documented direction concerns old input reuse after a prior present.
4. “D3D12TagFrameResource returning success means the producer GPU work is complete” is not supported by the XeFG contract.
5. “SetResourceReady means GPU-ready” is false in the current source. It is a CPU map/set transition.
6. “FGHooks resizeFence orders all frame resources” is false. It orders the queue supplied to the resize helper for an idle-before-resize operation.
7. “A successful ResizeBuffers1 or Present proves synchronization correctness” is false for a timing-sensitive race.
8. “ResTrack::SetResourceCmdList is active for the current Streamline/XeFG path” is false based on repository-wide call graph search.
9. “One paired MHW session with stable pointers proves all future queue generations stable” is false. The required changed-binding/array transition was not exercised.
10. “E_ABORT 4004 is caused by SDLX-006/007” remains unproven because the crash artifact lacks the resource/queue/fence timeline.

## 15. Required diagnostic design — no behavior change

The remaining opaque boundary is:

~~~text
game resource producer command list and queue
  -> actual GPU submission/completion
  -> XeFG internal consumer queue/work submission
  -> Intel driver/vendor Present completion
~~~

A future diagnostic-only patch should use a monotonic in-process event ID and a compact buffered record, not synchronous per-event file logging. It must not add a wait, Signal, queue replacement, Sleep, or logger-based timing effect.

Each record should contain:

- event sequence number;
- thread ID;
- frame token / frame ID / OptiScaler frame count / ring index;
- resource type;
- resource pointer;
- command-list pointer;
- resource validity and incoming D3D12 state;
- producer queue raw pointer;
- canonical real queue pointer and IUnknown identity when available;
- queue device identity;
- D3D12_COMMAND_QUEUE_DESC Type, Priority, Flags, NodeMask;
- XeFG owned queue and its identity;
- State::currentCommandQueue;
- FGHooks::currentCommandQueue and generation;
- Wrapped ResizeBuffers1 ppPresentQueue[i] and pCreationNodeMask[i] for every i in the effective BufferCount;
- current FG swapchain generation;
- DLSSG fence pointer/value when hkslDLSSGGetState returns them;
- D3D12TagFrameResource entry/return/result;
- SetResourceReady transition and source (SetResource or Execute hook);
- ExecuteCommandLists entry and return around the exact command-list array;
- SetCommandQueue transition with the resource type;
- XeFG Present/Dispatch entry and return;
- proxy Present/Present1 entry/return;
- D3D12/XeFG error or device-removed reason.

The minimum future source touchpoints are:

- Streamline_Hooks.cpp for tag/GetState boundaries;
- Streamline_Inputs_Dx12.cpp for frame/resource conversion;
- XeFG_Dx12.cpp for TagFrameResource, readiness, queue, Dispatch, and Present;
- ResTrack_dx12.cpp for Close/Execute correlation;
- wrapped_swapchain.cpp for complete ResizeBuffers1 arrays;
- FG_Hooks.cpp for generation/fence state.

No such patch was implemented in this audit.

### 15.1 Minimal deciding experiment

Use the same MHW binary pair and one controlled scene:

1. Run with the diagnostic buffer enabled and file logging disabled or minimized.
2. Capture one frame containing Depth, Velocity, and HUD-less tags.
3. Reconstruct R1 through R10 for the same frame ID.
4. Repeat across one ordinary ResizeBuffers1 and one true swapchain recreation.
5. Compare at least two runs without changing synchronization behavior.

Decision table:

| Observed condition | Decision |
|---|---|
| Qproducer == Qxefg consumer identity, producer Execute precedes Present, no queue replacement | SDLX-006 not demonstrated for that frame; same-queue contract may be sufficient |
| Qproducer == Qconsumer but Present/Dispatch precedes producer Execute | SDLX-006 becomes a confirmed reachable ordering defect |
| Qproducer != Qconsumer and no Signal/Wait or explicit vendor dependency | SDLX-006 becomes a confirmed conditional defect |
| Qproducer != Qconsumer but the loaded vendor contract or event trace proves an equivalent dependency | SDLX-006 is safe for that documented path |
| ppPresentQueue[i] differs across entries or from the previous lifecycle | SDLX-007 remains a queue-array/generation candidate; do not update only element 0 |
| SetCommandQueue is observed for different types on different queues | SDLX-007-A becomes a reachable global-queue ownership defect |
| no changed binding and no resource queue events | keep the finding runtime-dependent; do not promote from topology to root cause |

## 16. Completion gate

| Required question | Result |
|---|---|
| What do the DLSSG completion fence/value mean? | Plugin-internal completion for reuse/modification of previously presented DLSS-G inputs; exact signal site opaque |
| Who signals and who waits? | Plugin owns the internal signal semantics; host/client obtains the value on the present thread and waits when modifying old inputs on a non-presenting queue |
| Is it relevant to game-producer -> XeFG-consumer ordering? | Not proven; documented direction is different |
| What does the Streamline command buffer guarantee? | Host must ensure generating payload is already submitted or guaranteed before the provided command buffer; OptiScaler does not create that guarantee |
| Which command list carries Depth/Velocity/HUD-less work? | The Streamline cmdBuffer is copied to Dx12Resource::cmdList; actual producer list/queue is not logged |
| How is it registered for tracking? | Not registered by the active Streamline/XeFG path; SetResourceCmdList has no active caller there |
| On which queue is it submitted? | Not observable in current OptiScaler logs; conditional ResTrack hook observes the queue only when registered |
| When is resource CPU-ready? | After TagFrameResource in XeFG::SetResource; before any active producer Execute proof |
| What does TagFrameResource guarantee? | Resource/lifetime registration and possible copy/barrier management according to validity; not a client-visible cross-queue completion fence |
| What queue does XeFG use? | Public guide says the initialization queue is retained for interpolation; exact Intel internal/presentation boundary is not fully exposed |
| Is required GPU ordering provided? | Same-queue ordering may be sufficient when its preconditions are met; cross-queue ordering is not proven |
| What happens for null/sentinel cmdList? | ValidNow without a list is downgraded; old SDK UntilPresent may receive sentinel 1; sentinel is not synchronization |
| What is Qxefg-init / Qxefg-owned? | Initial canonicalized queue passed to D3D12InitFromSwapChainDesc and committed into _ownedGameCommandQueue |
| Can resource types overwrite it? | Conditional SetCommandQueue can overwrite one global pointer and ignores type; active current Streamline registration does not call it |
| Are producer queues allowed to differ? | D3D12 allows it; source and runtime do not prove they are equal for each resource |
| Are wrapper/proxy differences canonicalized? | Some Streamline proxy paths are canonicalized; all aliases are not universally IUnknown-compared |
| What are all ppPresentQueue[i] values? | Not captured; only the first element is used by the wrapper alias update |
| Which caches update after ResizeBuffers1? | Wrapper/State may update from element 0; FGHooks generation queue and XeFG-owned queue remain unchanged |
| Does XeFG owned queue update? | Public contract says ResizeBuffers1 queues are ignored for XeFG ownership; source does not update it |
| Can stale G state mix with G+1? | Structurally possible; not runtime-exercised |
| What do FGHooks resize fences order? | The published resize/lifecycle queue before resize/release; not per-frame input GPU visibility |
| Is distinct_same_device real? | Yes for the REF-observed init/presentation pair; COM identity and descriptor comparison prove the sampled relation |
| Does that prove E_ABORT cause? | No |
| Were SDLX-006 and SDLX-007 independently reclassified? | Yes: SDLX-006 runtime-dependent candidate; SDLX-007 safe but fragile |
| Was a fix implemented? | No |
| Recommended next action | DIAGNOSTIC ONLY, then a narrowly scoped future fix only if the captured edge proves it |

## 17. Source traceability

| Area | Primary source locations |
|---|---|
| Streamline tag interception | OptiScaler/hooks/Streamline_Hooks.cpp:147-340 |
| DLSSG state forwarding | OptiScaler/hooks/Streamline_Hooks.cpp:772-835, 947-960 |
| Streamline resource conversion | OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp:324-480 |
| CPU transactions/frame boundary | OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp:1-321, 489-501 |
| CPU readiness map | OptiScaler/framegen/IFGFeature.cpp:35-68, 312-319 |
| XeFG resource tag/readiness | OptiScaler/framegen/xefg/XeFG_Dx12.cpp:1599-1863 |
| XeFG init/owned queue | OptiScaler/framegen/xefg/XeFG_Dx12.cpp:469-685, 688-870, 1855-1863 |
| XeFG Dispatch/Present | OptiScaler/framegen/xefg/XeFG_Dx12.cpp:985-1283, 1484-1597 |
| ResTrack Close/Execute | OptiScaler/resource_tracking/ResTrack_dx12.cpp:625-698, 1607-1643 |
| ResTrack registration/cleanup | OptiScaler/resource_tracking/ResTrack_dx12.cpp:2205-2274 |
| FGHooks resize queue/fence | OptiScaler/hooks/FG_Hooks.cpp:43-100, 147-269, 620-858, 872-1113 |
| FGHooks Present | OptiScaler/hooks/FG_Hooks.cpp:1116-1327 |
| Wrapper LocalPresent/queue alias | OptiScaler/wrapped/wrapped_swapchain.cpp:148-300 |
| Wrapper ResizeBuffers1 | OptiScaler/wrapped/wrapped_swapchain.cpp:1214-1470 |
| State aliases/generations | OptiScaler/State.h:300-320 |
| Streamline contracts | external/streamline/sl_core_api.h:136-173; sl_core_types.h:326-418; sl_dlss_g.h:55-64, 149-197 |
| XeFG contracts | external/xess/inc/xess_fg/xefg_swapchain_d3d12.h:176-256; external/xess/doc/xess_fg_developer_guide_english.md:325-337, 699-761, 1014-1065 |
| REF identity proof | D:\repo\REFramework/src/compatibility/xefg/XeFGDiscovery.cpp:46-113, 222-225 |
| REF runtime binding/resize | D:\repo\REFramework/src/D3D12Hook.cpp:404-446, 643-697, 2161-2255 |

## 18. Final audit decision

The audit is closed at the correct boundary:

- SDLX-006 is not safe to call solved merely because TagFrameResource and Present return success. The source has a CPU-ready-before-submit path and no explicit producer-to-XeFG GPU fence. Its MHW causal status remains RUNTIME-DEPENDENT CANDIDATE because the producer and consumer GPU edges are not logged.
- SDLX-007 is not a simple “ResizeBuffers1 queue was not copied into XeFG” defect. XeFG's public contract explicitly retains its initialization queue and ignores ResizeBuffers1 queue inputs for XeFG ownership. The remaining issue is fragility: several aliases, incomplete ppPresentQueue observation, and a type-blind conditional global queue publication.
- The E_ABORT 4004 crash is confirmed as a crash-report message, not as an SDLX-006/007 causal result.
- The exact unproven boundary is Intel/driver/vendor GPU consumption and any hidden synchronization between the observed distinct queues.
- The smallest justified next action is DIAGNOSTIC ONLY. No code fix should be merged before one failing or controlled frame can answer: resource, producer command list, producer queue, CPU-ready point, GPU submission/completion, XeFG consumer queue, resize generation, and fence edge.
