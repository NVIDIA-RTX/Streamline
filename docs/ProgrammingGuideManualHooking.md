
Streamline - Manual Hooking
=======================

Version 2.14.16
=======

The automated global hooking is a great way to quickly enable SL features in any application. However, this can lead to unnecessary overhead caused by the entire API redirection through SL proxies and problems with tools and 3rd party libraries which do not expect to receive SL proxies as inputs.

To address this SL provides "manual" hooking which is slightly more involved style of integration leveraging `slGetNativeInterface` and `slUpgradeInterface` APIs.

> **IMPORTANT:**
> Please read the general [ProgrammingGuide.md](ProgrammingGuide.md) before proceeding with this advanced method of integration.

### 1.0 LINKING

#### 1.1 DirectX

When using D3D11 or D3D12 one can choose between statically linking `sl.interposer.lib` or continue linking `dxgi.lib`, `d3d12.lib`, `d3d11.lib` as usual and get only `sl*` methods from the `sl.interposer.dll` after loading it dynamically (please see [secure load](ProgrammingGuide.md#211-security)).

> **IMPORTANT**
> For DirectX integrations statically linking `sl.interposer.lib` is the preferred and easiest method since it allows access to all helper methods from `sl_$feature.h` whilst still achieving minimal CPU overhead. When using dynamic linking helpers cannot be used directly due to missing `slGetFeatureFunction` API at link time.

#### 1.1 Vulkan

When using Vulkan linking the `sl.interposer.lib` would result in additional CPU overhead so the best approach is to dynamically load `sl.interposer.dll` instead of `vulkan-1.dll` and use `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr` provided by the SL.

> **IMPORTANT**
> SL `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr` will return addresses from `vulkan-1.dll` for the entire Vulkan API **except for the few functions intercepted by SL**. For more details please continue reading.

### 2.0 MANUAL HOOKING API

All required definitions and declarations can be found in the `sl_hooks.h` header. Here is the list of all hooks used in this SDK:

```cpp
//! NOTE: Adding new hooks require sl.interposer to be recompiled
//! 
//! IMPORTANT: Since SL interposer proxies supports many different versions of various D3D/DXGI interfaces 
//! we use only base interface names for our hooks. 
//! 
//! For example if API was added in IDXGISwapChain5::FUNCTION it is still named eIDXGISwapChain_FUNCTION (there is no 5 in the name)
//! 
enum class FunctionHookID : uint32_t
{
    //! Mandatory - IDXGIFactory*
    eIDXGIFactory_CreateSwapChain,
    eIDXGIFactory_CreateSwapChainForHwnd,
    eIDXGIFactory_CreateSwapChainForCoreWindow,
    
    //! Mandatory - IDXGISwapChain*
    eIDXGISwapChain_Present,
    eIDXGISwapChain_Present1,
    eIDXGISwapChain_GetBuffer,
    eIDXGISwapChain_ResizeBuffers,
    eIDXGISwapChain_ResizeBuffers1,
    eIDXGISwapChain_GetCurrentBackBufferIndex,
    eIDXGISwapChain_SetFullscreenState,
    //! Internal - please ignore when doing manual hooking
    eIDXGISwapChain_Destroyed,

    //! Mandatory - ID3D12Device*
    eID3D12Device_CreateCommandQueue,

    //! Mandatory - Vulkan
    eVulkan_Present,
    eVulkan_CreateSwapchainKHR,
    eVulkan_DestroySwapchainKHR,
    eVulkan_GetSwapchainImagesKHR,
    eVulkan_AcquireNextImageKHR,
    eVulkan_DeviceWaitIdle,
    eVulkan_CreateWin32SurfaceKHR,
    eVulkan_DestroySurfaceKHR,

    eMaxNum
};
```

> **WARNING:** Present invocation requirement in manual hooking
>
> When `PreferenceFlag::eUseManualHooking` is set, you must ensure the common plugin’s `presentCommon()` function is called every frame. Failing to invoke `presentCommon()` will lead to incorrect behavior and memory growth (especially across swapchain resizes).
>
> - Verification: set a breakpoint in the common plugin’s `presentCommon()` function. It should be hit consistently every frame. During window resize, confirm that it executes around the transition.

> **If `presentCommon()` is not called:**
> - Verify Streamline is initialized and enabled before any rendering begins.
> - Route your frame presentation path to invoke the common plugin’s `presentCommon()` exactly once per frame. In manual hooking, the recommended approach is to call `slUpgradeInterface` immediately after creating the frame presentation interface (e.g., the swapchain on DirectX), so Streamline can integrate with your presentation path and invoke `presentCommon()`.
> - If you have multiple presentation code paths (alternate backends/window modes), unify or fan-in so all active paths invoke `presentCommon()`.
> - Re-run with a breakpoint in `presentCommon()` to confirm it executes every frame and around window resizes.

### 3.0 INITIALIZATION AND SHUTDOWN

Call `slInit` **before any of the hooks mentioned in section [2.0](#20-manual-hooking-api) could be triggered** (like for example, DXGI calls to create swap-chain) and make sure to specify the special flag `PreferenceFlag::eUseManualHooking` as shown in the snippet below:

```cpp
#include <sl.h>
#include <sl_consts.h>
#include <sl_hooks.h>

sl::Preferences pref{};
// Inform SL that we are doing advanced integration
pref.flags |= PreferenceFlag::eUseManualHooking;
// Set other preferences, request features etc.
if(SL_FAILED(result, slInit(pref)))
{
    // Handle error, check the logs
}
```

> **NOTE:**
> Unlike regular SL integrations, the D3D device can be created before or after `slInit` is called. When using Vulkan however, device still must be created **after** the `slInit` call, for more details please continue reading.

When shutting down, nothing much changes from the regular SL integration. Simply call `slShutdown()` **before destroying** dxgi/d3d11/d3d12/vk instances, devices and other components in your engine.

```cpp
if(SL_FAILED(result, slShutdown()))
{
    // Handle error, check the logs
}
```

### 4.0 ADDING HOOKS TO YOUR ENGINE

#### 4.1 DirectX

There are two scenarios depending on whether `sl.interposer.lib` is linked directly or not with the host application:

```cpp

//! SL LIB LINKED WITH THE GAME, SL PROXIES ARE PROVIDED TO THE HOST

if(!sl::security::verifyEmbeddedSignature(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll"))
{
    // SL module not signed, disable SL
}
else
{
    // SL digitally signed, OK to use it!

    // D3D11
    // 
    // IMPORTANT: Note that for D3D11 there is NO proxy for a device in any scenario
    ID3D11Device* nativeDevice;
    [nativeDevice, proxySwapchain] = D3D11CreateDeviceAndSwapChain();

    // D3D12
    proxyDevice = D3D12CreateDevice();

    ID3D12Device* nativeDevice{};
    if(SL_FAILED(result, slGetNativeInterface(proxyDevice, &nativeDevice))
    {
        // Handle error, check logs
    }
    // Normally this is not done for D3D11 (see above)
    auto proxyFactory = DXGICreateFactory();
    auto proxySwapChain = proxyFactory->CreateSwapChain();

    // DXGI
    //
    // This part is identical for D3D11 and D3D12    
    IDXGISwapChain* nativeSwapchain{};
    if(SL_FAILED(result, slGetNativeInterface(proxySwapChain, &nativeSwapchain)))
    {
        // Handle error, check logs
    }
}
```

```cpp

//! SL LIB NOT LINKED WITH THE GAME, NATIVE INTERFACES ARE PROVIDED TO THE HOST

if(!sl::security::verifyEmbeddedSignature(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll"))
{
    // SL module not signed, disable SL
}
else
{
    // SL digitally signed, we can load and use it!
    auto mod = LoadLibrary(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll");

    // Declare all SL functions here, showing only the one we are using
    SL_FUN_DECL(slUpgradeInterface);
    SL_FUN_DECL(slGetNativeInterface);

    // Get all SL functions here, showing only the one we are using
    auto slUpgradeInterface = reinterpret_cast<PFunSlGetUpgradeInterface>(GetProcAddress(mod, "slUpgradeInterface"));
    auto slGetNativeInterface = reinterpret_cast<PFunSlGetNativeInterface>(GetProcAddress(mod, "slGetNativeInterface"));

    // D3D11
    // 
    // IMPORTANT: Note that for D3D11 there is NO proxy for a device in any scenario
    ID3D11Device* nativeDevice;
    [nativeDevice, nativeSwapchain] = D3D11CreateDeviceAndSwapChain();

    // D3D12
    auto nativeDevice = D3D12CreateDevice();
    auto nativeFactory = DXGICreateFactory();

    ID3D12Device* proxyDevice = nativeDevice;
    if(SL_FAILED(result, slUpgradeInterface(&proxyDevice))
    {
        // Handle error, check logs
    }

    //! IMPORTANT: Any create swap chain API must be intercepted as specified by `enum class FunctionHookID` in sl_hooks.h
    //!
    //! Therefore here we are using proxy factory and not a native one!
    IDXGIFactory* proxyFactory = nativeFactory
    if(SL_FAILED(result, slUpgradeInterface(&proxyFactory)))
    {
        // Handle error, check logs
    }

    //! Now we can obtain our proxy swap-chain
    proxySwapChain = proxyFactory->CreateSwapChain();

    //! Next we get our proxy device
    ID3D12Device* proxyDevice = nativeDevice;
    if(SL_FAILED(result, slUpgradeInterface(&proxyDevice)))
    {
        // Handle error, check logs
    }

    //! Native swap-chain so we can call non-intercepted API directly
    IDXGISwapChain* nativeSwapchain{};
    if(SL_FAILED(result, slGetNativeInterface(proxySwapChain, &nativeSwapchain)))
    {
        // Handle error, check logs
    }
}
```

> **IMPORTANT**
> One should use native interfaces EVERYWHERE in the host application EXCEPT for the APIs which are hooked by SL (listed as enums in sl_hooks.h).

For example, here is how to call NVAPI functions from the host side:

```cpp
//! IMPORTANT: When using 3rd party libs use native interfaces NOT the SL proxies
NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS state{}
NvAPI_D3D12_SetCreatePipelineStateOptions(nativeDevice, &state);
```

Here is how one would add SL hooks to the swap-chain:

```cpp
//! FULL LIST OF SL API HOOKS IN sl_hooks.h
//!
//! eIDXGISwapChain_Present,
//! eIDXGISwapChain_Present1,
//! eIDXGISwapChain_GetBuffer,
//! eIDXGISwapChain_ResizeBuffers,
//! eIDXGISwapChain_ResizeBuffers1,
//! eIDXGISwapChain_GetCurrentBackBufferIndex,
//! eIDXGISwapChain_SetFullscreenState,
//! eID3D12Device_CreateCommandQueue

//! IMPORTANT: NOT INTERCEPTED BY STREAMLINE
//! 
HRESULT myrhi::SwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc)
{
    // EXISTING ENGINE CODE
    return nativeSwapchain->GetFullscreenDesc(pDesc);
}

//! IMPORTANT: INTERCEPTED BY STREAMLINE (eIDXGISwapChain_ResizeBuffers) - USING PROXY AS NEEDED
//! 
HRESULT myrhi::SwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // NEW CODE
    if(g_slEnabled)
    {
        return proxySwapchain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);        
    }
    // EXISTING ENGINE CODE
    return nativeSwapchain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// and so on, calls to proxies must be added for all hooks listed in sl_hooks.h
```

Here is an example for ID3D12Device:

```cpp
//! IMPORTANT: NOT INTERCEPTED BY STREAMLINE
//! 
HRESULT myrhi::D3D12Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator)
{
    // EXISTING ENGINE CODE
    return nativeDevice->CreateCommandAllocator(type, riid, ppCommandAllocator);
}

//! IMPORTANT: INTERCEPTED BY STREAMLINE (eID3D12Device_CreateCommandQueue) - USING PROXY AS NEEDED
//! 
HRESULT myrhi::D3D12Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue)
{
    // NEW CODE
    if(g_slEnabled)
    {
        return proxyDevice->CreateCommandQueue(pDesc, riid, ppCommandQueue);
    }
    // EXISTING ENGINE CODE
    return nativeDevice->CreateCommandQueue(pDesc, riid, ppCommandQueue);    
}

// and so on, calls to proxies must be added for all hooks listed in sl_hooks.h
```

Skip Vulkan information and go to section [5.0 Informing SL about the device to use](#50-informing-sl-about-the-device-to-use)

#### 4.2 Vulkan

SL hooking in Vulkan is also rather simple. All VK functions are obtained from `vulkan-1.dll` as usual but in addition for ones listed in the `sl_hooks.h` we need to obtain proxies from `sl.interposer.dll`. Here is some sample code:

```cpp
// VK export functions from the SL interposer, we use them to get our proxies
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddrProxy{}
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddrProxy{}

// Always secure load SL modules
if(!sl::security::verifyEmbeddedSignature(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll"))
{
    // SL module not signed, disable SL
}
else
{
    auto mod = LoadLibray(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll");

    //! GetProcAddr proxies
    //!
    //! IMPORTANT: These proxies return functions from `vulkan-1.dll` except for those intercepted by SL and listed in sl_hooks.h
    auto vkGetDeviceProcAddrProxy = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(mod, "vkGetDeviceProcAddr"));
    auto vkGetInstanceProcAddrProxy = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vkGetInstanceProcAddr"));

    // Get SL proxies for ALL mandatory APIs listed in the sl_hooks.h
    PFN_vkCreateSwapchainKHR  vkCreateSwapchainKHRProxy = reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetDeviceProcAddrProxy(device,"vkCreateSwapchainKHR"));
    PFN_vkDestroySwapchainKHR  vkDestroySwapchainKHRProxy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(vkGetDeviceProcAddrProxy(device,"vkDestroySwapchainKHR"));
    PFN_vkGetSwapchainImagesKHR  vkGetSwapchainImagesKHRProxy = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(vkGetDeviceProcAddrProxy(device,"vkGetSwapchainImagesKHR"));
    PFN_vkAcquireNextImageKHR  vkAcquireNextImageKHRProxy = reinterpret_cast<PFN_vkAcquireNextImageKHR>(vkGetDeviceProcAddrProxy(device,"vkAcquireNextImageKHR"));
    PFN_vkQueuePresentKHR vkQueuePresentKHRProxy = reinterpret_cast<PFN_vkQueuePresentKHR>(vkGetDeviceProcAddrProxy(device,"vkQueuePresentKHR"));
    
    // Optional but it makes integrations much easier since SL will take care of adding requires extensions, enabling features and any extra command queues
    PFN_vkCreateDevice vkCreateDeviceProxy = reinterpret_cast<PFN_vkCreateDevice>(vkGetInstanceProcAddrProxy(instance,"vkCreateDevice"));
    PFN_vkCreateInstance vkCreateInstanceProxy = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddrProxy(instance,"vkCreateInstance"));
}
```

If Vulkan function is NOT listed in the `sl_hooks.h` then it is not intercepted, continue to call base Vulkan function as normal. If specific Vulkan function is intercepted please modify your code as shown below:

```cpp

//! IMPORTANT: INTERCEPTED BY SL BUT OPTIONAL
//!
//! If not using the proxy then host is responsible for setting up all extensions, features, command queues and calling slSetVulkanInfo
VkResult myrhi::vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    //! IMPORTANT: Only call proxy for the instance that should be used by SL, skip proxy for any test instance

    // NEW CODE
    if(g_slEnabled)
    {
        // Proxy obtained from SL, takes care of all SL internal requirements when creating the instance
        return vkCreateInstanceProxy(pCreateInfo, pAllocator, pInstance);
    }
    // EXISTING ENGINE CODE
    return vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

//! IMPORTANT: INTERCEPTED BY SL BUT OPTIONAL
//!
//! If not using the proxy then host is responsible for setting up all extensions, features, command queues and calling slSetVulkanInfo
VkResult myrhi::vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    //! IMPORTANT: Only call proxy for the instance that should be used by SL, skip proxy for any test device

    // NEW CODE
    if(g_slEnabled)
    {
        // Proxy obtained from SL, takes care of all SL internal requirements when creating the device
        return vkCreateDeviceProxy(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }
    // EXISTING ENGINE CODE
    return vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

//! IMPORTANT: NOT INTERCEPTED BY SL
//!
void myrhi::vkCmdExecuteCommands(VkCommandBuffer CommandBuffer, uint32_t CommandBufferCount, const VkCommandBuffer* pCommandBuffers)
{
    // EXISTING ENGINE CODE
    vkCmdExecuteCommands(CommandBuffer, CommandBufferCount, pCommandBuffers);
}

//! IMPORTANT: INTERCEPTED BY SL
//!
VkResult myrhi::vkQueuePresentKHR(VkQueue Queue, const VkPresentInfoKHR* PresentInfo)
{
    // NEW CODE
    if(g_slEnabled)
    {
        // Proxy obtained from SL
        return vkQueuePresentKHRProxy(Queue, PresentInfo);
    }
    // EXISTING ENGINE CODE
    return vkQueuePresentKHR(Queue, PresentInfo)
}
```

### 5.0 INFORMING SL ABOUT THE DEVICE TO USE

Since SL is no longer intercepting all D3D/DXGI/Vulkan calls, the host needs to provide the device which is going to be used to initialize all SL features.

#### 5.1 DirectX

When using D3D simply create your device and call `slSetD3DDevice`. The following code snippet demonstrates how to do this:

```cpp
// Inform SL about the device we created
if(SL_FAILED(result, slSetD3DDevice(nativeDevice))
{
    // Handle error, check the logs
}
```

Skip Vulkan information and go to section [6.0 Tagging resources](#60-tagging-resources)

#### 5.2 Vulkan

**Important: If using `vkCreateDeviceProxy` and `vkCreateInstanceProxy` provided by SL you can skip this section.**

##### 5.2.1 INSTANCE AND DEVICE ADDITIONS


SL features can request special extensions, device features or even modifications to the number of command queues which need to be generated. Therefore before creating VK instance and device you must call `slGetFeatureRequirements` **for each enabled feature** to
get this information. Here is an example on how to obtain extensions, device features and additional queues needed by sl.dlss_g:

```cpp
sl::FeatureRequirements reqs{};
if (SL_FAILED(result, slGetFeatureRequirements(sl::kFeatureDLSS_G, reqs)))
{
    // Feature is not requested on slInit or failed to load, check logs, handle error
}
else
{
    // Feature is loaded, we can check the requirements

    // Add extra queues (if any)
    myConfig.extraGraphicsQueues += reqs.numGraphicsQueuesRequired;
    myConfig.extraComputeQueues += reqs.numComputeQueuesRequired;
	myConfig.extraOpticalFlowQueues += reqs.numOpticalFlowQueuesRequired;

    // Add extra features or extensions (if any)
    for (uint i = 0; i < reqs.numInstanceExtensions; i++)
    {
        myConfig.pluginInstanceExtensions.push_back(reqs.instanceExtensions[i]);
    }
    for (uint i = 0; i < reqs.numDeviceExtensions; i++)
    {
        myConfig.pluginDeviceExtensions.push_back(reqs.deviceExtensions[i]);
    }
    // Use helpers from sl_helpers_vk.h 
    VkPhysicalDeviceVulkan12Features features12 = sl::getVkPhysicalDeviceVulkan12Features(reqs.numFeatures12, reqs.features12);
    VkPhysicalDeviceVulkan13Features features13 = sl::getVkPhysicalDeviceVulkan13Features(reqs.numFeatures13, reqs.features13);
}
```

> **NOTE:**
> If the host application intends to view names of SL and NGX feature resources (e.g. DLSS textures and buffers) and SL and NGX profiling markers in tools such as Nsight, enable the `VK_EXT_debug_utils` instance extension on its `VkInstance` (in addition to the extensions reported by `slGetFeatureRequirements`).

Now that you have the information about the additional extensions, features and queues required by SL feature(s) you can proceed to create Vulkan instance and device. For more details please check out the [implementation](../source/core/sl.interposer/vulkan/wrapper.cpp).

Note that Vulkan supports optical flow feature extension from Nvidia natively, as required by DLSS-G, starting with VK_API_VERSION_1_1 (recommended version is VK_API_VERSION_1_3) and minimum Nvidia driver version 527.64 on Windows and 525.72 on Linux. Vulkan SDK version 1.3.231.0 supports validation layer for this extension.
Native optical flow feature in Vulkan requires its own optical flow queue whose family is exclusive to graphics, compute and copy queue families and whose info is required to be passed during Vulkan device creation and has certain requirements for its use in DLSS-G:
1. Native optical flow queue family cannot be the same as that of any of the other queues of its client.
2. Its queue should be the very first one of the very first native optical flow-capable family resulting in the required queue index is 0.
For more details, please check out the [helper getOpticalFlowQueueInfo](../source/platforms/sl.chi/vulkan.cpp) to retrieve the same as well as the implementation link referred above.
In the absence of this setup in manual hooking mode, DLSS-G runs optical flow in an interop mode.

##### 5.2.2 PROVIDING INSTANCE, DEVICE AND OTHER INFORMATION TO SL

Once that is done you need to inform SL about the devices, instance and queues using the following code:

```cpp

sl::VulkanInfo info{};
info.device = myVKDevice;
info.instance = myVKInstance;
info.physicalDevice = myVKPhysicalDevice;
info.computeQueueIndex = computeQueueIndexStartForSL; // Where first SL queue starts after host's queues
info.computeQueueFamily = myComputeQueueFamily;
info.graphicsQueueIndex = graphicsQueueIndexStartForSL; // Where first SL queue starts after host's queues
info.graphicsQueueFamily = myGraphicsQueueFamily;

// Inform SL about the VK devices, instances etc
if(SL_FAILED(result, slSetVulkanInfo(info)))
{
    // Handle error, check the logs
}
```

### 6.0 TAGGING RESOURCES

Since SL is no longer able to track resource creation and their states when using manual hooking mechanism, it is **mandatory for the host to provide resource information** when tagging them.

#### 6.1 DirectX

When using D3D tagging is similar to the regular SL integrations with the exception of having to provide the correct resource state. Here is an example:

```cpp
// Host providing native D3D12 resource state
//
// IMPORTANT: State needs to be correct when tagged resource is used by SL and not necessarily at this point when it is tagged.
//
sl::Resource mvec = { sl::ResourceType::Tex2d, mvecResource, nullptr, nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr };
sl::ResourceTag mvecTag = sl::ResourceTag {&mvec, sl::kBufferTypeMvec, sl::ResourceLifecycle::eOnlyValidNow, &mvecExtent };
slSetTagForFrame(*currentFrame, viewport, &mvecTag, 1, cmdList);
```

#### 6.2 Vulkan

When using Vulkan tagging is a bit more involved since host needs to provide additional information about each tagged resources. Here is an example:

```cpp
// Host providing native resource state and additional information about the resource
//
// IMPORTANT: Image layout needs to be correct when tagged resource is used by SL and not necessarily at this point when it is tagged.
//
// Vulkan does not provide a way to obtain VkImage description so host must provide one.
sl::Resource mvec = { sl::ResourceType::Tex2d, mvecImage, nullptr, mvecImageView, VK_IMAGE_LAYOUT_GENERAL , 1920, 1080, VK_FORMAT_R32G32_SFLOAT, 1, 1, 0, 0, VK_IMAGE_USAGE_STORAGE_BIT };
sl::ResourceTag mvecTag = sl::ResourceTag {&mvec, sl::kBufferTypeMvec, sl::ResourceLifecycle::eOnlyValidNow, &mvecExtent };
slSetTagForFrame(*currentFrame, viewport, &mvecTag, 1, cmdList);
```

> **IMPORTANT:**
> Failure to provide correct resource state will cause your application to generate D3D/Vulkan validation errors and potential bugs when resources are used by SL features.

### 7.0 RESTORING COMMAND LIST(BUFFER) STATE

When manual hooking is used the host application is no longer using an SL proxy for the command lists (CL), hence it is not possible for SL to restore the CL state after each `slEvaluateFeature` call.

#### 7.1 DirectX

Here is the code snippet showing what command list states are restored by SL in the regular integration mode, host application should do the same:

```cpp
void restorePipeline(sl::interposer::D3D12GraphicsCommandList* cmdList)
{
    if (cmdList->m_numHeaps > 0)
    {
        cmdList->SetDescriptorHeaps(cmdList->m_numHeaps, cmdList->m_heaps);
    }
    if (cmdList->m_rootSignature)
    {
        cmdList->SetComputeRootSignature(cmdList->m_rootSignature);
        for (auto& pair : cmdList->m_mapHandles)
        {
            cmdList->SetComputeRootDescriptorTable(pair.first, pair.second);
        }
        for (auto& pair : cmdList->m_mapCBV)
        {
            cmdList->SetComputeRootConstantBufferView(pair.first, pair.second);
        }
        for (auto& pair : cmdList->m_mapSRV)
        {
            cmdList->SetComputeRootShaderResourceView(pair.first, pair.second);
        }
        for (auto& pair : cmdList->m_mapUAV)
        {
            cmdList->SetComputeRootUnorderedAccessView(pair.first, pair.second);
        }
        for (auto& pair : cmdList->m_mapConstants)
        {
            cmdList->SetComputeRoot32BitConstants(pair.first, pair.second.Num32BitValuesToSet, pair.second.SrcData, pair.second.DestOffsetIn32BitValues);
        }
    }
    if (cmdList->m_pso)
    {
        cmdList->SetPipelineState(cmdList->m_pso);
    }
    if (cmdList->m_so)
    {
        static_cast<ID3D12GraphicsCommandList4*>(cmdList)->SetPipelineState1(cmdList->m_so);
    }   
}
```

#### 7.2 Vulkan

Here is the code snippet showing what command buffer states are restored by SL in the regular integration mode, host application should do the same:

```cpp
void restorePipeline(VkCommandBuffer cmdBuffer)
{
    VulkanThreadContext* thread = (VulkanThreadContext*)m_getThreadContext();

    if (thread->PipelineBindPoint != VK_PIPELINE_BIND_POINT_MAX_ENUM)
    {
        vkCmdBindPipeline(cmdBuffer, thread->PipelineBindPoint, thread->Pipeline);
    }
    if (thread->PipelineBindPointDesc != VK_PIPELINE_BIND_POINT_MAX_ENUM)
    {
        vkCmdBindDescriptorSets(cmdBuffer, thread->PipelineBindPointDesc, thread->Layout, thread->FirstSet, thread->DescriptorCount, thread->DescriptorSets, thread->DynamicOffsetCount, thread->DynamicOffsets);
    }
    
    return ComputeStatus::eOk;
}
```

> **IMPORTANT:**
> Failure to restore command list(buffer) state correctly will cause your application to crash or misbehave in some other form.
