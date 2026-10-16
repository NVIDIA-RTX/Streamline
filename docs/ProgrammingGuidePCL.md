

Streamline - PCL Stats
=======================

>The focus of this guide is on using Streamline to integrate PCL (PC Latency) Stats into an application.

Version 2.14.16
=======

The application should not explicitly check for GPU HW, vendor, and driver version.

PCL Stats enables real time measurement of per-frame PC latency (PCL) during gameplay​. E2E system latency = PCL + peripheral latency + display latency​. And PCL = input sampling latency + simulation start to present + present to displayed.

Windows messages with ID == `sl::PclState::statsWindowMessage` are sent to the application periodically. The time this ping message is sent is recorded and is used to measure the latency between the simulated input and the application picking up the input. This is the input sampling latency.

Typically, the application's message pump would process keyboard and mouse input messages into a queue to be read in the upcoming frame. On seeing the ping message, the application must either call `slPclSetMarker` or `sl::PclMarker::ePCLatencyPing` to send the ping marker right away, or put it in the queue, or set a flag for the next simulation to send the ping marker.

> **NOTE:**
> The exact timing of the ping marker does not matter. It is the frame index parameter that identify which frame picks up the simulated input. For example, since the frame index is usually incremented at simulation start, in the case of the ping marker being sent at message pump before simulation start, use current frame index +1.


### 1.0 INITIALIZE AND SHUTDOWN

Call `slInit` as early as possible (before any dxgi/d3d11/d3d12 APIs are invoked).  Similar to sl.common, PCL is loaded by default and doesn't need to be explicitly requested via `sl::Preferences::featuresToLoad` (as required for other plugins).

```cpp

#include <sl.h>
#include <sl_pcl.h>

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

### 2.0 CHECK IF PCL STATS IS SUPPORTED

As soon as SL is initialized, you can check if PCL Stats is available for the specific adapter you want to use:

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
            if (SL_FAILED(result, slIsFeatureSupported(sl::kFeaturePCL, adapterInfo)))
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

> **IMPORTANT:**
> **PCL Stats is supported on all GPU hardwares, vendors, and driver versions**. As long as `sl::kFeaturePCL` is supported, the application should always make the same `slPCLSetMarker` calls without any explicit checks for user enablement, or GPU hardware/vendor/driver version.


### 3.0 ADD SL PCL STATS TO THE RENDERING PIPELINE

Call `slPclSetMarker` at the appropriate locations where markers need to be injected:

```cpp
bool isPCLSupported = slIsFeatureSupported(sl::kFeaturePCL, adapterInfo);
if (!isPCLSupported)
{
    return;
}

// Using helpers from sl_pcl.h

// Mark the section where specific activity is happening.
//
// Here for example we will make the simulation code.
//

// Starting new frame, grab handle from SL
sl::FrameToken* currentFrame{};
if(SL_FAILED(res, slGetNewFrameToken(&currentFrame))
{
    // Handle error
}

// Simulation start
if(SL_FAILED(res, slPclSetMarker(sl::PclMarker::eSimulationStart, *currentFrame)))
{
    // Handle error
}

// Simulation code goes here

// Simulation end
if(SL_FAILED(res, slPclSetMarker(sl::PclMarker::eSimulationEnd, *currentFrame)))
{
    // Handle error
}   

// When checking for custom low latency messages inside the Windows message loop
//
if(pclState.statsWindowMessage == msgId) 
{
    // PCL ping based on custom message

    // First scenario, using current frame
    if(SL_FAILED(res, slPclSetMarker(sl::PclMarker::ePCLatencyPing, *currentFrame)))
    {
        // Handle error
    }
    
    // Second scenario, sending ping BEFORE simulation started, need to advance to the next frame
    sl::FrameToken* nextFrame{};
    auto nextIndex = *currentFrame + 1;
    if(SL_FAILED(res, slGetNewFrameToken(&nextFrame, &nextIndex))
    {
        // Handle error
    }
    if(SL_FAILED(res, slPclSetMarker(sl::PclMarker::ePCLatencyPing, *nextFrame)))
    {
        // Handle error
    }
}
```

### 4.0 MIGRATING FROM SL REFLEX

PCL markers were part of SL Reflex in earlier versions of SL.  If migrating from one of those releases, the following changes will be required:

- Make sure you copy the new `sl.pcl.dll` to your build (similar to `sl.common.dll`)
- PCL has its own `slIsFeatureSupported(sl::kFeaturePCL, ...)` distinct from `sl::kFeatureReflex` (which is still required if using SL Reflex)
- `slReflexSetMarker()` helper in `sl_reflex.h` is replaced with `slPCLSetMarker()` in sl_pcl.h
- Markers in `sl::ReflexMarker` enum are now in `sl::PCLMarker` enum *class*
    - Markers are no longer in top-level `sl::` namespace, use `sl::PCLMarker::`
    - If you were using the implicit cast to `uint32_t`, it will now need to be explicit
    - `eInputSample` marker was removed, this was already deprecated and removed in the native Reflex SDK but was never propagated to SL
