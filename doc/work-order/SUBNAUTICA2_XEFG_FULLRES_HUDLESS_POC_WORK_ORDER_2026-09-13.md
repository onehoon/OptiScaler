# Work Order: Subnautica 2 XeFG Full-Resolution HUDLess POC

Date: 2026-09-13  
Repository: `onehoon/OptiScaler`  
Existing PR: #21 (`diag: trace Subnautica 2 XeFG geometry on release/0.9`)  
Base: `release/0.9`  
Head: `diag/subnautica2-xefg-geometry-release-0.9`

## 1. Objective

Implement the next proof-of-concept as **an additional implementation commit on the existing PR #21 branch**.

The purpose is to test one specific hypothesis:

> Subnautica 2 reaches XeFG, but the game-provided Streamline HUDLess resource is smaller than the physical swapchain / UI resource. If OptiScaler creates a real full-resolution HUDLess intermediate and submits that intermediate to XeFG, XeFG may accept the resource geometry and frame generation may begin working.

This is a geometry/compatibility POC, not a production game quirk yet.

Do **not** create a new PR for this work. Keep PR #21 draft and add the implementation to its current head branch.

## 2. Current confirmed context from PR #21

PR #21 already contains diagnostics for:

- NGX render/output dimensions;
- Streamline resource extents and physical texture sizes;
- HUDLess/UI geometry;
- XeFG resource geometry;
- interpolation rectangle selection;
- final XeFG BACKBUFFER tag;
- physical swapchain dimensions.

The current branch also contains a POC that tags the XeFG BACKBUFFER as the full physical swapchain instead of the smaller interpolation rectangle.

Observed Subnautica 2 geometry has included cases such as:

- physical swapchain: `1920x1200`;
- Streamline/DLSS output / HUDLess: approximately `1536x960`;
- lower internal DLSS render resolution below the HUDLess size;

and equivalent behavior has also been observed at higher output resolutions, e.g. HUDLess around `2048x1152` for a `2560x1440` output.

The important mismatch is therefore not only metadata. The actual HUDLess D3D12 texture can be smaller than UI/BACKBUFFER.

## 3. Required experiment

For the Subnautica 2 failure case, convert the game-provided HUDLess texture into a **real D3D12 texture whose physical dimensions equal the current physical swapchain dimensions**, then submit that new texture as `FG_ResourceType::HudlessColor` to XeFG.

Expected geometry after the POC:

```text
Game HUDLess physical texture   1536x960
             |
             | OptiScaler spatial upscale
             v
POC HUDLess intermediate        1920x1200
UI resource                     1920x1200
XeFG BACKBUFFER tag             1920x1200
Physical swapchain              1920x1200
```

For a 2560x1440 case, the same rule applies dynamically:

```text
2048x1152 -> 2560x1440
```

Do not hard-code `1.25`, `1920x1200`, `2560x1440`, or any specific source resolution.

Target width/height must come from the **current physical swapchain**.

Source width/height must come from the **actual HUDLess resource / resolved HUDLess geometry**, not from guessed NGX quality ratios.

## 4. Critical implementation rule: do not fake the dimensions

Do not solve this by only changing:

- `resourceParam.resourceSize`;
- interpolation width/height;
- XeFG metadata;
- Streamline tag extents;
- BACKBUFFER metadata.

Do not tell XeFG that a `1536x960` texture is `1920x1200`.

The POC must create a real `1920x1200` resource and populate it with a spatially scaled version of the game HUDLess image.

Also, `CopyTextureRegion` / `CopyResource` alone is not sufficient because D3D12 copy operations do not perform spatial scaling.

## 5. Reuse existing shader infrastructure where practical

The `release/0.9` tree already contains D3D12 output-scaling infrastructure:

- `OptiScaler/shaders/output_scaling/OS_Dx12.h`
- `OptiScaler/shaders/output_scaling/OS_Dx12.cpp`

`OS_Dx12` already supports:

- creating an output buffer from a source texture format;
- SRV/UAV compute dispatch;
- linear/static sampling;
- an existing upsample path using the precompiled upscale shader.

However, **do not blindly call the current `OS_Dx12::Dispatch()` for the HUDLess POC** without checking its dimension source.

The current implementation builds shader constants from:

```cpp
State::Instance().currentFeature->TargetWidth();
State::Instance().currentFeature->TargetHeight();
State::Instance().currentFeature->DisplayWidth();
State::Instance().currentFeature->DisplayHeight();
```

For Subnautica 2, NGX internal render size, DLSS output/HUDLess size, and physical swapchain size are different concepts. The POC specifically needs:

```text
src = actual HUDLess dimensions
dst = actual physical swapchain dimensions
```

Therefore use one of these two approaches, in preference order:

### Preferred: explicit-dimension overload with no behavior change to existing callers

Add an overload or internal helper to `OS_Dx12` which accepts explicit source/destination geometry, for example conceptually:

