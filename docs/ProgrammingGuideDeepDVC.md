
Streamline - DeepDVC
=======================

>The focus of this guide is on using Streamline to integrate RTX Dynamic Vibrance ("DeepDVC") into an application.
>For information on user interface considerations when using the DeepDVC plugin, please see the ["RTX UI Developer Guidelines.pdf"](<RTX UI Developer Guidelines.pdf>) document included with this SDK.

RTX Dynamic Vibrance ("DeepDVC") uses AI to enhance digital vibrance in real-time, improving visual clarity and adjusting color saturation adaptively to the specific game.

The filter is controlled by two parameters:
| Parameter | What it does | What you'll notice | Range |
|--------------|--------------|-----------|------------|
| Intensity | Controls how strong or subtle the filter effect will be on an image. | A low intensity will keep the images closer to the original, while a high intensity will make the filter effect more pronounced. A zero value will result in the original image. | [0, 1] |
| Saturation Boost | Enhances the colors in your image, making them more vibrant and eye-catching. | This setting will only be active if you've turned up the Intensity. Once active, you'll see colors pop up more, making the image look more lively.  | [0, 1] |

Version 2.14.16
=======

### 1.0 CHECK IF DEEPDVC IS SUPPORTED

As soon as SL is initialized, you can check if DeepDVC is available for the specific adapter you want to use:

```cpp
DXGI_ADAPTER_DESC adapterDesc; // output from DXGIAdapter::GetDesc() for adapter to query
sl::AdapterInfo adapterInfo{};
...
result = slIsFeatureSupported(sl::kFeatureDeepDVC, slAdapterInfo);
```

### 2.0 TAG ALL REQUIRED RESOURCES

DeepDVC only requires final-res output color buffers.

```cpp
// IMPORTANT: Make sure to mark resources which can be deleted or reused for other purposes within a frame as volatile

// Prepare resources (assuming d3d11/d3d12 integration so leaving Vulkan view and device memory as null pointers)
sl::Resource colorOut = {sl::ResourceType::eTex2d, myTAAUOutput, nullptr, nullptr, nullptr};

sl::ResourceTag colorOutTag = sl::ResourceTag{&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &myExtent};

// Tag in group
sl::Resource inputs[] = {colorOutTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);
```

### 3.0 PROVIDE DEEPDVC OPTIONS

DeepDVC options must be set so that the DeepDVC plugin can track any changes made by the user:

```cpp
sl::DeepDVCOptions deepDVCOptions = {};
// These are populated based on user selection in the UI
deepDVCOptions.mode = myUI->getDeepDVCMode(); // e.g. sl::DeepDVCMode::eOn;
deepDVCOptions.intensity = myUI->getDeepDVCIntensity(); // e.g. 0.5
deepDVCOptions.saturationBoost = myUI->getDeepDVCSaturationBoost(); // e.g. 0.75
if(SL_FAILED(result, slDeepDVCSetOptions(viewport, deepDVCOptions)))
{
    // Handle error here, check the logs
}
```

> **NOTE:**
> To turn off DeepDVC set `sl::DeepDVCOptions.mode` to `sl::DeepDVCOptions::eOff`. Note that this does NOT release any resources, for that please use `slFreeResources`.

### 4.0 ADD DEEPDVC TO THE RENDERING PIPELINE

The call to evaluate the DeepDVC feature must occurs during the post-processing phase after tone-mapping. Applying DeepDVC in linear HDR in-game color-space may result in undesirables color effects. Since DeepDVC can enhance noisy or grainy regions, it is recommended that certain effects such as film grain should occur after DeepDVC.

On your rendering thread, call `slEvaluateFeature` at the appropriate location. Please note that when using `slSetTag` and `slDeepDVCSetOptions` the `frameToken` and `myViewport` used in `slEvaluateFeature` **must match across all API calls**.

