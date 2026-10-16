

Streamline - DirectSR
=======================

>The focus of this guide is on using Streamline to integrate DirectSR into an
application.

> **NOTE:**
> When targeting NVIDIA RTX GPUs it is recommended to use the `sl.dlss` plugin
> instead of `sl.directsr`. Using DLSS directly has several benefits over
> DirectSR including (but not limited to):
> - Wider Graphics API support (D3D11, D3D12, Vulkan) vs D3D12 only.
> - Records into existing command-lists, giving applications finer work-scheduling power.
> - Support for full RGBA scaling with `sl::DLSSOptions::alphaUpscalingEnabled`.

Version 2.14.16
=======

### 1.0 CHECK IF DIRECTSR IS SUPPORTED

As soon as SL is initialized, you can check if DirectSR is available for the specific adapter you want to use:

```cpp
sl::AdapterInfo adapterInfo;

adapterInfo.deviceLUID = (uint8_t*)&desc.AdapterLuid;
adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
result = slIsFeatureSupported(sl::kFeatureDirectSR, slAdapterInfo);
```

### 2.0 TAG ALL REQUIRED RESOURCES

DirectSR can make use of the following buffer types.

| Input Name        | SL BufferType                         | Use      |
|-------------------|---------------------------------------|----------|
| Input Color       | `sl::kBufferTypeScalingInputColor`    | Required |
| Output Color      | `sl::kBufferTypeScalingInputColor`    | Required |
| Depth             | `sl::kBufferTypeDepth`                | Required |
| Motion Vectors    | `sl::kBufferTypeMvec`                 | Required |
| Exposure          | `sl::kBufferTypeExposure`             | Optional |

To tag a resource do the following:

```cpp

colorIn = {sl::ResourceType::eTex2d, myTAAUInput, nullptr, nullptr, (uint32_t)myTAAUResourceState};

// IMPORTANT: Make sure to mark resources which can be deleted or reused for other purposes within a frame as
// volatile
sl::ResourceTag colorInTag =
    sl::ResourceTag{&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow};

slSetTagForFrame(*currentFrame, viewport, &colorInTag, 1, cmdList);
// You can also provide multiple tags to a single slSetTagForFrame() call.
sl::ResourceTag colorInTag =
    sl::ResourceTag{&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow};
sl::ResourceTag colorOutTag =
    sl::ResourceTag{&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow};

sl::ResourceTag inputs[] = {colorInTag, colorOutTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);
```

> **NOTE:**
> If dynamic resolution is used then please specify the extent for each tagged resource. Please note that SL **manages resource states so there is no need to transition tagged resources**.

```cpp
// Extents are in {top, left, width, height} order.
// Define a rectangle of size (1920, 1080) at offset (0, 0)
sl::Extent inputExtent{0, 0, 1920, 1080};
sl::ResourceTag colorInTag = sl::ResourceTag{&colorIn,
                                             sl::kBufferTypeScalingInputColor,
                                             sl::ResourceLifecycle::eOnlyValidNow,
                                             &inputExtent};
```

### 3.0 QUERYING DIRECTSR VARIANTS

`slDirectSRGetVariantInfo()` can be called to populate an array of
`sl::DirectSRVariantInfo` structures describing the variants available to the
running DirectSR configuration. The `variantIndex` field of
`sl::DirectSROptions` is an index matching the results of
`slDirectSRGetVariantInfo()`.

### 4.0 PROVIDE DIRECTSR OPTIONS

The `sl::DirectSROptions` structure is used in conjunction with
`slDirectSRSetOptions()` to set peristent state to a viewport. It is also used
with `slDirectSRGetOptimalSettings()` to retrieve information about the
preferred format and size of the pre-upscale render target (input). Calling this
function is necessary to ensure that the input data is compatible with the
chosen variant for the target output resolution.

### 6.0 PROVIDE COMMON CONSTANTS

Various per frame camera related constants are required by all Streamline features and must be provided ***if any SL feature is active and as early in the frame as possible***. Please keep in mind the following:

* All SL matrices are row-major and should not contain any jitter offsets
* If motion vector values in your buffer are in {-1,1} range then motion vector scale factor in common constants should be {1,1}
* If motion vector values in your buffer are NOT in {-1,1} range then motion vector scale factor in common constants must be adjusted so that values end up in {-1,1} range

```cpp
sl::Constants consts = {};
// Set motion vector scaling based on your setup
consts.mvecScale = {1,1}; // Values in eMotionVectors are in [-1,1] range
consts.mvecScale = {1.0f / renderWidth,1.0f / renderHeight}; // Values in eMotionVectors are in pixel space
consts.mvecScale = myCustomScaling; // Custom scaling to ensure values end up in [-1,1] range
// Set all other constants here
if(SL_FAILED(result, slSetConstants(consts, *frameToken, myViewport))) // constants are changing per frame so frame index is required
{
    // Handle error, check logs
}
```
For more details please see [common constants](ProgrammingGuide.md#2111-common-constants)

### 5.0 ADD DIRECTSR TO THE RENDERING PIPELINE

Due to the design of the DirectSR API (which this plugin integrates),
`sl.directsr` does not execute on the command-list provided during calls to
`slEvaluateFeature()`. This differs from other Streamline plugins which are
command-list based. Instead, it runs on the `ID3D12CommandQueue` specified by
the `pCommandQueue` field of `sl::DirectSROptions`.

Because of this it is necessary to split up your command-lists before and after
the call to `slEvaluateFeature()` for DirectSR, possibly using `ID3D12Fence`
objects to synchronize the workload, ensuring that the DirectSR workload starts
and finishes execution without overlapping with dependent workloads.

> **NOTE:**
> It is still necessary to call `slEvaluateFeature()` when using `sl.directsr`.

### 6.0 TROUBLESHOOTING

If the DirectSR plugin is marked as not supported please check the following:

* Make sure you are using D3D12. DirectSR is not supported on D3D11 or Vulkan
* If you are using a "preview" version of the Agility SDK, make sure that you
  have [Windows Developer Mode](https://learn.microsoft.com/en-us/windows/apps/get-started/enable-your-device-for-development)
  enabled.

If the DirectSR output does not look right please check the following:

* If your motion vectors are in pixel space then scaling factors `sl::Constants::mvecScale` should be {1 / render width, 1 / render height}
* If your motion vectors are in normalized -1,1 space then scaling factors `sl::Constants::mvecScale` should be {1, 1}
* Make sure that jitter offset values are in pixel space
