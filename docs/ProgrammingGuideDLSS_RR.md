

Streamline - DLSS-RR
=======================

>The focus of this guide is on using Streamline to integrate DLSS Ray Reconstruction (DLSS-RR) into an application.  For more information about DLSS-RR itself, please visit the [NVIDIA Developer DLSS Page](https://developer.nvidia.com/rtx/dlss)
>For information on user interface considerations when using the DLSS-RR plugin, please see the ["RTX UI Developer Guidelines.pdf"](<RTX UI Developer Guidelines.pdf>) document included in the DLSS SDK.

Version 2.14.16
=======

### 1.0 INITIALIZE AND SHUTDOWN

Call `slInit` as early as possible (before any dxgi/d3d11/d3d12 APIs are invoked)

```cpp
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_d.h>

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

### 2.0 CHECK IF DLSS-RR IS SUPPORTED

As soon as SL is initialized, you can check if DLSS-RR is available for the specific adapter you want to use:

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
            if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo)))
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

DLSS-RR is an extension to DLSS. To enable DLSS-RR, the Application sets up for DLSS-Super Resolution (DLSS) and toggles DLSS-RR on as an additional step. Effectively, DLSS-RR runs in the same Performance Quality Mode set for DLSS. 

Note: When DLSS-RR is switched on, it completely overrides DLSS (Super Resolution) It runs in the same Performance 

Next, we need to find out the rendering resolution and the optimal sharpness level based on DLSS settings:

```cpp

// Using helpers from sl_dlss_d.h

sl::DLSSDOptimalSettings dlssdSettings;
sl::DLSSOptions dlssdOptions;
// These are populated based on user selection in the UI
dlssdOptions.mode = myUI->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
dlssdOptions.outputWidth = myUI->getOutputWidth();    // e.g 1920;
dlssdOptions.outputHeight = myUI->getOutputHeight(); // e.g. 1080;
// Now let's check what should our rendering resolution be
if(SL_FAILED(result, slDLSSDGetOptimalSettings(dlssdOptions, dlssdSettings))
{
    // Handle error here
}
// Setup rendering based on the provided values in the sl::DLSSSettings structure
myViewport->setSize(dlssdSettings.renderWidth, dlssdSettings.renderHeight);
```

Note that the structure `sl::DLSSDOptimalSettings` will upon return from `slDLSSDGetOptimalSettings` contain information pertinent to DLSS dynamic resolution min and max source image sizes (if dynamic resolution is supported).

### 4.0 TAG ALL REQUIRED RESOURCES
#### 4.1 Buffers to tag
DLSS-RR requires the following buffers:

##### 4.1.1 Diffuse Albedo

Diffuse component of Reflectance material. Any standard 3-channel format provided at input resolution. The format must be linear, sRGB textures are not supported.

*SL Buffer Type* 

`kBufferTypeAlbedo`

##### 4.1.2 Specular Albedo

Specular component of Reflectance material. Any standard 3-channel format provided at input resolution. Refer Section 4.2.1 on notes for generating Specular Albedo. The format must be linear, sRGB textures are not supported.

*SL Buffer Type* 

`kBufferTypeSpecularAlbedo` 

##### 4.1.3 Normals

Shading Normals (normalized). Can be View Space or World Space. RGB_F16 or RGB_F32 provided at input resolution. 

*SL Buffer Type* 

`kBufferTypeNormalRoughness` , `kBufferTypeNormals`	 

*SL Parameter Notes* 

If using `kBufferTypeNormalRoughness`, you must set the `normalRoughnessMode` of `DLSSOptions` to `sl::DLSSDNormalRoughnessMode::ePacked` if packing Roughness into the Alpha channel, `sl::DLSSDNormalRoughnessMode::eUnPacked` otherwise 

##### 4.1.4 Roughness 

Linear Roughness of surface material provided at input resolution. As a standalone texture, you are encouraged to use a single channel format. Otherwise, it should be written into the R channel of that texture. Otherwise, you can pack Roughness into the Alpha channel of the Normals. 

*SL Buffer Type* 

`kBufferTypeRoughness` , `kBufferTypeNormalRoughness` 

*SL Parameter Notes* 

If using `kBufferTypeNormalRoughness`, you must set the `normalRoughnessMode` of `DLSSOptions` to `sl::DLSSDNormalRoughnessMode::ePacked`  

##### 4.1.5 Color Input

This is the Noisy Ray Traced Input Color. Any standard 3-channel format provided at input resolution.  

*SL Buffer Type* 

`kBufferTypeScalingInputColor`

##### 4.1.6 Motion Vectors

Dense motion vector field (i.e. includes camera motion, and motion of dynamic objects). RG16_FLOAT or RG32_FLOAT provided at Input Resolution. (For more information see section 3.6 of the DLSS Programming Guide). 

*SL Buffer Type*

`kBufferTypeMotionVectors`

##### 4.1.7 Depth Buffer

Same depth data used to generate motion vector data (View Space Depth or HW Depth). Any format with one channel in it (for instance R32_FLOAT or D32_FLOAT), as well as any depth-stencil format (for instance D24S8). Either view-space depth (`kBufferTypeLinearDepth`) or HW depth (`kBufferTypeDepth`) can be provided. Provided at input resolution. For more information see section 3.8 of the DLSS Programming Guide). 

*SL Buffer Type* 

`kBufferTypeLinearDepth`, `kBufferTypeDepth`

##### 4.1.8 Specular Motion Vector Reflections 	 

DLSS-RR uses Specular Motion Vectors to improve Image Quality of Reflections during motion. Application can either provide these directly or alternatively provide Specular Hit Distance with 1 and 2 matrices. 

Dense motion vector field for Reflections (Virtually Reflected Geometries) (i.e. includes camera motion, and motion of dynamic objects). RG16_FLOAT or RG32_FLOAT provided at input resolution.  

*SL Buffer Type* 

`kBufferTypeSpecularMotionVectors` 

##### 4.1.9 Specular  Hit Distance 

World Space distance between the Specular Ray Origin and Hit Point. Specular Ray Origin must be on the Primary Surface. Floating Point Scalar Value (FP16, or FP32). 

Additionally the Application need to provide it’s World To View Matrix and View To Clip Space Matrix. 

Note: Only needed if Specular Motion Vectors are not provided. 

*SL Buffer Type* 

`kBufferTypeSpecularHitDistance` with `DLSSDOptions::worldToCameraView`, `DLSSDOptions::cameraViewToWorld`

##### 4.1.10 Transparency Overlay

Optional only - A buffer that has particles or other transparent effects rendered into it instead of passing it as part of the input color. 

Single standard 4-channel input - where RGB must be pre-multiplied with Alpha, Alpha channel is the blending factor.

Or 2 separate 3-channel inputs - One representing color (RcGcBc), other representing Alpha (RaGaBa). 

*SL Buffer Type* 

`kBufferTypeTransparencyLayer, kBufferTypeTransparencyLayerOpacity `

##### 4.1.11 Color Before Transparency Guide

Optional only - Snapshot of noisy color buffer before any transparencies are rendered on top. The main purpose of this guide is to help with ghosting or disappearing particles. If this is not a problem in your application, this buffer is probably not needed. 

It is important to use the same image format as for the noisy color input since small precision differences between the buffers may cause DLSS-RR to interpret the pixel as having transparencies, which can lead to artifacts under difficult lighting conditions.

In certain cases, rendering some transparent objects into the guide may improve image quality. If this guide is needed in the first place, the recommendation is to start with just opaque pixels, and if there are image quality problems, try adding the problematic transparent objects to the guide.

*SL Buffer Type* 

`kBufferTypeColorBeforeTransparency `

##### 4.1.12 Screen Space Sub Surface Scattering Guide

Optional only - Input buffer for specifying Sub Surface Scattering (SSS) guide.

In general the buffer should contain luminance(colorAfterSSS - colorBeforeSSS).

If SSS blur is computed based on diffuse light, diffuse albedo should be applied before computing luminance, in
which case the formula used should be:
luminance ((diffuseAfterSSS - diffuseBeforeSSS) * diffuseAlbedo)
where luminance(s) = (s.r + (2 * s.g) + s.b)  / 4

Pixels without SSS material must have 0 in the guide.

1-channel FP16 format provided at input resolution.

*SL Buffer Type*

`kBufferTypeScreenSpaceSubsurfaceScatteringGuide`

##### 4.1.13 Depth of Field Guide

Optional only - Input buffer for specifying Depth of Field (DOF) guide.

In general the buffer should contain luminance(colorAfterDOF - colorBeforeDOF),
where luminance(s) = (s.r + (2 * s.g) + s.b)  / 4

Pixels without DOF must have 0 in the guide.

Alternatively, depth of field can be applied after DLSS-RR, in which case this buffer can be omitted.

1-channel FP16 format provided at input resolution.

*SL Buffer Type*

`kBufferTypeDepthOfFieldGuide`

##### 4.1.14 Output

Destination for the Denoised full resolution frame. Any standard 3 or 4-channel format provided at output resolution. 

*SL Buffer Type*

`KBufferTypeScalingOutputColor`



```cpp

// IMPORTANT: Make sure to mark resources which can be deleted or reused for other purposes within a frame as volatile
// Prepare resources (assuming d3d11/d3d12 integration so leaving Vulkan view and device memory as null pointers)
sl::Extent inputExtent{ 0, 0, inputWidth, inputHeight };
sl::Extent outputExtent{ 0, 0, scaledOutputWidth, scaledOutputHeight };

sl::Resource colorIn = {sl::ResourceType::Tex2d, myTAAUInput, nullptr, nullptr, nullptr};
sl::Resource colorOut = {sl::ResourceType::Tex2d, myTAAUOutput, nullptr, nullptr, nullptr};
sl::Resource depth = {sl::ResourceType::Tex2d, myDepthBuffer, nullptr, nullptr, nullptr};
sl::Resource mvec = {sl::ResourceType::Tex2d, myMotionVectorsBuffer, nullptr, nullptr, nullptr};
sl::Resource diffuseAlbedo = sl::Resource{ sl::ResourceType::Tex2d, myDiffuseBuffer, nullptr, nullptr, nullptr};
sl::Resource specularAlbedo = sl::Resource{ sl::ResourceType::Tex2d, mySpecularBuffer, nullptr, nullptr, nullptr};
sl::Resource normalRoughness = sl::Resource{ sl::ResourceType::Tex2d, myNormalsRoughnessBuffer, nullptr, nullptr, nullptr};
sl::Resource specMvecs = sl::Resource{ sl::ResourceType::Tex2d, myspecMvecBuffer, nullptr, nullptr, nullptr};

sl::ResourceTag colorInTag = sl::ResourceTag {&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
sl::ResourceTag colorOutTag = sl::ResourceTag {&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent };
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
sl::ResourceTag mvecTag = sl::ResourceTag {&mvec, sl::kBufferTypeMvec, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
sl::ResourceTag diffuseAlbdedoTag = sl::ResourceTag{ &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
sl::ResourceTag specularAlbedoTag = sl::ResourceTag{ &specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
sl::ResourceTag normalRoughnessTag = sl::ResourceTag{ &normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
sl::ResourceTag specMvecsTag = sl::ResourceTag{ &specMvecs, sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };

// Tag in group
sl::Resource inputs[] = {colorInTag, colorOutTag, depthTag, mvecTag, diffuseAlbdedoTag, specularAlbedoTag, normalRoughnessTag, specMvecsTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);
```

> **NOTE:**
> If dynamic resolution is used then please specify the extent for each tagged resource. Please note that SL **manages resource states so there is no need to transition tagged resources**.

#### 4.2 Additional Resource Guidelines

##### 4.2.1 Specular Albedo Generation

Specular Albedo may be computed with this function 

```cpp
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV) 
{ 
  NoV = abs(NoV); 
  // [Ray Tracing Gems, Chapter 32]
  float4 X; 
  X.x = 1.f; 
  X.y = NoV; 
  X.z = NoV * NoV; 
  X.w = NoV * X.z; 
  float4 Y; 
  Y.x = 1.f; 
  Y.y = alpha; 
  Y.z = alpha * alpha; 
  Y.w = alpha * Y.z; 
  float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f); 
  float3x3 M2 = float3x3(1.f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f); 
  float2x2 M3 = float2x2(0.0365463f, 3.32707, 9.0632f, -9.04756); 
  float3x3 M4 = float3x3(1.f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f); 
  float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw)); 
  float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw)); 
  // This is a hack for specular reflectance of 0
  bias *= saturate(SpecularColor.g * 50); 
  **return** mad(SpecularColor, max(0, scale), max(0, bias)); 
} 

// Here fRoughness is expected to be linear roguhness and may be engine dependent 
outSpecularAlbedo = EnvBRDFApprox2( GBuffer.SpecularAlbedo, GBuffer.fRoughness * GBuffer.fRoughness, NdotV ); 
```

##### 4.2.2 Roughness

As a standalone texture, it should be written into the R channel of that texture. Otherwise, you can pack into the alpha channel of the Normals texture in which case you must set the `normalRoughnessMode` of `DLSSOptions` to `sl::DLSSDNormalRoughnessMode::ePacked`  (See section below on providing DLSS + DLSS-RR Options)

### 5.0 PROVIDE DLSS + DLSS-RR OPTIONS

DLSS-RR options are an extension of DLSS options and must be set in addition to compatible DLSS options so that the DLSS-RR plugin can track any changes made by the user:

```cpp
sl::DLSSDOptions dlssdOptions = {};
// Set preferred Render Presets per Perf Quality Mode. These are typically set one time
// and established while evaluating DLSS RR Image Quality for your Application.
// It will be set to DLSSDPreset::eDefault if unspecified.
// Please Refer to section 3.12 of the DLSS RR Programming Guide for details.
dlssdConstants.dlaaPreset = sl::DLSSDPreset::ePresetD;
dlssdConstants.qualityPreset = sl::DLSSDPreset::ePresetD;
dlssdConstants.balancedPreset = sl::DLSSDPreset::ePresetD;
dlssdConstants.performancePreset = sl::DLSSDPreset::ePresetD;
dlssdConstants.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;
// These are populated based on user selection in the UI
dlssdConstants.mode = myUI->DLSS_Mode; // DLSS-RR uses the same Perf Quality Mode set for DLSS. e.g. sl::eDLSSModeBalanced;
dlssdConstants.outputWidth = myUI->getOutputWidth();    // e.g 1920;
dlssdConstants.outputHeight = myUI->getOutputHeight(); // e.g. 1080;
dlssdConstants.colorBuffersHDR = sl::Boolean::eTrue; // must be HDR pipeline
dlssdConstants.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked; // Linear Roughness is packed in Alpha channel of Normal Texture
dlssdOptions.alphaUpscalingEnabled = sl::Boolean::eFalse; // set to eTrue to upscale alpha channel of color texture in addition to RGB

if(SL_FAILED(result, slDLSSDSetOptions(viewport, dlssOptions)))
{
    // Handle error here, check the logs
}
```

> **NOTE:**
> `sl::DLSSDPreset::ePresetA` to `sl::DLSSDPreset::ePresetC` are no longer available.

> **NOTE:**
> To turn off DLSS-RR set `sl::DLSSDOptions.mode` to `sl::DLSSDMode::eOff`, note that this does NOT release any resources, for that please use `slFreeResources`
>
> **NOTE:**
> DLSS-RR only support HDR inputs and Low Resolution . So `sl::DLSSOptions.colorBuffersHDR` **must** be set to `eTrue`
>
> **NOTE:**
> DLSS-RR will ignore DLSS options `sharpness` and `useAutoExposure`

> **NOTE:**
> Alpha upscaling (`DLSSOptions::alphaUpscalingEnabled`) may impact performace. This feature should be used only if the alpha channel of the color texture needs to be upscaled (if `eFalse`, only RGB channels will be upscaled).

> **NOTE:**
> DLSS-RR does not support Dynamic Resolution Scaling (DRS). Attempting to change input resolution while rendering will trigger denoiser re-initialization.


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

### 7.0 ADD DLSS-RR TO THE RENDERING PIPELINE

On your rendering thread, call `slEvaluateFeature` at the appropriate location where up-scaling is happening. Please note that when using `slSetTag`, `slSetConstants` and `slDLSSSetOptions` the `frameToken` and `myViewport` used in `slEvaluateFeature` **must match across all API calls**.

```cpp
// Make sure DLSS-RR is available and user selected this option in the UI
if(useDLSSD) 
{
    // NOTE: We can provide all inputs here or separately using slSetTag, slSetConstants or slDLSSSetOptions

    // Inform SL that DLSS should be injected at this point for the specific viewport
    const sl::BaseStructure* inputs[] = {&myViewport};
    if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), myCmdList)))
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
> Please note that **host is responsible for restoring the command buffer(list) state** after calling `slEvaluate`. For more details on which states are affected please see [restore pipeline section](./ProgrammingGuideManualHooking.md#70-restoring-command-listbuffer-state)

### 8.0 MULTIPLE VIEWPORTS

Here is a code snippet showing one way of handling two viewports with explicit resource allocation and de-allocation:

```cpp
// Viewport1
{
    // We need to setup our constants first so sl.dlss plugin has enough information
    sl::DLSSOptions dlssdOptions = {};
    dlssdOptions.mode = viewport1->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
    dlssdOptions.outputWidth = viewport1->getOutputWidth();    // e.g 1920;
    dlssdOptions.outputHeight = viewport1->getOutputHeight(); // e.g. 1080;
    // Note that we are passing viewport id 1
    slDLSSDSetOptions(viewport1->id, dlssdOptions);
    
    // Set our tags, note that we are passing viewport id
    setTag(viewport1->id, &tags2, numTags2);
    // and so on ...

    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDLSS_RR, viewport1->id);

    // Evaluate DLSS on viewport1, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DLSS resources would be initialized at this point
    slEvaluateFeature(sl::kFeatureDLSS_RR, myFrameIndex, viewport1->id, nullptr, 0, myCmdList);

    // Assuming the above evaluate call is still pending on the CL, make sure to flush it before releasing resources
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDLSS_RR, viewport1->id);
}

// Viewport2
{
    // We need to setup our constants first so sl.dlss plugin has enough information
    sl::DLSSOptions dlssdOptions = {};
    dlssdOptions.mode = viewport2->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
    dlssdOptions.outputWidth = viewport2->getOutputWidth();    // e.g 1920;
    dlssdOptions.outputHeight = viewport2->getOutputHeight(); // e.g. 1080;
    // Note that we are passing viewport id 2
    slDLSSDSetOptions(viewport2->id, dlssdOptions);

    // Set our tags, note that we are passing viewport id
    setTag(viewport2->id, &tags2, numTags2);
    // and so on ...

    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDLSS_RR, viewport2->id);
    
    // Evaluate DLSS on viewport2, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DLSS resources would be initialized at this point
    slEvaluateFeature(sl::kFeatureDLSS_RR, myFrameIndex, viewport2->id, nullptr, 0, myCmdList);

    // Assuming the above evaluate call is still pending on the CL, make sure to flush it before releasing resources
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDLSS_RR, viewport2->id);
}

```

### 9.0 CHECK STATE AND VRAM USAGE

To obtain current state for a given viewport the following API can be used:

```cpp
sl::DLSSState dlssdState{};
if(SL_FAILED(result, slDLSSGetState(viewport, dlssdState))
{
    // Handle error here
}
// Check how much memory DLSS is using for this viewport
dlssState.estimatedVRAMUsageInBytes
```

### 10.0 TROUBLESHOOTING

If the DLSS-RR output does not look right please check the following:

* If your motion vectors are in pixel space then scaling factors `sl::Constants::mvecScale` should be {1 / render width, 1 / render height}
* If your motion vectors are in normalized -1,1 space then scaling factors `sl::Constants::mvecScale` should be {1, 1}
* Make sure that jitter offset values are in pixel space
* `NVSDK_NGX_Parameter_FreeMemOnRelease` is replaced with `slFreeResources`
* `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes` is handled automatically based on tagged motion vector buffer's size and extent.

Please further refer to chapter 8.1 (Troubleshooting via Debug overlays) of the DLSS-RR Integration Guide.  