```cpp
// Make sure DeepDVC is available and user selected this option in the UI
if(useDeepDVC) 
{
    // NOTE: We can provide all inputs here or separately using slSetTag, slSetConstants or slDeepDVCSetOptions

    // Inform SL that DeepDVC should be injected at this point for the specific viewport
    const sl::BaseStructure* inputs[] = {&myViewport};
    if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureDeepDVC, *frameToken, inputs, _countof(inputs), myCmdList)))
    {
        // Handle error
    }
    else
    {
        // IMPORTANT: Host is responsible for restoring state on the command list used
        restoreState(myCmdList);
    }
}
```

> **IMPORTANT:**
> Please note that **host is responsible for restoring the command buffer(list) state** after calling `slEvaluate`. For more details on which states are affected please see [restore pipeline section](./ProgrammingGuideManualHooking.md#70-restoring-command-listbuffer-state)

### 5.0 MULTIPLE VIEWPORTS

Here is a code snippet showing one way of handling two viewports with explicit resource allocation and de-allocation:

```cpp
// Viewport1
{
    // We need to setup our constants first so sl.deepdvc plugin has enough information
    sl::DeepDVCOptions deepDVCOptions = {};
    deepDVCOptions.mode = viewport1->getDeepDVCMode(); // e.g. sl::DeepDVCMode::eOn;
    deepDVCOptions.intensity = viewport1->getDeepDVCIntensity(); // e.g. 0.5
    deepDVCOptions.saturationBoost = viewport1->getDeepDVCSaturationBoost(); // e.g. 0.75
    // Note that we are passing viewport id 1
    slDeepDVCSetOptions(viewport1->id, deepDVCOptions);
    
    // Set our tags, note that we are passing viewport id
    setTag(viewport1->id, &tags2, numTags2);
    // and so on ...

    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDeepDVC, viewport1->id);

    // Evaluate DeepDVC on viewport1, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DeepDVC resources would be initialized at this point
    slEvaluateFeature(sl::kFeatureDeepDVC, myFrameIndex, viewport1->id, nullptr, 0, myCmdList);

    // Assuming the above evaluate call is still pending on the CL, make sure to flush it before releasing resources
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDeepDVC, viewport1->id);
}

// Viewport2
{
    // We need to setup our constants first so sl.deepdvc plugin has enough information
    sl::DeepDVCOptions deepDVCOptions = {};
    deepDVCOptions.mode = viewport2->getDeepDVCMode(); // e.g. sl::DeepDVCMode::eOn;
    deepDVCOptions.intensity = viewport2->getDeepDVCIntensity(); // e.g. 0.5
    deepDVCOptions.saturationBoost = viewport2->getDeepDVCSaturationBoost(); // e.g. 0.75
    // Note that we are passing viewport id 2
    slDeepDVCSetOptions(viewport2->id, deepDVCOptions);

    // Set our tags, note that we are passing viewport id
    setTag(viewport2->id, &tags2, numTags2);
    // and so on ...

    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDeepDVC, viewport2->id);
    
    // Evaluate DeepDVC on viewport2, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DeepDVC resources would be initialized at this point
    slEvaluateFeature(sl::kFeatureDeepDVC, myFrameIndex, viewport2->id, nullptr, 0, myCmdList);

    // Assuming the above evaluate call is still pending on the CL, make sure to flush it before releasing resources
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDeepDVC, viewport2->id);
}

```
### 6.0 CHECK STATE AND VRAM USAGE

To obtain current state for a given viewport the following API can be used:

```cpp
sl::DeepDVCState deepDVCState{};
if(SL_FAILED(result, slDeepDVCGetState(viewport, deepDVCState))
{
    // Handle error here
}
// Check how much memory DeepDVC is using for this viewport
deepDVCState.estimatedVRAMUsageInBytes
```

### 7.0 LIMITATIONS

Current DeepDVC implementation supports SDR inputs in display-deferred color-space after tone mapping. Applying DeepDVC on HDR images may introduce undesirable color artifacts.