```cpp
bool Dispatch(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* input,
    ID3D12Resource* output,
    uint32_t srcWidth,
    uint32_t srcHeight,
    uint32_t dstWidth,
    uint32_t dstHeight);
```

The existing public behavior must remain unchanged for existing output-scaling users. The existing overload may delegate to the new explicit-dimension implementation using its current Target/Display dimensions.

### Acceptable alternative: small dedicated HUDLess scaler

If reusing `OS_Dx12` cleanly would create broader coupling or alter existing output-scaling behavior, create a very small D3D12 HUDLess scaler based on the same `Shader_Dx12` / precompiled shader infrastructure.

Do not duplicate large shader systems unless necessary.

## 6. XeFG integration point

Primary integration point:

- `OptiScaler/framegen/xefg/XeFG_Dx12.cpp`
- `XeFG_Dx12::SetResource(Dx12Resource* inputResource)`

PR #21 already reaches the following flow:

```text
inputResource
  -> _frameResources[fIndex][type]
  -> GetResourceData(type, fIndex)
  -> XeFGProxy::D3D12TagFrameResource(...)
```

For `FG_ResourceType::HudlessColor`, before building/submitting the final XeFG resource data:

1. Inspect the actual source HUDLess resource dimensions.
2. Read the current physical swapchain dimensions from the same authoritative state already used by PR #21 for the BACKBUFFER POC.
3. If HUDLess already physically matches the swapchain, do nothing and keep the normal path.
4. If HUDLess is smaller than the swapchain and the POC is enabled, create/reuse a full-resolution intermediate texture.
5. Dispatch the spatial upscale from the original HUDLess into the full-resolution intermediate on an appropriate graphics command list.
6. Make the frame resource presented to `GetResourceData()` / XeFG refer to the scaled intermediate.
7. Its geometry must be `0,0,swapchainWidth,swapchainHeight`.
8. Submit that resource to XeFG.

Conceptual flow only:

```cpp
if (type == FG_ResourceType::HudlessColor && ShouldRunFullResHudlessPoc(...))
{
    const auto srcDesc = originalHudless->GetDesc();
    const uint32_t dstWidth = state.currentSwapchainDesc.BufferDesc.Width;
    const uint32_t dstHeight = state.currentSwapchainDesc.BufferDesc.Height;

    if (srcDesc.Width != dstWidth || srcDesc.Height != dstHeight)
    {
        // Ensure reusable intermediate matching source format + swapchain dimensions.
        // Spatially upscale source into intermediate with explicit src/dst dimensions.
        // Preserve correct D3D12 transitions.
        // Then point fResource/copy at the intermediate and set geometry to full size.
    }
}
```

This is guidance, not copy/paste code. Adapt it to the actual resource wrapper and ownership model.

## 7. POC scoping / safety

This experiment must not silently become a global XeFG behavior change.

Use the narrowest practical gate available on this branch.

Preferred POC gating options:

1. existing exact-game / executable matching if a suitable mechanism already exists on `release/0.9`;
2. otherwise a clearly named temporary opt-in config switch for the POC;
3. if neither can be added without unrelated architecture work, use a narrowly documented diagnostic guard that cannot affect normal configurations by default.

Do not enable full-resolution HUDLess scaling globally for every XeFG title.

Do not introduce a generalized game-quirk framework into `release/0.9` solely for this experiment.

The production quirk design comes only after the experiment proves that full-resolution HUDLess fixes XeFG.

## 8. Resource lifetime and allocation

The intermediate texture must be reusable. Do not allocate a committed D3D12 resource every frame.

Recommended ownership:

- one reusable HUDLess upscale resource per required frame-buffer/index if the existing XeFG resource lifetime requires it; or
- reuse the existing shader-owned output buffer only if its lifetime is demonstrably compatible with XeFG's `UNTIL_NEXT_PRESENT` use.

Recreate only when one of these changes:

- device;
- source format / compatibility requirements;
- physical swapchain width/height;
- swapchain recreation;
- other existing resource conditions that require recreation.

Release the POC resources from the existing XeFG cleanup / swapchain destruction lifecycle.

Do not leave stale resources across resize or device recreation.

## 9. D3D12 state requirements

Keep state handling explicit and conservative.

The implementation must account for:

- original HUDLess incoming state;
- SRV-readable state for the scale dispatch;
- UAV state for the intermediate output;
- the state XeFG will receive in `xefg_swapchain_d3d12_resource_data_t`;
- restoring the original game resource state when required;
- avoiding use of an invalid/null command list.

Reuse existing OptiScaler barrier helpers where possible.

Do not add extra synchronization or queue waits unless a real command-ordering requirement is demonstrated. Keep this POC on the command-list path that already owns the HUDLess tag when possible.

## 10. Geometry metadata after scaling

When the scaled intermediate is used as HUDLess, XeFG must see geometry consistent with the physical texture.

Required:

```text
HUDLess pResource physical desc = swapchainWidth x swapchainHeight
HUDLess resourceBase            = 0,0
HUDLess resourceSize            = swapchainWidth x swapchainHeight
UI physical/resource geometry   = existing full-resolution UI geometry
BACKBUFFER                       = PR #21 full-swapchain POC geometry
```

Do not change depth or motion-vector geometry in this commit unless XeFG produces a new concrete validation failure requiring it.

This commit tests HUDLess only.

## 11. Diagnostics to add

Keep PR #21 diagnostics and add concise POC-specific logging.

At minimum log once per relevant resource/frame path:

```text
[RES-POC][HUDLESS-SCALE]
frame/index
source physical WxH
source logical/base/size
swapchain WxH
intermediate physical WxH
format
whether scaling ran
whether XeFG received original or scaled HUDLess
```

Also log the resulting XeFG HUDLess `resourceBase`, `resourceSize`, and physical resource description after substitution.

Do not flood the log with per-dispatch internal shader details beyond what is needed to diagnose success/failure.

## 12. Failure behavior

This POC must fail safely.

If any of the following occurs:

- intermediate allocation failure;
- unsupported resource format;
- scaler initialization failure;
- no usable command list;
- shader dispatch failure;
- invalid swapchain dimensions;

then:

1. log a clear `[RES-POC][HUDLESS-SCALE]` error/warning;
2. do not crash;
3. do not submit a bogus full-size metadata description for the original smaller texture;
4. either fall back to the original HUDLess path or abort the XeFG submission using the branch's existing failure behavior, whichever is safer in the actual code path.

Never pair the original low-resolution physical texture with forged full-resolution `resourceSize` metadata.

## 13. Do not touch in this commit

Do not:

- modify the game executable or game files;
- patch Subnautica 2 memory/code;
- change NGX DLSS render/output resolution;
- change the game's DLSS quality mode;
- force Windows/DPI/swapchain size;
- change Streamline's original tag supplied by the game globally;
- redesign interpolation rectangle behavior beyond what PR #21 already does;
- change UI composition logic;
- change depth or velocity scaling;
- add a production Subnautica 2 quirk;
- upstream this POC;
- refactor unrelated XeFG or output-scaling code.

## 14. Validation matrix

Build the same `release/0.9` target used by PR #21 and test Subnautica 2 with DLSSG -> XeFG.

Minimum validation:

### A. 1920x1200 output case

Confirm log sequence shows approximately:

```text
original HUDLess     1536x960
scaled HUDLess       1920x1200
UI                   1920x1200
BACKBUFFER            1920x1200
swapchain             1920x1200
```

Exact source HUDLess size may vary with game settings; do not encode the example value as a requirement.

### B. 2560x1440 output case, if available

Confirm dynamic behavior such as:

```text
original HUDLess     ~2048x1152
scaled HUDLess       2560x1440
UI                   2560x1440
BACKBUFFER            2560x1440
```

### C. Already-matching HUDLess case

If source HUDLess already matches swapchain size, verify the scaler is bypassed and the original resource is used.

### D. Resize / resolution change

Change output resolution or recreate the swapchain and verify the intermediate is recreated at the new dimensions without a crash or stale resource.

### E. POC disabled / non-target game

Verify existing behavior is unchanged.

## 15. Success criteria

Primary success criterion:

- XeFG no longer rejects the frame because of HUDLess/UI/BACKBUFFER resource-resolution mismatch, and frame generation starts producing interpolated frames / measurable FPS increase.

Secondary success criteria:

- no D3D12 device removal;
- no crash/hang;
- no obvious corrupted output;
- no resource allocation every frame;
- resize remains safe;
- existing non-POC path remains unchanged.

## 16. If the POC still fails

Do not keep adding speculative fixes in the same commit.

Capture the **next exact XeFG result/error and the complete geometry log**.

If XeFG proceeds past HUDLess/UI geometry and rejects another input such as depth or motion vectors, stop and report that as the next concrete blocker.

If XeFG accepts all geometry but interpolation still does not occur, report that separately; that would indicate the root cause is no longer merely the HUDLess/UI/BACKBUFFER size mismatch.

## 17. Production decision after the POC

Do not implement this section yet.

If the POC works, the next design phase is to decide between:

1. keeping an OptiScaler-created full-resolution HUDLess intermediate as a targeted Subnautica 2 compatibility quirk; or
2. reverse-engineering the game rendering path only to identify an existing native full-resolution pre-UI texture, then having OptiScaler substitute that resource instead of performing an additional upscale.

Game reverse engineering would therefore be used to discover a better resource for OptiScaler to consume; it does not imply modifying the game executable.

## 18. Expected implementation commit

After following this work order, add **one focused implementation commit** to the current PR #21 head branch.

Suggested commit message:

```text
poc: upscale Subnautica 2 HUDLess to XeFG swapchain size
```

Keep PR #21 as draft after the implementation. Do not squash or merge until runtime validation confirms whether XeFG starts generating frames.
