

Streamline - DLSS
=======================

>The focus of this guide is on using Streamline to integrate DLSS into an application.  For more information about DLSS itself, please visit the [NVIDIA Developer DLSS Page](https://developer.nvidia.com/rtx/dlss)
>For information on user interface considerations when using the DLSS plugin, please see the ["RTX UI Developer Guidelines.pdf"](<RTX UI Developer Guidelines.pdf>) document included with this SDK.

Version 2.14.16
=======

### 1.0 INITIALIZE AND SHUTDOWN

Call `slInit` as early as possible (before any dxgi/d3d11/d3d12 APIs are invoked)

```cpp
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

sl::Preferences pref{};
pref.showConsole = true; // for debugging, set to false in production
pref.logLevel = sl::eLogLevelDefault;
pref.pathsToPlugins = {}; // change this if Streamline plugins are not located next to the executable
pref.numPathsToPlugins = 0; // change this if Streamline plugins are not located next to the executable
pref.pathToLogsAndData = {}; // change this to enable logging to a file
pref.logMessageCallback = myLogMessageCallback; // highly recommended to track warning/error messages in your callback
pref.applicationId = myId; // Provided by NVDA, required if using NGX components (DLSS 2/3)
pref.engineType = myEngine; // If using UE or Unity
pref.engineVersion = myEngineVersion; // Optional version
pref.projectId = myProjectId; // Optional project id
if(SL_FAILED(res, slInit(pref)))
{
    // Handle error, check the logs
    if(res == sl::Result::eErrorDriverOutOfDate) { /* inform user */}
    // and so on ...
}
```

For more details please see [preferences](ProgrammingGuide.md#222-preferences)

Call `slShutdown()` before destroying dxgi/d3d11/d3d12/vk instances, devices and other components in your engine.

```cpp
if(SL_FAILED(res, slShutdown()))
{
    // Handle error, check the logs
}
```

#### 1.1 SET THE CORRECT DEVICE

Once the main device is created call `slSetD3DDevice` or `slSetVulkanInfo`:

```cpp
if(SL_FAILED(res, slSetD3DDevice(nativeD3DDevice)))
{
    // Handle error, check the logs
}
```

### 2.0 CHECK IF DLSS IS SUPPORTED

As soon as SL is initialized, you can check if DLSS is available for the specific adapter you want to use:

```cpp
Microsoft::WRL::ComPtr<IDXGIFactory> factory;
if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
{
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter{};
    uint32_t i = 0;
    while (factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
        {
            sl::AdapterInfo adapterInfo{};
            adapterInfo.deviceLUID = (uint8_t*)&desc.AdapterLuid;
            adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
            if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo)))
            {
                // Requested feature is not supported on the system, fallback to the default method
                switch (result)
                {
                    case sl::Result::eErrorOSOutOfDate:             // inform user to update OS
                    case sl::Result::eErrorDriverOutOfDate:         // inform user to update driver
                    case sl::Result::eErrorNoSupportedAdapterFound: // cannot use this adapter (older or non-NVDA
                                                                    // GPU etc)
                        break;
                        // and so on ...
                };
            }
            else
            {
                // Feature is supported on this adapter!
            }
        }
        i++;
    }
}
```

### 3.0 CHECK DLSS SETTINGS AND SETUP VIEWPORT RENDERING SIZE

Next, we need to find out the rendering resolution and the optimal sharpness level based on DLSS settings:

```cpp

// Using helpers from sl_dlss.h

sl::DLSSOptimalSettings dlssSettings;
sl::DLSSOptions dlssOptions;
// These are populated based on user selection in the UI
dlssOptions.mode = myUI->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
dlssOptions.outputWidth = myUI->getOutputWidth();    // e.g 1920;
dlssOptions.outputHeight = myUI->getOutputHeight(); // e.g. 1080;
// Now let's check what should our rendering resolution be
if(SL_FAILED(result, slDLSSGetOptimalSettings(dlssOptions, dlssSettings))
{
    // Handle error here
}
// Setup rendering based on the provided values in the sl::DLSSSettings structure
myViewport->setSize(dlssSettings.renderWidth, dlssSettings.renderHeight);
```

Note that the structure `sl::DLSSOptimalSettings` will upon return from `slDLSSGetOptimalSettings` contain information pertinent to DLSS dynamic resolution min and max source image sizes (if dynamic resolution is supported).

### 4.0 TAG ALL REQUIRED RESOURCES

DLSS requires depth, motion vectors, render-res input color and final-res output color buffers.

```cpp

// IMPORTANT: Make sure to mark resources which can be deleted or reused for other purposes within a frame as volatile

// Prepare resources (assuming d3d11/d3d12 integration so leaving Vulkan view and device memory as null pointers)
sl::Resource colorIn = {sl::ResourceType::Tex2d, myTAAUInput, nullptr, nullptr, nullptr};
sl::Resource colorOut = {sl::ResourceType::Tex2d, myTAAUOutput, nullptr, nullptr, nullptr};
sl::Resource depth = {sl::ResourceType::Tex2d, myDepthBuffer, nullptr, nullptr, nullptr};
sl::Resource mvec = {sl::ResourceType::Tex2d, myMotionVectorsBuffer, nullptr, nullptr, nullptr};
sl::Resource exposure = {sl::ResourceType::Tex2d, myExposureBuffer, nullptr, nullptr, nullptr};

sl::ResourceTag colorInTag = sl::ResourceTag {&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &myExtent };
sl::ResourceTag colorOutTag = sl::ResourceTag {&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &myExtent };
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };
sl::ResourceTag mvecTag = sl::ResourceTag {&mvec, sl::kBufferTypeMvec, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent };
sl::ResourceTag exposureTag = sl::ResourceTag {&exposure, sl::kBufferTypeExposure, sl::ResourceLifecycle::eOnlyValidNow, &my1x1Extent};

// Tag in group
sl::Resource inputs[] = {colorInTag, colorOutTag, depthTag, mvecTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);
```

> **NOTE:**
> If dynamic resolution is used then please specify the extent for each tagged resource. Please note that SL **manages resource states so there is no need to transition tagged resources**.

> **NOTE:**
> If `sl::kBufferTypeExposure` is NOT provided or `dlssOptions.useAutoExposure` is set to be true then DLSS will be in auto-exposure mode (`NVSDK_NGX_DLSS_Feature_Flags_AutoExposure` will be set automatically)

### 5.0 PROVIDE DLSS OPTIONS

DLSS options must be set so that the DLSS plugin can track any changes made by the user:

```cpp
sl::DLSSOptions dlssOptions = {};
// Set preferred Render Presets per Perf Quality Mode. These are typically set one time
// and established while evaluating DLSS SR Image Quality for your Application.
// It will be set to DSSPreset::eDefault if unspecified.
// Please Refer to section 3.12 of the DLSS Programming Guide for details.
dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetK;
dlssOptions.qualityPreset = sl::DLSSPreset::ePresetK;
dlssOptions.balancedPreset = sl::DLSSPreset::ePresetK;
dlssOptions.performancePreset = sl::DLSSPreset::ePresetM;
dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
// These are populated based on user selection in the UI
dlssOptions.mode = myUI->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
dlssOptions.outputWidth = myUI->getOutputWidth();    // e.g 1920;
dlssOptions.outputHeight = myUI->getOutputHeight(); // e.g. 1080;
dlssOptions.sharpness = dlssSettings.sharpness; // optimal sharpness
dlssOptions.colorBuffersHDR = sl::Boolean::eTrue; // assuming HDR pipeline
dlssOptions.useAutoExposure = sl::Boolean::eFalse; // autoexposure is not to be used if a proper exposure texture is available
dlssOptions.alphaUpscalingEnabled = sl::Boolean::eFalse; // experimental alpha upscaling, enable to upscale alpha channel of color texture
if(SL_FAILED(result, slDLSSSetOptions(viewport, dlssOptions)))
{
    // Handle error here, check the logs
}
```

> **NOTE:**
> `sl::DLSSPreset::ePresetA` to `sl::DLSSPreset::ePresetD` are no longer available. `sl::DLSSPreset::ePresetG` to `sl::DLSSPreset::ePresetI` are reserved and should also not be used.
`sl::DLSSPreset::ePresetE` and `sl::DLSSPreset::ePresetF` are legacy presets. Support for these has been deprecated.

> **NOTE:**
> To turn off DLSS set `sl::DLSSOptions.mode` to `sl::DLSSMode::eOff`, note that this does NOT release any resources, for that please use `slFreeResources`

> **NOTE:**
> Set the DLSSOptions.useAutoExposure boolean to be true only if you want DLSS to be in in auto-exposure mode. Also, it is strongly advised to provide exposure if a proper exposure texture is available. 

> **NOTE:**
> Alpha upscaling (`DLSSOptions::alphaUpscalingEnabled`) is experimental, and will impact performace. This feature should be used only if the alpha channel of the color texture needs to be upscaled (if `eFalse`, only RGB channels will be upscaled).

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

### 7.0 ADD DLSS TO THE RENDERING PIPELINE

On your rendering thread, call `slEvaluateFeature` at the appropriate location where up-scaling is happening. Please note that when using `slSetTag`, `slSetConstants` and `slDLSSSetOptions` the `frameToken` and `myViewport` used in `slEvaluateFeature` **must match across all API calls**.

```cpp
// Make sure DLSS is available and user selected this option in the UI
if(useDLSS) 
{
    // NOTE: We can provide all inputs here or separately using slSetTag, slSetConstants or slDLSSSetOptions

    // Inform SL that DLSS should be injected at this point for the specific viewport
    const sl::BaseStructure* inputs[] = {&myViewport};
    if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), myCmdList)))
    {
        // Handle error
    }
    else
    {
        // IMPORTANT: Host is responsible for restoring state on the command list used
        restoreState(myCmdList);
    }
}
else
{
    // Default up-scaling pass like for example TAAU goes here
}
```

> **IMPORTANT:**
> Plase note that **host is responsible for restoring the command buffer(list) state** after calling `slEvaluate`. For more details on which states are affected please see [restore pipeline section](./ProgrammingGuideManualHooking.md#70-restoring-command-listbuffer-state)

### 8.0 MULTIPLE VIEWPORTS

Here is a code snippet showing one way of handling two viewports with explicit resource allocation and de-allocation:

```cpp
// Viewport1
{
    // We need to setup our constants first so sl.dlss plugin has enough information
    sl::DLSSOptions dlssOptions = {};
    dlssOptions.mode = viewport1->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
    dlssOptions.outputWidth = viewport1->getOutputWidth();    // e.g 1920;
    dlssOptions.outputHeight = viewport1->getOutputHeight(); // e.g. 1080;
    // Note that we are passing viewport id 1
    slDLSSSetOptions(viewport1->id, dlssOptions);
    
    // Set our tags, note that we are passing viewport id
    setTag(viewport1->id, &tags2, numTags2);
    // and so on ...

    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDLSS, viewport1->id);

    // Evaluate DLSS on viewport1, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DLSS resources would be initialized at this point
    slEvaluateFeature(sl::kFeatureDLSS, myFrameIndex, viewport1->id, nullptr, 0, myCmdList);

    // Assuming the above evaluate call is still pending on the CL, make sure to flush it before releasing resources
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDLSS, viewport1->id);
}

// Viewport2
{
    // We need to setup our constants first so sl.dlss plugin has enough information
    sl::DLSSOptions dlssOptions = {};
    dlssOptions.mode = viewport2->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
    dlssOptions.outputWidth = viewport2->getOutputWidth();    // e.g 1920;
    dlssOptions.outputHeight = viewport2->getOutputHeight(); // e.g. 1080;
    // Note that we are passing viewport id 2
    slDLSSSetOptions(viewport2->id, dlssOptions);

    // Set our tags, note that we are passing viewport id
    setTag(viewport2->id, &tags2, numTags2);
    // and so on ...

    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDLSS, viewport2->id);
    
    // Evaluate DLSS on viewport2, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DLSS resources would be initialized at this point
    slEvaluateFeature(sl::kFeatureDLSS, myFrameIndex, viewport2->id, nullptr, 0, myCmdList);

    // Assuming the above evaluate call is still pending on the CL, make sure to flush it before releasing resources
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDLSS, viewport2->id);
}

```

### 9.0 CHECK STATE AND VRAM USAGE

To obtain current state for a given viewport the following API can be used:

```cpp
sl::DLSSState dlssState{};
if(SL_FAILED(result, slDLSSGetState(viewport, dlssState))
{
    // Handle error here
}
// Check how much memory DLSS is using for this viewport
dlssState.estimatedVRAMUsageInBytes
```

### 10.0 TROUBLESHOOTING

If the DLSS output does not look right please check the following:

* If your motion vectors are in pixel space then scaling factors `sl::Constants::mvecScale` should be {1 / render width, 1 / render height}
* If your motion vectors are in normalized -1,1 space then scaling factors `sl::Constants::mvecScale` should be {1, 1}
* Make sure that jitter offset values are in pixel space
* `NVSDK_NGX_Parameter_FreeMemOnRelease` is replaced with `slFreeResources`
* `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes` is handled automatically based on tagged motion vector buffer's size and extent.
