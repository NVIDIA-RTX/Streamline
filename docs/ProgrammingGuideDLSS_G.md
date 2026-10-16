

Streamline - DLSS-G
=======================

NVIDIA DLSS Frame Generation (“DLSS-FG” or “DLSS-G”) is an AI-based technology that infers frames from rendered frames produced by a game engine or rendering pipeline. This document explains how to integrate DLSS-G into a renderer.

The diagram below illustrates this integration. Traditionally, the render pipeline contains discrete GPU stages that build a frame in the swap chain back buffer before culminating in a frame present. The Streamline DLSS Frame Generation plugin intercepts this present call with a Streamline proxy swap chain, coordinates execution of the frame generation algorithm, and paces frames for a smooth user experience. Specific resources from the render pipeline must be tagged before present so Frame Generation can infer additional frames.

![dlssg_docs_overview](./media/dlssg_docs_overview.png "NVIDIA Streamline DLSS Frame Generation Overview")

Version 2.14.16
=======

### 0.0 Integration checklist

See Section 15.0 and the sections referenced in the table below for further details on these items.

Item | Reference | Confirmed
---|---|---
All required inputs are passed to Streamline: depth buffers, motion vectors, and HUD-less color buffers | [Section 5.0](#50-tag-all-required-resources) |
Common constants and frame index are provided for **each frame** using `slSetConstants` and `slSetFeatureConstants` | [Section 7.0](#70-provide-common-constants) |
All tagged buffers are valid at frame present time and are not reused for other purposes | [Section 5.0](#50-tag-all-required-resources) |
Tag buffers with unique id 0 | [Section 5.0](#50-tag-all-required-resources) |
Ensure the frame index provided with the common constants matches the presented frame | [Section 8.0](#80-integrate-sl-reflex) |
Verify that inputs passed to Streamline look correct, including camera matrices and dynamic objects | [SL ImGUI guide](<Debugging - SL ImGUI (Realtime Data Inspection).md>) |
The application checks the signature of sl.interposer.dll to verify it is a genuine NVIDIA library | [Streamline programming guide, section 2.1.1](./ProgrammingGuide.md#211-security) |
Requirements for Dynamic Resolution are met (if the game supports Dynamic Resolution) | [Section 10.0](#100-dlss-g-and-dynamic-resolution) |
DLSS-G is turned off (by setting `sl::DLSSGOptions::mode` to `sl::DLSSGMode::eOff`) when the game is paused, loading, in menus, or otherwise not rendering game frames, and when changing resolution or switching between full-screen and windowed mode | [Section 12.0](#120-dlss-g-and-dxgi) |
The swap chain is recreated every time DLSS-G is turned on or off (by changing `sl::DLSSGOptions::mode`) to avoid unnecessary performance overhead when DLSS-G is switched off | [Section 18.0](#180-how-to-avoid-unnecessary-overhead-when-dlss-g-is-turned-off) |
Reduce the amount of motion blur; when DLSS-G is enabled, halve the distance or magnitude of motion blur | N/A |
Reflex is properly integrated (see checklist in Reflex Programming Guide) | [Section 8.0](#80-integrate-sl-reflex) |
In-game UI for enabling/disabling DLSS-G is implemented | [RTX UI Guidelines](<RTX UI Developer Guidelines.pdf>) |
Only full production non-watermarked libraries are packaged in the release build | N/A |
No errors or unexpected warnings in Streamline and DLSS-G log files while running the feature | N/A |
Ensure the extent resolution or resource size (whichever applies) of the `Hudless` and `UI Color and Alpha` buffers exactly matches that of the back buffer. | N/A |
Execute the DLSS-G In-Game Enhanced Debug Visualization Tests | [Section 21.0](#210-enhanced-in-game-debug-visualization) | |
Check `bIsVsyncSupportAvailable` before exposing VSync toggle in UI | [Section 22.0](#220-vsync-with-frame-generation) | |
Application either does not use `GetFrameLatencyWaitableObject`/`SetMaximumFrameLatency` while sl.dlss_g is loaded and paces via `slReflexSleep`, or requests `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and manages frame latency itself | [Section 12.1](#121-frame-latency-waitable-objects) | |

### 1.0 REQUIREMENTS

**NOTE:** DLSS-G requires the following Windows versions and settings. If any requirement is not met, DLSS-G will be unavailable and Streamline will log an error:

* Minimum Windows OS version of Win10 20H1 (version 2004, build 19041 or higher)
* Display Hardware-accelerated GPU Scheduling (HWS) must be enabled via Settings : System : Display : Graphics : Change default graphics settings.

#### 1.1 Run-time Performance

DLSS-G's execution time and memory footprint vary across different engines and
integrations. To provide a general guideline, NVIDIA has profiled DLSS-G in-game
on various NVIDIA GeForce RTX GPUs. These results serve as a rough estimate of
run-time execution and can help developers estimate the potential savings
offered by DLSS.

**Execution Cost**

| GeForce SKU | Resolution | 2x Cost (ms) | 4x Cost (ms) |
|-------------|------------|--------------|--------------|
| RTX 4090    | 1080p      | 1.35         | -            |
| RTX 4090    | 1440p      | 1.78         | -            |
| RTX 4090    | 4k         | 2.77         | -            |
| RTX 5080    | 1080p      | 1.45         | 2.43         |
| RTX 5080    | 1440p      | 2.04         | 3.61         |
| RTX 5080    | 4k         | 2.84         | 5.25         |
| RTX 5090    | 1080p      | 1.07         | 1.78         |
| RTX 5090    | 1440p      | 1.46         | 2.48         |
| RTX 5090    | 4k         | 1.72         | 3.32         |

**Memory Cost**

The DLSS-G memory footprint does not change significantly between single-frame
generation and multi-frame generation modes.

| Resolution | VRAM Estimate (MB) |
|------------|--------------------|
| 1080p      | 272                |
| 1440p      | 489                |
| 4k         | 725                |

### 2.0 INITIALIZATION AND SHUTDOWN

Call `slInit` as early as possible (before any D3D12/Vulkan APIs are invoked).

```cpp
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>

sl::Preferences pref;
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

For more details, see [preferences](ProgrammingGuide.md#222-preferences).

Call `slShutdown()` before destroying DXGI, D3D12, or Vulkan instances, devices, and other components in your engine.

```cpp
if(SL_FAILED(res, slShutdown()))
{
    // Handle error, check the logs
}
```

#### 2.1 SET THE CORRECT DEVICE

Once the main device is created, call `slSetD3DDevice` or `slSetVulkanInfo`:

```cpp
if(SL_FAILED(res, slSetD3DDevice(nativeD3DDevice)))
{
    // Handle error, check the logs
}
```

### 3.0 CHECK IF DLSS-G IS SUPPORTED

As soon as SL is initialized, you can check if DLSS-G is available for the specific adapter you want to use:

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
            if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo)))
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

#### 3.1 CHECKING DLSS-G'S CONFIGURATION AND SPECIAL REQUIREMENTS

For DLSS-G to work correctly, certain requirements regarding the OS, driver, and other settings on the user's machine must be met. To obtain DLSS-G configuration and verify that all requirements are satisfied, use the following code snippet:

```cpp
sl::FeatureRequirements requirements{};
if (SL_FAILED(result, slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements)))
{
    // Feature is not requested on slInit or failed to load, check logs, handle error
}
else
{
    // Feature is loaded; we can check the requirements
    requirements.flags & FeatureRequirementFlags::eD3D12Supported
    requirements.flags & FeatureRequirementFlags::eVulkanSupported
    requirements.maxNumViewports
    // and so on ...
}
```
> **NOTE:**
> DLSS-G runs optical flow in interop mode in Vulkan by default. To leverage the potential performance benefit of running optical flow natively in Vulkan, the client must meet the minimum requirements: NVIDIA driver version 527.64 or later on Windows, 525.72 or later on Linux, and `VK_API_VERSION_1_1` (recommended: `VK_API_VERSION_1_3`).
> In manual hooking mode, additional requirements apply as described in section 5.2.1 of ProgrammingGuideManualHooking.md.

### 4.0 HANDLE MULTIPLE SWAP-CHAINS

DLSS-G automatically attaches to any swap chain created by the application **unless manual hooking is used**. In editor mode, there may be multiple swap chains, but DLSS-G should attach only to the main one where frame interpolation is used.
Here is how DLSS-G can be enabled on a single swap chain:

```cpp
// This is just one example, swap-chains can be created at any point in time and in any order.
// SL features also can be loaded/unloaded at any point in time and in any order.

// Unload DLSS-G (this can be done at any point in time and as many times as needed)
slSetFeatureLoaded(sl::kFeatureDLSS_G, false);

// Create swap chains for which DLSS-G is NOT required
IDXGISwapChain1* swapChain{};
factory->CreateSwapChainForHwnd(device, hWnd, desc, nullptr, nullptr, &swapChain);
// and so on

// Load DLSS-G (this can be done at any point in time and as many times as needed)
slSetFeatureLoaded(sl::kFeatureDLSS_G, true);

// Create main swap chains for which DLSS-G is required
IDXGISwapChain1* mainSwapChain{};
factory->CreateSwapChainForHwnd(device, hWnd, desc, nullptr, nullptr, &mainSwapChain);

// From this point onward, DLSS-G automatically manages only mainSwapChain; other swap chains use the standard DXGI implementation

```

### 5.0 TAG ALL REQUIRED RESOURCES

#### 5.1 REQUIRED AND OPTIONAL RESOURCES

DLSS-G requires the following buffers for frame generation:
- **Backbuffer** (automatically intercepted via the Streamline swap chain)
- **Depth**
- **Motion Vectors**

**Backbuffer Subrect Support**

If DLSS-G is intended to run only on a specified subrectangle of the final color
buffer, you must also tag the backbuffer:
- Use the backbuffer tag to pass the subrect coordinates to Streamline.
- The actual backbuffer resource pointer is optional.
- See [Section 5.2](#52-tagging-recommendations) for additional
  details.

**UI Handling**

For the best image quality, it is **critical** to provide a Hudless (pre-UI)
buffer and a UI buffer. The frame generation algorithm can use these resources
to reduce distortion of HUD elements during interpolation.

- **Hudless** - The scene color _before_ any UI/HUD elements are drawn.
- **UI Buffer** - Choose one of:
  - **UI Alpha** (Preferred): A single-channel image containing only the opacity
    (alpha) of the UI. This is the most performant option.
  - **UI Color and Alpha**: A full-color 4-channel image containing the color
    and alpha of the UI.
  - _Note: If both buffers are tagged, Streamline will use the UI Alpha buffer._

If both Hudless and UI alpha are tagged, you can also enable user interface
recomposition for a further improvement to UI interpolation quality. See
[Section 6.6](#66-enabling-user-interface-recomposition).

For best results, the size of the Hudless and UI buffers should match the size
of the backbuffer.

Input | Requirements/Recommendations | Reference Image
---|---|---
Final Color | - *No requirements, this is intercepted automatically via SL's SwapChain API* | ![dlssg_final_color](./media/dlssg_docs_final_color.png "DLSSG Input Example: Final Color")
Final Color Subrect | - Subregion of the final color buffer to run frame-generation on. <br> - Subrect-external backbuffer region is copied as is to the generated frame. <br> - Tag backbuffer optionally, only to pass in backbuffer subrect info. <br> - Extent resolution or resource size, whichever is in use, for `Hudless`, `UI Color and Alpha`, and `UI Alpha` buffers should exactly match that of backbuffer. <br> - Refer to [Section 5.2](#52-tagging-recommendations) below for details. | ![dlssg_final_color_subrect](./media/dlssg_docs_final_color_subrect.png "DLSSG Input Example: Final Color Subrect")
Depth | - Same depth data used to generate motion vector data <br> - `sl::Constants` depth-related data (e.g., `depthInverted`) should be set accordingly<br>  - *Note: this is the same set of requirements as DLSS-SR, and the same depth can be used for both* | ![dlssg_depth](./media/dlssg_docs_depth.png "DLSSG Input Example: Depth")
Motion Vectors | - Dense motion vector field (i.e. includes camera motion, and motion of dynamic objects) <br> - *Note: this is the same set of requirements as DLSS-SR, and the same motion vectors can be used for both* | ![dlssg_mvec](./media/dlssg_docs_mvec.png "DLSSG Input Example: Motion Vectors")
Hudless | - Should contain the full viewable scene, **without any HUD/UI elements in it**. If some HUD/UI elements are unavoidably included, expect image quality degradation on those elements <br> - Same color space and post-processing effects (e.g., tonemapping, blur, etc.) as the color back buffer <br> - When appropriate buffer extents are *not* provided, must have the same dimensions as the color back buffer <br> | ![dlssg_hudless](./media/dlssg_docs_hudless.png "DLSSG Input Example: Hudless")
UI Alpha OR UI Color and Alpha | - `UI Alpha` is a single channel containing only the alpha values (0.0 to 1.0) of the UI <br> - `UI Color and Alpha` also contains the RGB color of the UI <br> - Prefer `UI Alpha` (single channel) for performance when available. If both are tagged, only `UI Alpha` will be used. <br> - Must be 0.0 for pixels with no UI elements <br> - Alpha must be non-zero for pixels with UI <br> - Values provided must respect the standard blending formula: `Final_Color.RGB = UI.RGB + (1 - UI.Alpha) x Hudless.RGB` <br> - When UI color is provided, the RGB channels must be pre-multiplied by alpha. <br> - When appropriate buffer extents are *not* provided, needs to have the same dimensions as the color backbuffer | **Alpha Channel** ![dlssg_ui_alpha](./media/dlssg_docs_ui_alpha.png "DLSSG Input Example: UI Alpha")<br><br>**RGB Channels** (UI Color and Alpha only) ![dlssg_ui_color_and_alpha](./media/dlssg_docs_ui_color_and_alpha.png "DLSSG Input Example: UI Color and Alpha")
Bidirectional Distortion Field | - Optional buffer, **only needed when strong distortion effects are applied as post-processing filters** <br> - Refer to [Section 5.4](#54-bidirectional-distortion-field-buffer-generation-code-sample) for an example of how to generate this optional buffer <br> - When this buffer is tagged, Mvec and Depth must be **undistorted** <br> - When this buffer is tagged, FinalColor should be **distorted** <br> - When this buffer is tagged, Hudless and UIColorAndAlpha must satisfy `Blend(Hudless, UIColorAndAlpha) = FinalColor`. This may require equally distorting Hudless and, in rare cases, UIColorAndAlpha as well <br> - **Resolution**: we recommend using half of FinalColor's width and height <br> - **Channel count**: 4 channels <br> - **RG channels**: UV coordinates of the corresponding **undistorted** pixel, as an offset relative to the source UV coordinate <br> - **BA channels**: UV coordinates of the corresponding **distorted** pixel, as an offset relative to the source UV coordinate <br> - **Units**: buffer values should be in normalized pixel space `[0,1]`. These should use the same scale as the input MVecs <br> - **Channel precision and format:** Signed format, equal bit count per channel (i.e., R10G10B10A2 is NOT allowed). We recommend a minimum of 8 bits per channel, with precision scale and bias (`PrecisionInfo`) passed in as part of the `ResourceTag` | <center>**Barrel distortion, RGB channels**  ![dlssg_bidirectional_distortion_field](./media/dlssg_docs_bidirectional_distortion_field.png "DLSSG Input Example: Bidirectional Distortion Field") <br><br> <center>**Barrel distortion, absolute value of RG channels** ![dlssg_docs_bidirectional_distortion_field_rg_abs](./media/dlssg_docs_bidirectional_distortion_field_rg_abs.png "DLSSG Input Example: Bidirectional Distortion Field, RG channels, Absolute value")

#### 5.2 TAGGING RECOMMENDATIONS

**For all buffers**: tagged buffers are used during the `Swapchain::Present` call. **If tagged buffers will be reused, destroyed, or changed in any way before the frame is presented, their lifecycle must be specified correctly**.

It is important to emphasize that **overuse of `sl::ResourceLifecycle::eOnlyValidNow` and `sl::ResourceLifecycle::eValidUntilEvaluate` can waste VRAM**. Therefore, please do the following:

* First tag all DLSS-G inputs as `sl::ResourceLifecycle::eValidUntilPresent`, then test to confirm DLSS-G is working correctly.
* Only if one or more inputs (depth, mvec, hud-less, UI, etc.) has incorrect content at present time should you flag them as `sl::ResourceLifecycle::eOnlyValidNow` or `sl::ResourceLifecycle::eValidUntilEvaluate`, as appropriate.

To run DLSS-G on a final color subrect:
* Tag the backbuffer to pass in subrect data.
* Only the buffer type `kBufferTypeBackbuffer` and backbuffer extent data are required when setting the tag; the remaining `sl::ResourceTag` fields are optional. This means passing a NULL backbuffer resource pointer is valid because SL already knows which backbuffer is being presented.
* If a valid backbuffer resource pointer is passed when tagging:
  * SL will hold a reference to it until a null tag is set.
  * SL will warn if it doesn't match the SL-provided backbuffer resource being presented.

> NOTE:
> SL holds a reference to all `sl::ResourceLifecycle::eValidUntilPresent` resources until a null tag is set. Therefore, the application will not crash if the host releases a tagged resource before the present event. This does not apply to Vulkan.

```cpp

// IMPORTANT: 
//
// Resource state for the immutable resources needs to be correct when tagged resource is used by SL - during the Present call
// Resource state for the volatile resources needs to be correct for the command list used to tag the resource - SL will make a copy which is later on used by DLSS-G during the Present call
// 
// GPU payload that generates content for any volatile resource MUST be either already submitted to the provided command list or some other command list which is guaranteed to be executed BEFORE.

// Prepare resources (assuming d3d12 integration so leaving Vulkan view and device memory as null pointers)
//
// NOTE: As an example we are tagging depth as immutable and mvec as volatile, this needs to be adjusted based on how your engine works
sl::Resource depth = {sl::ResourceType::Tex2d, myDepthBuffer, nullptr, nullptr, depthState, nullptr};
sl::Resource mvec = {sl::ResourceType::Tex2d, myMotionVectorsBuffer, nullptr, mvecState, nullptr, nullptr};
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent }; // valid all the time
sl::ResourceTag mvecTag = sl::ResourceTag {&mvec, sl::kBufferTypeMvec, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent };     // reused for something else later on

// Normally depth and mvec are available at a similar point in the pipeline so tagging them together
// If this is not the case simply tag them separately when they are available
sl::Resource inputs[] = {depthTag, mvecTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);

// Tag backbuffer only to pass in backbuffer subrect info
sl::Extent backBufferSubrectInfo {128, 128, 512, 512}; // backbuffer subrect info to run FG on.
sl::ResourceTag backbufferTag = sl::ResourceTag {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &backBufferSubrectInfo };
sl::Resource inputs[] = {backbufferTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);

// After post-processing pass but before UI/HUD is added tag the hud-less buffer
//
sl::Resource hudLess = {sl::ResourceType::Tex2d, myHUDLessBuffer, nullptr, nullptr, hudlessState, nullptr};
sl::ResourceTag hudLessTag = sl::ResourceTag {&hudLess, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent }; // valid all the time
sl::Resource inputs[] = {hudLessTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);

// UI buffer: color+alpha or alpha-only
// Prefer alpha-only for performance when available.
//
// Option A: Provide combined color+alpha
sl::Resource uiColorAlpha = {sl::ResourceType::Tex2d, myUIBuffer, nullptr, nullptr, uiTextureState, nullptr};
sl::ResourceTag uiColorAlphaTag = sl::ResourceTag {&uiColorAlpha, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };
// Option B: Provide alpha-only
sl::Resource uiAlpha = {sl::ResourceType::Tex2d, myUIAlphaBuffer, nullptr, nullptr, uiAlphaState, nullptr};
sl::ResourceTag uiAlphaTag = sl::ResourceTag {&uiAlpha, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };
// Tag whichever you have; if both are tagged, DLSS-FG today prefers kBufferTypeUIAlpha.
sl::Resource inputs[] = { uiAlphaTag /* or uiColorAlphaTag */ };
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);

// OPTIONAL! Only need the Bidirectional distortion field when strong distortion effects are applied during post-processing
//
sl::Resource bidirectionalDistortionField = {sl::ResourceType::Tex2d, myBidirectionalDistortionBuffer, nullptr, nullptr, bidirectionalDistortionState};
sl::ResourceTag bidirectionalDistortionTag = sl::ResourceTag {&bidirectionalDistortionField, sl::kBufferTypeBidirectionalDistortionField, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent }; // valid all the time
sl::Resource inputs[] = {bidirectionalDistortionTag};
slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList);
```

> **NOTE:**
> If dynamic resolution is used, specify the extent for each tagged resource. SL **manages resource states, so there is no need to transition tagged resources**.

> **IMPORTANT:**
> If the validity of tagged resources cannot be guaranteed (for example, while loading, paused, in menus, or playing a video cutscene), **set all tags to null pointers to avoid stability or image quality issues**.

#### 5.3 MULTIPLE VIEWPORTS

DLSS-G supports multiple viewports. Resources for each viewport must be tagged independently. Our [SL Sample](https://github.com/NVIDIA-RTX/Streamline_Sample) supports multiple viewports; check the sample for recommended best practices. Resource tags for different viewports are independent of one another. For example, if you have two viewports, you must make two `slSetTagForFrame()` calls. Input resources may differ between viewports, but all viewports write to the same back buffer.

Note that DLSS-G does not currently support multiple swap chains. All viewports must write to the same back buffer.

#### 5.4 BIDIRECTIONAL DISTORTION FIELD BUFFER GENERATION CODE SAMPLE

The following HLSL code snippet demonstrates generation of the bidirectional distortion field buffer. The example distortion illustrated is barrel distortion.

```cpp
const float distortionAlpha = -0.5f;
 
float2 barrelDistortion(float2 UV)
{    
    // Barrel distortion assumes UVs relative to center (0,0), so we transform
    // to [-1, 1]
    float2 UV11 = (UV * 2.0f) - 1.0f;
     
    // Squared norm of distorted distance to center
    float r2 = UV11.x * UV11.x + UV11.y * UV11.y;
     
    // Reference: http://www.cs.ait.ac.th/~mdailey/papers/Bukhari-RadialDistortion.pdf
    float x = UV11.x / (1.0f + distortionAlpha * r2);
    float y = UV11.y / (1.0f + distortionAlpha * r2);
     
    // Transform back to [0, 1]     
    float2 outUV = float2(x, y);
    return (outUV + 1.0f) / 2.0f;
}
 
float2 inverseBarrelDistortion(float2 UV)
{  
    // Barrel distortion assumes UVs relative to center (0,0), so we transform
    // to [-1, 1]
    float2 UV11 = (UV * 2.0f) - 1.0f;
     
    // Squared norm of undistorted distance to center
    float ru2 = UV11.x * UV11.x +  UV11.y * UV11.y;
 
    // Solve for distorted distance to center, using quadratic formula
    float num = sqrt(1.0f - 4.0f * distortionAlpha * ru2) - 1.0f;
    float denom = 2.0f * distortionAlpha * sqrt(ru2);
    float rd = -num / denom;
     
    // Reference: http://www.cs.ait.ac.th/~mdailey/papers/Bukhari-RadialDistortion.pdf
    float x = UV11.x * (rd / sqrt(ru2));
    float y = UV11.y * (rd / sqrt(ru2));
     
    // Transform back to [0, 1]     
    float2 outUV = float2(x, y);
    return (outUV + 1.0f) / 2.0f;
}

float2 generateBidirectionalDistortionField(Texture2D output, float2 UV)
{
    // Assume UV is in [0, 1]
    float2 rg = barrelDistortion(UV) - UV;
    float2 ba = inverseBarrelDistortion(UV) - UV;
 
    // rg and ba must use the same canonical format as the motion vectors
    // i.e., a displacement of rg or ba must be on the same scale as (Mvec.x, Mvec.y)
     
    // The output can be outside of the [0, 1] range
    Texture2D[UV] = float4(rg, ba); // needs to be signed
}
```

This HLSL code snippet uses an iterative Newton-Raphson method to solve the inverse distortion problem. It is designed to be used directly in shader code, especially when an analytical solution is not available. While the method is effective, it does not guarantee convergence for all distortion functions, so users should verify its suitability for their specific use case.

```cpp
float2 myDistortion(float2 xy)
{
     // The distortion function
}

float loss(float2 Pxy, float2 ab)
{
    float2 Pab = myDistortion(ab);
    float2 delta = Pxy - Pab;
    return dot(delta, delta);
}

float2 iterativeInverseDistortion(float2 UV)
{
    const float kTolerance = 1e-6f;
    const float kGradDelta = 1e-6f;      // The delta used for gradient estimation
    const int kMaxIterations = 5;        // Select a low number of iterations which minimizes the loss
    const int kImprovedInitialGuess = 1; // Assume a locally uniform distortion field 
    
    float2 ab = UV; // initial guess
    
    if (kImprovedInitialGuess)
    {
        ab = UV - (myDistortion(UV) - UV);
    }

    for (int i = 0; i < kMaxIterations; ++i)
    {
        float F = loss(UV, ab);

        // Central difference
        const float Fabx1 = loss(UV, ab + float2(kGradDelta * 0.5f, 0));
        const float Fabx0 = loss(UV, ab - float2(kGradDelta * 0.5f, 0));
        const float Faby1 = loss(UV, ab + float2(0, kGradDelta * 0.5f));
        const float Faby0 = loss(UV, ab - float2(0, kGradDelta * 0.5f));

        float2 grad;
        grad.x = (Fabx1 - Fabx0) / kGradDelta;
        grad.y = (Faby1 - Faby0) / kGradDelta;

        const float norm = grad.x * grad.x + grad.y * grad.y;
        if (abs(norm) < kTolerance) {
            break;
        }

        float delta_x = F * grad.x / norm;
        float delta_y = F * grad.y / norm;

        ab.x = ab.x - delta_x;
        ab.y = ab.y - delta_y;
    }

    return ab;
}
```

### 6.0 SET DLSS-G OPTIONS

The `slDLSSGSetOptions()` function is used to configure the DLSS-G plugin for a
specific viewport. It allows the application to enable or disable frame
generation and set flags and other settings that control frame generation
behavior.

The `slDLSSGSetOptions()` function takes effect in the next `Present()` call
that executes after it.

If `slDLSSGSetOptions()` is called from a thread other than the presenting
thread, Streamline cannot guarantee which `Present()` call will pick up the
updated options. To ensure deterministic behavior, the application should either:
1. Call `slDLSSGSetOptions()` on the presenting thread
2. Use their own synchronization to ensure the intended ordering between
   `slDLSSGSetOptions()` and `Present()`.

#### 6.1 ENABLING/DISABLING FRAME GENERATION

To enable frame generation, use the `mode` property in `DLSSGOptions`. Allowed
values are:
- `DLSSGMode::eOff`: Frame Generation disabled
- `DLSSGMode::eOn`: Frame Generation enabled with a fixed multiplier
- `DLSSGMode::eDynamic`: Dynamic Multi Frame Generation
- `DLSSGMode::eAuto` (legacy): Auto mode

```cpp
sl::DLSSGOptions options{};
// These are populated based on the user's selection in the UI
options.mode = myUI->getDLSSGMode(); // e.g. sl::DLSSGMode::eOn;

// IMPORTANT: Make sure this is the same as the viewport used for tagging
// resources
if (SL_FAILED(result, slDLSSGSetOptions(viewport, options)))
{
    // Handle error here, check the logs
}
```

**NOTE: Loading the DLSS-G plugin and tagging resources does not automatically
enable interpolation. `slDLSSGSetOptions` must be used to explicitly activate
the feature.**

#### 6.2 ENABLING MULTI FRAME GENERATION

When `mode` is set to `DLSSGMode::eOn`, the `numFramesToGenerate` property
determines the number of interpolated frames produced for every one rendered
frame provided by the application.

For example, setting `numFramesToGenerate` to 3 results in a 4x frame
multiplier: Streamline will present four frames (three generated frames plus
the original application frame) for each `Present()` call.

Multi frame support is dependent on the hardware and system configuration, and
must be verified before use. To check for support, call `slDLSSGGetState`. The
value of `numFramesToGenerate` must be between 1 (single frame generation) and
the maximum defined by `DLSSGState::numFramesToGenerateMax`.

#### 6.3 ENABLING DYNAMIC MULTI FRAME GENERATION

Setting `mode` to `DLSSGMode::eDynamic` enables Dynamic Multi Frame Generation.
In this mode, Streamline automatically adjusts the number of generated frames to
align the output frame rate with the display's refresh rate or a user-defined
target.

Dynamic multi frame support is dependent on the hardware and system
configuration. Applications must verify support by calling `slDLSSGGetState()` and
checking if `bIsDynamicMFGSupported` is `eTrue`.

The most likely reasons for lack of support are:
- Multi-frame generation is not supported
- The NVIDIA display driver is below version 595.41
- The application is using Vulkan (support is currently limited to D3D12 only)

When `eDynamic` is active, the `numFramesToGenerate` property is ignored.

The target frame rate is managed via the `dynamicTargetFrameRate` property:
- **Custom Target:** Set to a specific FPS value (e.g., 120.0f).
- **Auto-Detect:** Set to 0.0f to automatically target the monitor's current
  refresh rate. When multiple monitors are in use, the placement of the
  application window determines which monitor is used.

When VSync is enabled, the `dynamicTargetFrameRate` property is ignored, and the
optimal refresh rate to achieve tearing-free presentation (near the monitor's
refresh rate) is used instead.

Dynamic MFG is designed to work with the Reflex frame rate limiter, which can be
enabled by setting `frameLimitUs` in `ReflexOptions`. Other frame rate limiting
techniques may not work or may produce unexpected results when combined with
Dynamic MFG.

**Auto Mode as a Fallback**

If Dynamic MFG is unsupported, the application may instead wish to expose an
option to use Auto mode (`DLSSGMode::eAuto`).

In Auto mode, DLSSG uses the fixed multiplier defined in `numFramesToGenerate`,
but will also monitor performance and automatically disable interpolation if it
detects that the application would perform better (higher FPS) with the feature
disabled.

#### 6.4 DISABLING FRAME GENERATION

When interpolation is not needed, the feature should be disabled. DLSSG should
be disabled in any full-screen menus (such as settings menus or pause menus) or
when a UI element is overlaid over the majority of the screen, such as a
leaderboard.

To disable DLSS-G, set `mode` to `DLSSGMode::eOff`. By default, this releases
all internal resources allocated by the feature. When the feature is enabled
again, reallocating these resources may cause a stutter.

To ensure seamless transitions, the `DLSSGFlags::eRetainResourcesWhenOff` flag
is strongly recommended. When this flag is set, `DLSSGMode::eOff` instead
suspends frame generation without freeing memory. This avoids the stutter when
the feature is re-enabled.

When using this flag, you must manage the lifecycle of DLSS-G memory during
long-term deactivations (such as the user turning the feature off in a settings
menu):
1. Set `mode` to `DLSSGMode::eOff`
2. Call `slFreeResources()` to explicitly deallocate the feature's memory.

For short-term deactivations, such as in pause menus, avoid calling
`slFreeResources()` to prevent stutter when leaving the menu.

#### 6.5 AUTOMATICALLY DISABLING DLSS-G IN MENUS

If `kBufferTypeUIColorAndAlpha` is provided, DLSS-G can automatically detect fullscreen menus and turn itself off. To enable automatic fullscreen menu detection, set the `sl::DLSSGFlags::eEnableFullscreenMenuDetection` flag.
This flag may be changed on a per-frame basis to disable detection for specific scenes, for example.

Since this approach may not detect menus in all cases, it is still preferable to disable DLSS-G manually by setting the mode to `sl::DLSSGMode::eOff`.

**Note:** when DLSS-G is disabled by fullscreen menu detection, its resources
will _always_ be retained, regardless of the value of the
`sl::DLSSGFlags::eRetainResourcesWhenOff` flag

#### 6.6 ENABLING USER INTERFACE RECOMPOSITION

When both Hudless and a UI buffer are tagged, User Interface Recomposition can
be enabled by setting `DLSSGOptions::enableUserInterfaceRecomposition = eTrue`.

When enabled, the HUD and scene are interpolated separately and composited later, providing significantly improved UI interpolation quality.

Using user interface recomposition has a slight performance and memory cost.

#### 6.7 HOW TO SETUP A CALLBACK TO RECEIVE API ERRORS (OPTIONAL)

DLSS-G intercepts `IDXGISwapChain::Present` and, when using Vulkan, `vkQueuePresentKHR` and `vkAcquireNextImageKHR` calls, executing them asynchronously. When calling these methods from the host side, SL returns the "last known error." To obtain per-call API errors, you must provide an API error callback. Here is how to set one up:

```cpp

// Triggered immediately upon return from the API call but ONLY if return code != 0
void myAPIErrorCallback(const sl::APIError& e)
{
    // Handle error, use e.hres with DirectX and e.vkRes on Vulkan
    
    // IMPORTANT: STORE ERROR AND RETURN IMMEDIATELY TO AVOID STALLING PRESENT THREAD
};

sl::DLSSGOptions options{};
// Constants are populated based on user selection in the UI
options.mode = myUI->getDLSSGMode(); // e.g. sl::eDLSSGModeOn;
options.onErrorCallback = myAPIErrorCallback;
if(SL_FAILED(result, slDLSSGSetOptions(viewport, options)))
{
    // Handle error here, check the logs
}
```

> **NOTE:**
> API error callbacks are triggered from the Present thread and **must not be blocked** for a prolonged period of time.

> **IMPORTANT:**
> THIS IS OPTIONAL AND ONLY NEEDED IF YOU ARE ENCOUNTERING ISSUES AND NEED TO PROCESS SPECIFIC ERRORS RETURNED BY THE VULKAN OR DXGI API

### 7.0 PROVIDE COMMON CONSTANTS

Various per-frame camera-related constants are required by all Streamline features and must be provided ***if any SL feature is active, as early in the frame as possible***. Keep in mind the following:

* All SL matrices are row-major and should not contain any jitter offsets
* If motion vector values in your buffer are in {-1,1} range then motion vector scale factor in common constants should be {1,1}
* If motion vector values in your buffer are NOT in {-1,1} range then motion vector scale factor in common constants must be adjusted so that values end up in {-1,1} range

```cpp
sl::Constants consts = {};
// Set motion vector scaling based on your setup
consts.mvecScale = {1,1}; // Values in eMotionVectors are in [-1,1] range
consts.mvecScale = {1.0f / renderWidth,1.0f / renderHeight}; // Values in eMotionVectors are in pixel space
consts.mvecScale = myCustomScaling; // Custom scaling to ensure values end up in [-1,1] range
sl::Constants consts = {};
// Set all constants here
//
// Constants are changing per frame tracking handle must be provided
if(!setConstants(consts, *frameToken, viewport))
{
    // Handle error, check logs
}
```

For more details, see [common constants](ProgrammingGuide.md#2111-common-constants).

### 8.0 INTEGRATE SL REFLEX

**It is required** for sl.reflex to be integrated in the host application. **Any existing Reflex SDK integration that does not use Streamline cannot be used with DLSS-G.** Pay special attention to the `eReflexMarkerPresentStart` and `eReflexMarkerPresentEnd` markers, which must provide the correct frame index so it can be matched to the one provided in [Section 7](#70-provide-common-constants).

For more details, see the [Reflex guide](ProgrammingGuideReflex.md).

> **IMPORTANT:**
> If you see a warning in the SL log stating that `common constants cannot be found for frame N`, this indicates that the sl.reflex markers `eReflexMarkerPresentStart` and `eReflexMarkerPresentEnd` are out of sync with the frame being presented.

### 9.0 DLSS-G DEVELOPMENT HOTKEYS

When using non-production (development) builds of `sl.dlss_g.dll`, there are numerous hotkeys available, all of which can be remapped using the remapping methods described in [debugging](<Debugging - JSON Configs (Plugin Configs).md>)

* `"dlssg-sync"` (default `VK_END`)
  * Toggle delaying the presentation of the next frame to experiment with minimizing latency
* `"vsync"` (default `Shift-Ctrl-'1'`)
  * Toggle vsync on output swapchain
* `"debug"` (default `Shift-Ctrl-VK_INSERT`)
  * Toggle debugging view
* `"stats"` (default `Shift-Ctrl-VK_HOME`)
  * Toggle performance stats
* `"dlssg-toggle"` (default `VK_OEM_2` `/?` for US)
  * Toggle DLSS-G on/off/auto (override app setting)
* `"write-stats"` (default `Ctrl-Alt-'O'`)
  * Write performance stats to file

### 10.0 DLSS-G AND DYNAMIC RESOLUTION

DLSS-G supports dynamic resolution of the MVec and Depth buffer extents. Dynamic resolution may be implemented via DLSS or an app-specific method. Since DLSS-G uses the final color buffer with all post-processing complete, the color buffer (or its subrect, if in use) must remain a fixed size and cannot resize per frame. When DLSS-G dynamic resolution mode is enabled, the application can pass differently sized extents for the MVec and Depth buffers on a per-frame basis, allowing the application to change its rendering load smoothly.

There are a few requirements when using dynamic resolution with DLSS-G:

* The application must set the flag `sl::DLSSGFlags::eDynamicResolutionEnabled` in `sl::DLSSGOptions::flags` when dynamic resolution is active. Clear the flag when dynamic resolution is disabled. *Do not* leave the dynamic resolution flag set when using fixed-ratio DLSS, as it may decrease performance or image quality.
* The application should set `sl::DLSSGOptions::dynamicResWidth` and `sl::DLSSGOptions::dynamicResHeight` to a target resolution within the range of the dynamic MVec and Depth buffer sizes.
  * This is the fixed resolution at which DLSS-G processes the MVec and Depth buffers.
  * This value must not change dynamically per frame. Changing it outside of the application UI can cause a frame rate glitch.
  * Set it to a reasonable middle-range value and do not change it unless DLSS or other dynamic resolution settings change.  
  * For example, if the application has a final, upscaled color resolution of 3840x2160 pixels, with a rendering resolution that can vary between 1920x1080 and 3840x2160 pixels, the `dynamicResWidth` and `Height` could be set to 2880x1620 or 1920x1080.
  * This ratio between the min and max resolutions can be tuned for performance and quality.
  * If the application passes 0 for these values when DLSS-G dynamic resolution is enabled, then DLSS-G will default to half of the resolution of the final color target or its subrect, if in use.

```cpp

// Using helpers from sl_dlss_g.h

sl::DLSSGOptions options{};
// These are populated based on user selection in the UI
options.mode = myUI->getDLSSGMode(); // e.g. sl::eDLSSGModeOn;
options.flags = sl::DLSSGFlags::eDynamicResolutionEnabled;
options.dynamicResWidth = appSelectedInternalWidth;
options.dynamicResHeight = appSelectedInternalHeight;
if(SL_FAILED(result, slDLSSGSetOptions(viewport, options)))
{
    // Handle error here, check the logs
}
```

Additionally, in development (i.e. non-production) builds of sl.dlss_g.dll, it is possible to enable DLSS-G dynamic res mode globally for debugging purposes via sl.dlss_g.json.  The supported options are:

* `"forceDynamicRes": true,` force-enables DLSS-G dynamic mode, equivalent to passing the flag `eDynamicResolutionEnabled` to `slDLSSGSetOptions` on every frame.
* `"forceDynamicResScaling": 0.5` sets the desired `dynamicResWidth` and `dynamicResHeight` indirectly, as a fraction of the color output buffer size.  In the case shown, the fraction is 0.5, so with a color buffer that is 3840x2160, the internal resolution used by DLSS-G for dynamic resolution MVec and Depth buffers will be 1920x1080.  If this value is not set, it defaults to 0.5.

### 11.0 DLSS-G AND HDR

If your game supports HDR, use **UINT10/RGB10 pixel format and HDR10/BT.2100 color space**. For more details, see <https://docs.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range#option-2-use-uint10rgb10-pixel-format-and-hdr10bt2100-color-space>.

When tagging `eUIColorAndAlpha`, ensure the alpha channel has enough precision (for example, do not use formats like R10G10B10A2).

> **IMPORTANT:**
> DLSS-G currently does NOT support FP16 pixel format and scRGB color space because it is too expensive in terms of compute and bandwidth cost.

### 12.0 DLSS-G AND DXGI

DLSS-G takes over frame presentation, so the host application must turn DLSS-G on and off as needed to avoid potential problems and deadlocks.
As a general rule, **when the host is changing resolution, switching between full-screen and windowed mode, or performing any other operation that could cause `SwapChain::Present` to deadlock, DLSS-G must be turned off using the `sl::DLSSGOptions::mode` field.** When DLSS-G is off, it calls `SwapChain::Present` on the same thread as the host application; this is not the case when DLSS-G is on. For more details, see <https://docs.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices#multithreading-and-dxgi>.

> **IMPORTANT:**
> Turning DLSS-G on and off via `sl::DLSSGOptions::mode` should not be confused with enabling or disabling the DLSS-G feature using `slSetFeatureLoaded`. The latter completely unloads and unhooks the sl.dlss_g plugin, disabling `sl::kFeatureDLSS_G` entirely (it cannot be turned on/off or used in any way).

#### 12.1 FRAME-LATENCY WAITABLE OBJECTS

When the sl.dlss_g plugin is loaded, the host application receives a proxy swap chain while SL creates and owns the actual swap chain. That internal swap chain is always created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. DXGI provides a single frame-latency waitable object (a single pool of signals) per swap chain, so only one side - the application or SL - can safely wait on it. Which side owns it depends on whether the application requested `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` when creating its swap chain. Both behaviors below apply whenever the plugin is loaded, **even while DLSS-G is turned off** with `DLSSGMode::eOff`.

**If the application does not request the flag (recommended),** the SL pacer owns the waitable object: SL sets the maximum frame latency and consumes the waitable's signals for its own frame pacing. The application **must not call `IDXGISwapChain2::GetFrameLatencyWaitableObject` or `IDXGISwapChain2::SetMaximumFrameLatency`** on such a swap chain. These calls forward to the internal swap chain: the returned handle refers to the same waitable object the SL pacer is waiting on, so the application and SL compete for signals—application waits can block unpredictably and SL frame pacing breaks. Note that `GetFrameLatencyWaitableObject` returns a valid handle in this case too, so a non-null handle does not mean the waitable is safe to use.

**If the application requests the flag,** the application owns the waitable object and SL stays off of it: SL never waits on it and never limits the frame latency through it, so `GetFrameLatencyWaitableObject` and `SetMaximumFrameLatency` behave as they do without SL. The one exception is at swap-chain creation, where SL performs a single `SetMaximumFrameLatency` call raising the limit to the internal back-buffer count so that frame generation can queue its presents without CPU stalls; the application's own `SetMaximumFrameLatency` call overwrites it - last writer wins. In exchange, the application is expected to manage frame latency itself: SL applies no latency limit of its own, so an application that leaves the limit at the creation-time value can queue presents up to the internal back-buffer count deep. Applications that pace on the waitable object and set their own frame-latency limit get standard DXGI behavior.

Applications that use the waitable object to pace their frame start (as recommended by Microsoft for regular DXGI swap chains) may keep doing so by requesting the flag as described above, but the preferred approach is to use `slReflexSleep` instead - it blocks until the optimal frame-start time computed by the driver from measured render and queue timing, and it is available in any DLSS-G integration since [SL Reflex is required](#80-integrate-sl-reflex). To pace to a fixed frame rate rather than run at the lowest latency, use `sl::ReflexOptions::frameLimitUs`. On configurations where the sl.dlss_g plugin is not loaded the waitable object behaves natively and can be used as usual.

### 13.0 HOW TO OBTAIN THE ACTUAL FRAME TIMES AND NUMBER OF FRAMES PRESENTED

When DLSS-G is enabled, it presents additional frames. The actual frame time can be obtained using the following sample code:

```cpp

// Using helpers from sl_dlss_g.h

// Not passing flags or special options here, no need since we just want the frame stats
sl::DLSSGState state{};
if(SL_FAILED(result, slDLSSGGetState(viewport, state)))
{
    // Handle error here, check the logs
}
```

> **IMPORTANT:**
> When querying only frame times or status, do not specify the `DLSSGFlags::eRequestVRAMEstimate`; setting that flag and passing a non-null `sl::DLSSGOptions` will cause DLSS-G to compute and return the estimated VRAM required.  This is needless and too expensive to do per frame.

Once DLSS-G state has been obtained, the actual FPS can be estimated like this:

```cpp
//! IMPORTANT: Returned value represents number of frames presented since 
//! we last called slDLSSGGetState so make sure to account for that.
//!
//! If calling 'slDLSSGGetState' after each present then the actual FPS
//! can be computed like this:
auto actualFPS = myFPS * state.numFramesActuallyPresented;
```

`numFramesActuallyPresented` equals the number of frames presented per application frame. For example, if the DLSS-G plugin inserts one generated frame after each application frame, that variable will contain `2`.

> **IMPORTANT:**

DLSS-G will **always present the real frame generated by the host, but the interpolated frame can be dropped** if presents go out of sync (for example, when the interpolated frame is too close to the last real one). In addition, if the host is CPU-bound, **reported FPS can exceed 2× when DLSS-G is on** because the call to `Swapchain::Present` is no longer blocking for the host and can be up to 1 ms faster, which translates to faster base frame times. Here is an example:

* Host is CPU bound and producing frames every 10ms
* Up to 1ms is spent blocked by the `Swapchain::Present` call
* SL present hook will take around 0.2ms instead since `Swapchain::Present` is now an async event handled by the SL pacer
* Host is now delivering frames at 10 ms − 0.8 ms = 9.2 ms
* This results in 109 FPS increasing to 218 FPS when DLSS-G is active; 2.18× scaling instead of the expected 2×

#### 13.1 UNDERSTANDING FRAME PACING BEHAVIOR

**Frame Presentation Timing:**
* DLSS-G uses an asynchronous presentation mechanism called the "SL pacer" to manage frame delivery
* The pacer intelligently schedules both real (application-rendered) and interpolated (AI-generated) frames
* Presentation timing is optimized to maintain smooth visual experience while maximizing perceived frame rate

**The tool for measuring frame timing:**

NVIDIA [FrameView](https://www.nvidia.com/en-us/geforce/technologies/frameview/) provides two key metrics for measuring frame presentation:

* `MsBetweenDisplayChange`: Indicates when the image has been shown to the end user
* `MsBetweenPresents`: Indicates when the internal Present() call has happened in DLSS-G

**Why MsBetweenPresents Is Insufficient:**

`MsBetweenPresents` is not suitable for measuring frame pacing quality because DLSS-G uses specialized hardware to delay the image after Present() has been called. This delay ensures the image is shown to the end user at precisely the right time, but `MsBetweenPresents` does not account for this delay.

NVIDIA [FrameView](https://www.nvidia.com/en-us/geforce/technologies/frameview/) (as of version 16.1) uses `MsBetweenDisplayChange` and is the recommended tool for measuring frame pacing quality. Third-party tools may not account for the hardware-level presentation delay used by DLSS-G, making FrameView the most accurate option for evaluating DLSS-G frame pacing performance.

### 14.0 HOW TO CHECK DLSS-G STATUS AT RUNTIME

#### 14.1 HOW TO CHECK FOR MULTI FRAME SUPPORT

Multi frame support is reported via `sl::DLSSGState::numFramesToGenerateMax`.
Before enabling multi frame, check for device support by calling
`slDLSSGGetState` and checking `numFramesToGenerateMax`. If the value is 1,
multi frame is not supported. Otherwise, multi frame is supported, up to the
number of frames specified.

#### 14.2 HOW TO CHECK FOR DYNAMIC MULTI FRAME SUPPORT

Dynamic multi frame support is reported via
`sl::DLSSGState::bIsDynamicMFGSupported`. Before setting `DLSSGMode::eDynamic`,
check for system support by calling `slDLSSGGetState` and checking this
property. If it is not `eTrue`, `eDynamic` is not supported, and attempting to
enable it will result in an error.

#### 14.3 HOW TO CHECK FOR RUNTIME ERRORS

Even if the DLSS-G feature is supported and loaded, it can still enter an invalid runtime state for various reasons. The following code snippet shows how to check runtime status:

```cpp
sl::DLSSGState state{};
if(SL_FAILED(result, slDLSSGGetState(viewport, state)))
{
    // Handle error here, check the logs
}
// Run-time status
if(state.status != sl::eDLSSGStatusOk)
{
    // Turn off DLSS-G

    sl::DLSSGOptions options{};    
    options.mode = sl::DLSSGMode::eOff;
    slDLSSGSetOptions(viewport, options);
    // Check status and errors in the log and fix your integration if applicable
}
```

For more details, see `enum DLSSGStatus` in sl_dlss_g.h.

> **IMPORTANT:**
> When DLSS-G is in an invalid state and turned on, it adds a pink overlay to the final color image. A warning message is shown on screen in NDA development builds, and an error is logged describing the issue.

> **IMPORTANT:**
> When querying only frame times or status, do not specify the `DLSSGFlags::eRequestVRAMEstimate`; setting that flag and passing a non-null `sl::DLSSGOptions::ext` will cause DLSS-G to compute and return the estimated VRAM required.  This is needless and too expensive to do per frame.

### 15.0 HOW TO GET AN ESTIMATE OF VRAM REQUIRED BY DLSS-G

SL can return a general estimate of the GPU memory required by DLSS-G via `slDLSSGGetState`.  This can be queried before DLSS-G is enabled, and can be queried for resolutions and formats other than those currently active.  To receive an estimate of GPU memory required, the application must:

* Set the `sl::DLSSGOptions::flags` flag, `DLSSGFlags::eRequestVRAMEstimate`
* Provide values in the `sl::DLSSGOptions` structure, including the intended resolutions of the MVecs, Depth buffer, and final color buffer (UI buffers are assumed to match the color buffer size), as well as the 3D API-specific format enums for each buffer. Finally, specify the expected number of back buffers in the swap chain. See the `sl::DLSSGOptions` struct for details.

If the flag and structure are provided, `slDLSSGGetState` should return a nonzero value in `sl::DLSSGState::estimatedVRAMUsageInBytes`.  Note that this value is a very rough estimate/guideline and should be used for general allocation.  The actual amount used may differ from this value.

> **IMPORTANT:**
> When querying only frame times or status, do not specify the `DLSSGFlags::eRequestVRAMEstimate`; setting that flag and passing a non-null `sl::DLSSGOptions` will cause DLSS-G to compute and return the estimated VRAM required.  This is needless and too expensive to do per frame.

#### 15.1 HOW TO SYNCHRONIZE THE HOST APP DLSS-G INPUTS AND STREAMLINE IF REQUIRED

```cpp
//! SL client must wait on SL DLSS-G plugin-internal fence and associated value, before it can modify or destroy the tagged resources input
//! to DLSS-G enabled for the corresponding previously presented frame on a non-presenting queue.
//! If modified on client's presenting queue, then it's recommended but not required.
//! However, if DLSSGQueueParallelismMode::eBlockNoClientQueues is set, then it's always required for VK.
//! It must call slDLSSGGetState on the present thread to retrieve the fence value for the inputs consumed by FG, on which client would
//! wait in the frame it would modify those inputs.
void* inputsProcessingCompletionFence{};
uint64_t lastPresentInputsProcessingCompletionFenceValue{};
```

### 16.0 HOW TO SYNCHRONIZE THE HOST APP AND STREAMLINE WHEN USING VULKAN

SL DLSS-G implements the following logic when intercepting `vkQueuePresentKHR` and `vkAcquireNextImageKHR`:

* sl.dlss_g waits for the binary semaphore provided in `VkPresentInfoKHR` before proceeding with adding workload(s) to the GPU
* sl.dlss_g signals the binary semaphore provided in the `vkAcquireNextImageKHR` call when DLSS-G workloads are submitted to the GPU

Based on this, the host application MUST:

* Signal the `present` binary semaphore provided in `VkPresentInfoKHR` when submitting the final workload at the end of the frame
* Wait for the signal on the `acquire` binary semaphore provided with the `vkAcquireNextImageKHR` call before starting the new frame

Here is some pseudo-code:

```cpp
createBinarySemaphore(acquireSemaphore);
createBinarySemaphore(presentSemaphore);

// SL will signal the 'acquireSemaphore' when ready to continue next frame
vkAcquireNextImageKHR(acquireSemaphore, &index);

// Frame start
waitOnGPU(acquireSemaphore);

// Render frame using render target with given index
renderFrame(index);

// Finish frame
signalOnGPU(presentSemaphore);

// Present the frame (SL will wait for the 'presentSemaphore' on the GPU)
vkQueuePresent(presentSemaphore, index);

```

### 17.0 DLSS-G INTEGRATION CHECKLIST DETAILS

* Provide either correct application ID or engine type (Unity, UE etc.) when calling `slInit`
* In final (production) builds validate the public key for the NVIDIA custom digital certificate on `sl.interposer.dll` if using the binaries provided by NVIDIA. See [security section](ProgrammingGuide.md#211-security) for more details.
* Tag `eDepth`, `eMotionVectors`, `eHUDLessColor`, and `eUIColorAndAlpha` buffers
  * When depth and mvec values may be invalid, set all tags to null pointers (level loading, video cutscenes, paused, in menus, etc.)
  * Tagged buffers must be marked as volatile if they will not be valid when the `SwapChain::Present` call is made
* Tag the backbuffer only if DLSS-G needs to run on a subregion of the final color buffer. If tagged, set the tag to a null pointer when it may be invalid.
* Provide correct common constants and frame index using the `slSetConstants` method.
  * When the game is rendering frames, set `sl::Constants::renderingGameFrames` correctly
* Ensure the frame index provided with the common constants matches the presented frame (i.e., the frame index provided with Reflex markers `ReflexMarker::ePresentStart` and `ReflexMarker::ePresentEnd`)
* **Do not set common constants (camera matrices, etc.) multiple times in a single frame**, as this causes ambiguity that can result in image quality issues.
* Use the sl.imgui plugin to validate that inputs (camera matrices, depth, mvec, color, etc.) are correct
* Turn DLSS-G off (by setting `sl::DLSSGOptions::mode` to `DLSSGMode::eOff`) before any window manipulation (resize, maximize/minimize, full-screen transition, etc.) to avoid potential deadlocks or instability
* Reduce the amount of motion blur when DLSS-G is active
* Call `slDLSSGGetState` to obtain `sl::DLSSGState` and check the following:
  * Ensure `sl::DLSSGStatus` is set to `eDLSSGStatusOk`. If not, disable DLSS-G and fix the integration as needed (see the logs for errors).
  * If the swap chain back buffer size is lower than `sl::DLSSGSettings::minWidthOrHeight`, DLSS-G must be disabled.
  * If VRAM stats and other extra information are not needed, pass `nullptr` for constants for the lowest overhead.
* Call `slGetFeatureRequirements` to obtain requirements for DLSS-G (see [programming guide](./ProgrammingGuide.md#23-checking-features-requirements)) and check the following:
  * If any items in the `sl::FeatureRequirements` structure (OS, driver, etc.) are not supported, inform the user accordingly.
* To avoid additional overhead when presenting frames while DLSS-G is off, **always recreate the swap chain when DLSS-G is turned off**. For details, see [Section 18](#180-how-to-avoid-unnecessary-overhead-when-dlss-g-is-turned-off).
* In Vulkan, to exploit command queue parallelism, setting `DLSSGOptions::queueParallelismMode` to `DLSSGQueueParallelismMode::eBlockNoClientQueues` may offer extra performance gains depending on the workload.
  * The same `DLSSGQueueParallelismMode` must be set for all viewports.
  * When using this mode, the client should wait on `DLSSGState::inputsProcessingCompletionFence` and its associated value before modifying or destroying tagged resource inputs for the corresponding previously presented frame on any of its queues.
  * For synchronization details, see [Section 15.1](#151-how-to-synchronize-the-host-app-dlss-g-inputs-and-streamline-if-required).
  * Gains are most apparent in GPU-limited applications that use multiple queues for submissions, especially when the presenting queue is the only one accessing FG inputs. Workloads from other application queues can run in parallel with the DLSS-G workload; if those workloads underutilize GPU SM resources, the DLSS-G workload may better fill SM utilization, improving overall performance. Highly CPU-limited applications may see relatively smaller gains due to lower parallelism.

#### 17.1 Game setup for testing DLSS Frame Generation

1. Set up a machine with an Ada board and drivers recommended by the NVIDIA team.
1. Turn on Hardware GPU Scheduling: Windows Display Settings (scroll down) → Graphics Settings → Hardware-accelerated GPU Scheduling: ON. Restart your PC.
1. Verify that Vertical Sync is set to "Use the 3D application setting" in the NVIDIA Control Panel ("Manage 3D Settings").
1. Get the game build that has Streamline, DLSS-G, and Reflex integrated and install it on the machine.
1. Once the game has loaded, open the game settings and turn DLSS-G on.
1. Once DLSS-G is on, you should be able to confirm it by:
    * observing an FPS boost in any external FPS measurement tool; and
    * if the build includes Streamline and DLSS-G development libraries, seeing a debug overlay at the bottom of the screen (configurable in sl.dlss-g.json).

If the steps above fail, set up logging in sl.interposer.json, check the log for easy-to-fix issues and errors, and contact the NVIDIA team.

### 18.0 HOW TO AVOID UNNECESSARY OVERHEAD WHEN DLSS-G IS TURNED OFF

When DLSS-G is loaded, it creates an extra graphics command queue used to present frames asynchronously and forces the host application to render off-screen (the host has no direct access to swap chain buffers). When DLSS-G is switched off by the user, this results in unnecessary overhead from the extra copy from the off-screen buffer to the back buffer and from synchronization between the game's graphics queue and DLSS-G's queue. To avoid this, the swap chain must be torn down and recreated every time DLSS-G is switched on or off.

Here is some pseudocode showing how this can be done:

```cpp
void onDLSSGModeChange(sl::DLSSGMode mode)
{
    if (mode != sl::DLSSGMode::eOff)
    {
        // DLSS-G was off, now we are turning it on

        // Make sure no work is pending on GPU
        waitForIdle();
        // Destroy swap-chain back buffers
        releaseBackBuffers();
        // Release swap-chain
        releaseSwapChain();
        // Make sure DLSS-G is loaded
        slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
        // Re-create our swap-chain using the same parameters as before
        // Note that DLSS-G is loaded so SL will return a proxy (assuming host is linking SL and using SL proxy DXGI factory)
        auto swapChainProxy = createSwapChain();
        // Obtain native swap-chain if using manual hooking        
        slGetNativeInterface(swapChainProxy,&swapChainNative);    
        // Obtain new back buffers from the swap-chain proxy (rendering off-screen)
        getBackBuffers(swapChainProxy)
    }
    else // if mode == sl::DLSSGMode::eOff
    {
        // DLSS-G was on, now we are turning it off

        // Make sure no work is pending on GPU
        waitForIdle();
        // Destroy swap-chain back buffers
        releaseBackBuffers();
        // Release swap-chain
        releaseSwapChain();
        // Make sure DLSS-G is unloaded
        slSetFeatureLoaded(sl::kFeatureDLSS_G, false);
        // Re-create our swap-chain using the same parameters as before
        // Note that DLSS-G is unloaded so there is no proxy here, SL will return native swap-chain interface
        auto swapChainNative = createSwapChain();
        // Obtain new back buffers from the swap-chain (rendering directly to back buffers)
        getBackBuffers(swapChainNative)
    }    
}
```

For additional implementation details, check the Streamline sample, especially the `void DeviceManagerOverride_DX12::BeginFrame()` function.

> **NOTE:**
> When DLSS-G is turned on, the overhead from rendering to an off-screen target is negligible compared with the overall frame rate boost provided by the feature.

### 19.0 DLSS-FG INDICATOR TEXT

DLSS-FG can render on-screen indicator text when the feature is enabled. Developers may find this helpful for confirming DLSS-FG is executing.

The indicator supports all build variants, including production.

The indicator is configured via the Windows Registry and contains 3 levels: `{0, 1, 2}` for `{off, minimal, detailed}`.

**Example .reg file setting the level to detailed:**

```
[HKEY_LOCAL_MACHINE\SOFTWARE\NVIDIA Corporation\Global\NGXCore]
"DLSSG_IndicatorText"=dword:00000002
```

### 20.0 AUTO SCENE CHANGE DETECTION

Auto Scene Change Detection (ASCD) intelligently annotates the reset flag during input frame pair sequences.

ASCD is enabled in all DLSS-FG build variants, executes on every frame pair, and supports all graphics platforms.

#### 20.1 INPUT DATA

ASCD uses the camera forward, right, and up vectors passed into Streamline via `sl_consts.h`. These are stitched into a 3x3 camera rotation matrix such that:

```
[ cameraRight[0] cameraUp[0] cameraForward[0] ]
[ cameraRight[1] cameraUp[1] cameraForward[1] ]
[ cameraRight[2] cameraUp[2] cameraForward[2] ]
```

It is important that this matrix is orthonormal, i.e. the transpose of the matrix should equal the inverse. ASCD will only run if the orthonormal property is true. If the orthonormal check fails, ASCD is entirely disabled. Logs for DLSS-FG will show additional detail to debug incorrect input data.

#### 20.2 VIEWING STATUS

In all variants the detector status can be visualized with the detailed DLSS_G Indicator Text.

The mode will be
* Enabled
* Disabled
* Disabled (Invalid Input Data)

In developer builds, ASCD can be toggled with `Shift+F9`. In developer builds, an additional `ignore_reset_flag` option simulates pure dependence on ASCD (`Shift+F10`).

In cases where input camera data is incorrect, ASCD will report failure to the logs every frame. Log messages can be resolved by updating the camera inputs or disabling ASCD temporarily with the keybind.

#### 20.3 DEVELOPER HINTS

In developer DLSS-FG variants ASCD displays on-screen hints for:

1. Scene change detected without the reset flag.
2. Scene change detected with the reset flag.
3. No scene change detected with the reset flag.

The hints appear as text blurbs in the center of the screen, messages in the DLSS-FG log file, and in scenario 1, a goldenrod-yellow screen tint.

### 21.0 ENHANCED IN-GAME DEBUG VISUALIZATION

The developer FG NGX feature contains an in-game debug visualization feature that can be used to accurately validate that GPU and CPU input resources are correct. Consult the **Troubleshooting and Optional Features** section of the [DLSS-FG Programming Guide.pdf](<DLSS-FG Programming Guide.pdf>) for more details.

### 22.0 VSYNC WITH FRAME GENERATION

DLSS-G supports application-controlled VSync. This allows games to enable VSync while using Frame Generation, providing tear-free presentation with frame interpolation.

#### 22.1 CHECKING VSYNC SUPPORT

Applications should check `sl::DLSSGState::bIsVsyncSupportAvailable` to determine if VSync is supported with the current DLSS-G build:

```cpp
sl::DLSSGState state{};
if(SL_SUCCEEDED(result, slDLSSGGetState(viewport, state)))
{
    if(state.bIsVsyncSupportAvailable == sl::Boolean::eTrue)
    {
        // VSync is supported - enable VSync toggle in UI
        showVSyncToggle(true);
    }
    else
    {
        // VSync not supported with this build - hide or disable VSync toggle
        showVSyncToggle(false);
    }
}
```

#### 22.2 ENABLING VSYNC

When VSync is supported, the application controls VSync through the standard `SyncInterval` parameter in the `Present()` call:

* `SyncInterval = 0`: VSync disabled (tearing allowed)
* `SyncInterval = 1`: VSync enabled (present every refresh)
* `SyncInterval > 1`: **Not supported.** Will be clamped to 1 with a warning

#### 22.3 NVCPL PRECEDENCE

NVIDIA Control Panel (NVCPL) VSync settings take precedence over application settings:

| NVCPL Setting | App Request | Result |
|---------------|-------------|--------|
| Force OFF | VSync ON | **VSync OFF** (NVCPL wins) |
| Force OFF | VSync OFF | VSync OFF |
| Force ON | VSync ON | VSync ON |
| Force ON | VSync OFF | **VSync ON** (NVCPL wins) |
| App-Controlled | VSync ON | VSync ON |
| App-Controlled | VSync OFF | VSync OFF |

This behavior matches standard NVIDIA driver VSync precedence and ensures user preferences are respected.

#### 22.4 MS HYBRID SYSTEM SUPPORT

**Important:** On MS Hybrid (Optimus) systems, NVCPL VSync settings historically had no effect because the NVIDIA discrete GPU renders frames but the integrated GPU (Intel/AMD) handles display output. The NVIDIA driver has no control over the iGPU's display path.

With App-Enabled VSync, **NVCPL VSync settings now work on MS Hybrid systems** when Frame Generation is enabled. Streamline reads NVCPL settings via DRS and enforces them through frame pacing (RSync), making this the first time NVCPL VSync overrides function on hybrid graphics configurations.

#### 22.5 LIMITATIONS

**VSync is NOT supported in the following scenarios:**

1. **VSync Interval > 1**: Only `SyncInterval = 1` is supported. Higher intervals are clamped to 1.

2. **Excluded Platforms**: VSync is not supported on GeForce NOW (GFN) platforms. GFN streams rendered frames to the client device rather than displaying them directly on the game machine, so it has its own mechanisms for handling frame synchronization and is not covered by the Streamline VSync implementation.

3. **Vulkan**: VSync with Frame Generation is only supported on D3D12.

#### 22.6 IFLIP REQUIREMENT

VSync with Frame Generation relies on Independent Flip (IFLIP) a Windows presentation mode where the GPU flips directly to the application's back buffer on the display, bypassing Desktop Window Manager (DWM) composition. IFLIP is critical for low-latency VSync because it allows the driver to control presentation timing at the hardware level.

**If VSync is enabled with Frame Generation on a system that does not support IFLIP, high latency is expected.** On such systems it is better to not enable VSync.

IFLIP is supported on all modern systems under normal conditions. However, certain configurations can prevent IFLIP from being used for example, certain overlays, resolution mismatches, or forced DWM composition.

**Diagnosing IFLIP with NVIDIA FrameView:**

If unexpectedly high latency is observed with VSync and Frame Generation enabled, verify that the system is using IFLIP:

1. Capture a trace with [NVIDIA FrameView](https://developer.nvidia.com/frameview).
2. Open the trace CSV and look at the **`PresentMode`** column.
3. If the value contains **"Composed"** (e.g., `Composed: Flip`, `Composed: Copy with GPU GDI`), this means IFLIP was not available. High latency is expected in this case, and VSync should be disabled.
4. If the value shows **"Hardware: Independent Flip"**, IFLIP is active and VSync should operate with low latency.

#### 22.7 HIGH LATENCY WITH HIGH FG MULTIPLIERS

When using high Frame Generation multipliers with VSync enabled on low refresh rate monitors, users will experience significantly increased input latency. This occurs because frames are generated faster than the display can present them, causing frame queue backup.

Note that the maximum Frame Generation multiplier is **6×** (i.e. 5 generated frames + 1 real frame).

**High-latency thresholds:**

| Monitor Refresh Rate | Maximum Safe FG Multiplier | High Latency If... |
|---------------------|---------------------------|-------------------|
| 60Hz | 4× | FG > 4× |
| 75Hz | 5× | FG > 5× |

**Example:** At 60Hz with 6× Frame Generation, 6 frames are produced per render cycle but the display can only show 60 frames per second. Frames accumulate faster than they can be displayed, causing latency to grow.

**Recommendations for users experiencing high latency with VSync:**
* Reduce Frame Generation multiplier
* Use a higher refresh rate display
* Disable VSync if latency is critical
