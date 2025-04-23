
Streamline - NIS
=======================

>The focus of this guide is on using Streamline to integrate the NVIDIA Image Scaling (NIS) SDK into an application.  For more information about NIS itself, please visit the [NVIDIA Image Scaling SDK Github Page](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling)  
>For information on user interface considerations when using the NIS plugin, please see the ["RTX UI Developer Guidelines.pdf"](<RTX UI Developer Guidelines.pdf>) document included with this SDK.

Version 2.7.30
=======

### Introduction

The NVIDIA Image Scaling SDK (NIS) provides a single spatial scaling and sharpening algorithm for cross-platform support. The scaling algorithm uses a 6-tap scaling filter combined with 4 directional scaling and adaptive sharpening filters, which creates nice smooth images and sharp edges. In addition, the SDK provides a state-of-the-art adaptive directional sharpening algorithm for use in applications where no scaling is required. By integrating both NVIDIA Image Scaling and NVIDIA DLSS, developers can get the best of both worlds: NVIDIA DLSS for the best image quality, and NVIDIA Image Scaling for cross-platform support.

The directional scaling and sharpening algorithm are combined together in NVScaler while NVSharpen only implements the adaptive-directional-sharpening algorithm. Both algorithms are provided as compute shaders and developers are free to integrate them in their applications. Note that if you integrate NVScaler, you should NOT also integrate NVSharpen, as NVScaler already includes a sharpening pass.

For more information on the NVIDIA Image scaling SDK visit https://github.com/NVIDIAGameWorks/NVIDIAImageScaling

### 1.0 INITIALIZE AND SHUTDOWN

Call `slInit` as early as possible (before any dxgi/d3d11/d3d12 APIs are invoked)

```cpp
#include <sl.h>
#include <sl_consts.h>
#include <sl_nis.h>

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

### 2.0 CHECK IF NIS IS SUPPORTED

As soon as SL is initialized, you can check if NIS is available for the specific adapter you want to use:

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
            if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureNIS, adapterInfo)))
            {
                // Requested feature is not supported on the system, fallback to the default method
                switch (result)
                {
                    case sl::Result::eErrorOSOutOfDate:         // inform user to update OS
                    case sl::Result::eErrorDriverOutOfDate:     // inform user to update driver
                    case sl::Result::eErrorNoSupportedAdapter:  // cannot use this adapter (older or non-NVDA GPU etc)
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

### 3.0 TAG ALL REQUIRED RESOURCES

NIS requires render-res input color after TAA and final-res output color buffers. We can tag resources list this:

```cpp
// Showing two scenarios, depending if resources are immutable or volatile

// IMPORTANT: Make sure to mark resources which can be deleted or reused for other purposes within a frame as volatile

// FIRST SCENARIO

sl::Resource colorIn = sl::Resource{ sl::ResourceType::eTex2d, myNativeObject, nullptr, nullptr, myInitialState};
sl::Resource colorOut = sl::Resource{ sl::ResourceType::eTex2d, myNativeObject, nullptr, nullptr, myInitialState};
// Marked both resources as volatile since they can change
sl::ResourceTag colorInTag = sl::ResourceTag {&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &myExtent };
sl::ResourceTag colorOutTag = sl::ResourceTag {&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &myExtent };

// Resources must be valid at this point and valid command list must be provided since resources are volatile
sl::Resource inputs[] = {colorInTag, colorOutTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);

// SECOND SCENARIO

// Marked both resources as immutable
sl::ResourceTag colorInTag = sl::ResourceTag {&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &myExtent };
sl::ResourceTag colorOutTag = sl::ResourceTag {&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &myExtent };

// Resources are immutable so they are valid all the time, no need to provide command list since no copies need to be made
std::vector<sl::Resource> inputs = {colorInTag, colorOutTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);

```
> **IMPORTANT**
> When using Vulkan additional information about the resource must be provided (width, height, format, image view etc). See `sl::Resource` for details.

### 4.0 PROVIDE NIS OPTIONS

NIS options must be set so that the NIS plugin can track any changes made by the user:

```cpp

// Using helpers from sl_nis.h

sl::NISOptions nisOptions{};
nisOptions.mode = NISMode::eNISModeScaler; // use upscaling algorithm or use eNISModeSharpen for sharpening only
nisOptions.hdrMode = NISHDR::eNISHDRNone; // No HDR mode;
// These can be populated based on user selection in the UI
nisOptions.sharpness = myUI->getSharpness();
if(SL_FAILED(result, slNISSetOptions(viewport, nisOptions)))
{
    // Handle error here, check the logs
}
```
> **NOTE:**
> To use NIS sharpening only mode (with no up-scaling) set `sl::NISOptions.mode` to `sl::NISMode::eSharpen`

> **NOTE:**
> To turn off NIS set `sl::NISOptions.mode` to `sl::NISMode::eNISModeOff`or simply stop calling `slEvaluateFeature`, note that this does NOT release any resources, for that please use `slFreeResources`

### 5.0 ADD NIS TO THE RENDERING PIPELINE

On your rendering thread, call `slEvaluateFeature` at the appropriate location where up-scaling is happening. Please note that `myViewport` used in `slEvaluateFeature` must match the one used when setting NIS options and tags (unless options and tags are provided as part of evaluate inputs)

```cpp
// Make sure NIS is available and user selected this option in the UI
if(useNIS) 
{
    // NOTE: We can provide all inputs here or separately using slSetTagForFrame or slNISSetOptions

    // Inform SL that NIS should be injected at this point for the specific viewport
    const sl::BaseStructure* inputs[] = {&myViewport};
    if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureNIS, *frameToken, inputs, _countof(inputs), myCmdList)))
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
> Please note that **host is responsible for restoring the command buffer(list) state** after calling `slEvaluateFeature`. For more details on which states are affected please see [restore pipeline section](./ProgrammingGuideManualHooking.md#70-restoring-command-listbuffer-state)


