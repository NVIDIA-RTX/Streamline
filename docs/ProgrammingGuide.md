
Streamline - SL
=======================

Version 2.14.16
=======

1 SETTING UP
-------------

### 1.1 MACHINE CONFIGURATION

The following is needed to use SL features:

* Windows PC with Windows 10 RS3 (64-bit) or newer
* GPU capable of running DirectX 11 and Vulkan 1.2 or higher

> **NOTE:**
> At the moment, some features only work on specific NVIDIA hardware. Please see below on how to check if a feature is supported or not.

2 INTEGRATING SL WITH YOUR APPLICATION
---------------------------------

### 2.1 ADDING SL TO YOUR APPLICATION

The SL SDK comes with several header files, `sl.h` and `sl_*.h`, which are located in the `./include` folder. Two main header files are `sl.h`and `sl_consts.h` and they should be included always. Depending on which SL features are used by your application, additional header(s) should be included (see below for more details). Since SL can work as an interposer or as a regular library, there are several ways it can be integrated into your application:

* If you are statically linking `d3d11.lib`, `d3d12.lib` and/or `dxgi.lib`, there are two options:
  * remove standard libraries from the linking stage and and link `sl.interposer.lib` instead; SL will automatically intercept API it requires to function correctly
  * keep linking the standard libraries, load `sl.interposer.dll` dynamically and redirect DXGI/D3D API calls as required (code sample can be found below)
* If you are dynamically loading `d3d11.dll`, `d3d12.dll` and `dxgi.dll` also dynamically load `sl.interposer.dll`
* If you are using Vulkan, instead of `vulkan-1.dll` dynamically load `sl.interposer.dll` and obtain `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` as usual to get the rest of the Vulkan API.

> **IMPORTANT**
> For the optimal and recommended way of integrating SL please see the [advanced guide on manual hooking](ProgrammingGuideManualHooking.md) after you finish reading this guide.

Here is how SL library can be loaded dynamically and used instead of standard DXGI/D3D API while continuing to link standard `d3d11.lib`, `d3d12.lib` and `dxgi.lib` libraries (if needed or desired to simplify build process):

```cpp

// IMPORTANT: Always securely load SL library, see source/core/sl.security/secureLoadLibrary for more details
// Always secure load SL modules
if(!sl::security::verifyEmbeddedSignature(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll"))
{
    // SL module not signed, disable SL
}
else
{
    auto mod = LoadLibray(PATH_TO_SL_IN_YOUR_BUILD + "/sl.interposer.dll");

    // These are the exports from SL library
    typedef HRESULT(WINAPI* PFunCreateDXGIFactory)(REFIID, void**);
    typedef HRESULT(WINAPI* PFunCreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI* PFunCreateDXGIFactory2)(UINT, REFIID, void**);
    typedef HRESULT(WINAPI* PFunDXGIGetDebugInterface1)(UINT, REFIID, void**);
    typedef HRESULT(WINAPI* PFunD3D12CreateDevice)(IUnknown* , D3D_FEATURE_LEVEL, REFIID , void**);

    // Map functions from SL and use them instead of standard DXGI/D3D12 API
    auto slCreateDXGIFactory = reinterpret_cast<PFunCreateDXGIFactory>(GetProcAddress(mod, "CreateDXGIFactory"));
    auto slCreateDXGIFactory1 = reinterpret_cast<PFunCreateDXGIFactory1>(GetProcAddress(mod, "CreateDXGIFactory1"));
    auto slCreateDXGIFactory2 = reinterpret_cast<PFunCreateDXGIFactory2>(GetProcAddress(mod, "CreateDXGIFactory2"));
    auto slDXGIGetDebugInterface1 = reinterpret_cast<PFunDXGIGetDebugInterface1>(GetProcAddress(mod, "DXGIGetDebugInterface1"));
    auto slD3D12CreateDevice = reinterpret_cast<PFunD3D12CreateDevice>(GetProcAddress(mod, "D3D12CreateDevice"));
}

// For example to create DXGI factory and D3D12 device we could do something like this:

IDXGIFactory1* DXGIFactory{};
if(s_useStreamline)
{
    // Interposed factory
    slCreateDXGIFactory1(IID_PPV_ARGS(&DXGIFactory));
}    
else    
{
    // Regular factory
    CreateDXGIFactory1(IID_PPV_ARGS(&DXGIFactory));
}

ID3D12Device* device{};
if(s_useStreamline)
{
    // Interposed device
    slD3D12CreateDevice(targetAdapter, deviceParams.featureLevel,IID_PPV_ARGS(&device));
}
else 
{
    // Regular device
    D3D12CreateDevice(targetAdapter, deviceParams.featureLevel,IID_PPV_ARGS(&device));
}

// IMPORTANT: When SL is enabled from this point onwards any new swap-chains or command lists will be managed by SL automatically

```

#### 2.1.1 SECURITY

All modules provided in the `./bin/x64` SDK folder are digitally signed by NVIDIA. There are two digital signatures on each SL module, one is a standard Windows store certificate and can be validated using `WinVerifyTrust` while the other is a custom NVIDIA certificate used to handle scenarios where the OS is compromised and Windows store certificates cannot be trusted. To secure your application from potentially malicious replacement modules, please do the following:

* Validate the digital signature on `sl.interposer.dll` using the `WinVerifyTrust` Win32 API when starting your application.
* Validate the public key for the NVIDIA custom digital certificate on `sl.interposer.dll` if using the binary provided by NVIDIA (see `sl::security::verifyEmbeddedSignature` in `include/sl_security.h` for more details)
* When calling `sl::security::verifyEmbeddedSignature` or `WinVerifyTrust` please keep in mind to **always pass in the full path to a sl.*.dll file** - relative paths are not allowed.

***It is strongly recommended*** to use the provided `sl.interposer.dll` binary and follow the above guidelines. The prebuilt binary automatically performs the above steps when loading SL plugins to ensure maximum security. If you decide to build your own `sl.interposer.dll`, make sure to enforce your own strict security policies.

> **IMPORTANT:**
> Note that if using provided development SL DLLs or if using self-built SL DLLs, they will not be signed.  In these cases (which should be used ONLY in development/debugging situations and never shipped), you will need to temporarily disable the app's signature-checking.

### 2.2 INITIALIZING SL

#### 2.2.1 SL FEATURE LIFECYCLE

Here is the typical lifecycle for SL features:

* Requested feature DLLs are loaded during `slInit` call.
  * This is (and must be) done before any DX/VK APIs are invoked (which is why the app must call slInit very early in its initialization)
  * The feature "request" process is detailed in [featuresToLoad in preferences](#222-preferences)
* At this point, required and supported features are loaded but are NOT yet initialized
  * As a result, methods like `slSet/Get/Eval` CANNOT BE USED immediately after slInit
* The app can and should call `slIsFeatureSupported` while enumerating adapters to determine which feature is supported and on which adapter
* The app can and should call `slGetFeatureRequirements` to determine OS, driver, rendering API and other requirements for the given feature
  * This method returns information about OS, driver, supported rendering APIs etc.
* Later in the application's initialization sequence, the app creates a rendering API device
  * Specifically, a device on the adapter where SL features are supported
* This device creation indirectly triggers SL feature initialization (but only if **manual hooking is not used or 3rd party overlays did not disable SL interposer**) since the device is now available
  * In practice, this means that each enabled and loaded feature's slOnPluginStartup is called
  * If any feature's slOnPluginStartup returns failure, that plugin will be unloaded
* To ensure correct functionality in any scenario the app must call `slSetD3DDevice` after the main device is created
  * In practice, this will explicitly initialize all plugins with the given device information
  * When using Vulkan calling `slSetVulkanInfo` is mandatory only if `vkCreateInstance` and `vkCreateDevice` and not handled by SL (see manual hooking guide for details)
* Note that features can fail to initialize (and thus be unloaded) even if they are declared as supported
  * For example, the app may create a D3D11 device for a feature that is supported on the device's adapter, but only when using a D3D12 device
* After device creation, any calls to SL will succeed only if the feature is FULLY functional on that device
  * This means that the set of features that return true from `slIsFeatureSupported` AFTER device creation can be smaller (but not larger) than the set that returned true from `slIsFeatureSupported` after slInit, but before device creation
  * And thus, at this point, any SL method can be used safely since all required and supported features are initialized
  * In addition, at this point it is possible to [explicitly allocate/free resources used by feature(s)](#28-explicit-resource-allocation-and-de-allocation)

> **IMPORTANT:**
> `slInit` must NOT be called within DLLMain entry point in your application because that can cause a deadlock.

#### 2.2.2 PREFERENCES

To control the behavior of the SL SDK, some preferences can be specified using the following data structure:

```cpp
//! Optional flags
enum PreferenceFlags : uint64_t
{
    //! IMPORTANT: This flag is now default!
    //!
    //! Host application is responsible for restoring CL state correctly after each 'slEvaluate' call
    eDisableCLStateTracking = 1 << 0,
    //! Disables debug text on screen in development builds
    egDisableDebugText = 1 << 1,
    //! IMPORTANT: Only to be used in the advanced integration mode, see the 'manual hooking' programming guide for more details
    eUseManualHooking = 1 << 2,
    //! Optional - Enables downloading of Over The Air (OTA) updates for SL and NGX
    //! This will invoke the OTA updater to look for new updates. A separate
    //! flag below is used to control whether or not OTA-downloaded SL Plugins are
    //! loaded.
    eAllowOTA = 1 << 3,
    //! Do not check OS version when deciding if feature is supported or not
    //! 
    //! IMPORTANT: ONLY SET THIS FLAG IF YOU KNOW WHAT YOU ARE DOING. 
    //! 
    //! VARIOUS WIN APIs INCLUDING BUT NOT LIMITED TO `IsWindowsXXX`, `GetVersionX`, `rtlGetVersion` ARE KNOWN FOR RETURNING INCORRECT RESULTS.
    eBypassOSVersionCheck = 1 << 4,
    //! Optional - If specified SL will create DXGI factory proxy rather than modifying the v-table for the base interface.
    //! 
    //! This can help with 3rd party overlays which are NOT integrated with the host application but rather operate via injection.
    eUseDXGIFactoryProxy = 1 << 5,
    //! Optional - Enables loading of plugins downloaded Over The Air (OTA), to
    //! be used in conjunction with the eAllowOTA flag.
    eLoadDownloadedPlugins = 1 << 6,
};

//! Engine types
//! 
enum class EngineType : uint32_t
{
    eCustom,
    eUnreal,
    eUnity,
};

//! Application preferences
//!
//! {1CA10965-BF8E-432B-8DA1-6716D879FB14}
SL_STRUCT_BEGIN(Preferences, StructType({ 0x1ca10965, 0xbf8e, 0x432b, { 0x8d, 0xa1, 0x67, 0x16, 0xd8, 0x79, 0xfb, 0x14 } }), kStructVersion1)
    //! Optional - In non-production builds it is useful to enable debugging console window
    bool showConsole = false;
    //! Optional - Various logging levels
    LogLevel logLevel = LogLevel::eDefault;
    //! Optional - Absolute paths to locations where to look for plugins, first path in the list has the highest priority
    const wchar_t** pathsToPlugins{};
    //! Optional - Number of paths to search
    uint32_t numPathsToPlugins = 0;
    //! Optional - Absolute path to location where logs and other data should be stored
    //! 
    //! NOTE: Set this to nullptr in order to disable logging to a file
    const wchar_t* pathToLogsAndData{};
    //! Optional - Allows resource allocation tracking on the host side
    PFun_ResourceAllocateCallback* allocateCallback{};
    //! Optional - Allows resource deallocation tracking on the host side
    PFun_ResourceReleaseCallback* releaseCallback{};
    //! Optional - Allows log message tracking including critical errors if they occur
    PFun_LogMessageCallback* logMessageCallback{};
    //! Optional - Flags used to enable or disable advanced options
    PreferenceFlags flags = PreferenceFlags::eDisableCLStateTracking | PreferenceFlags::eAllowOTA | PreferenceFlags::eLoadDownloadedPlugins;
    //! Required - Features to load (assuming appropriate plugins are found), if not specified NO features will be loaded by default
    const Feature* featuresToLoad{};
    //! Required - Number of features to load, only used when list is not a null pointer
    uint32_t numFeaturesToLoad{};
    //! Optional - Id provided by NVIDIA, if not specified then engine type and version are required
    uint32_t applicationId{};
    //! Optional - Type of the rendering engine used, if not specified then applicationId is required
    EngineType engine = EngineType::eCustom;
    //! Optional - Version of the rendering engine used
    const char* engineVersion{};
    //! Optional - GUID (like for example 'a0f57b54-1daf-4934-90ae-c4035c19df04')
    const char* projectId{};
    //! Optional - Which rendering API host is planning to use
    //! 
    //! NOTE: To ensure correct `slGetFeatureRequirements` behavior please specify if planning to use Vulkan.
    RenderAPI renderAPI = RenderAPI::eD3D12;
SL_STRUCT_END()
```

##### 2.2.2.1 MANAGING FEATURES

SL will only load features specified via the `Preferences::featuresToLoad` (assuming they are supported on user's system). ***If this parameter is empty, no features will be loaded.*** For example, to load only DLSS, one can do the following:

```cpp
Preferences pref;
Feature myFeatures[] = { sl::kFeatureDLSS };
pref.featuresToLoad = myFeatures;
pref.numFeaturesToLoad = _countof(myFeatures);
// Set other preferences here ...
```

> **IMPORTANT:**
> If SL plugins are not located next to the host executable, then the absolute paths to locations where to look for them must be specified by setting the `pathsToPlugins` field in the `Preferences` structure. Plugins will be loaded from the first path where they can be found.

##### 2.2.2.2 LOGGING AND DEBUG CONSOLE WINDOW

SL provides different levels of logging and can also show a debug console window if requested via the `Preferences` structure. The host can also specify the
location where logs should be saved and track all messages via special `logMessagesCallback` including any errors or warnings.

```cpp
//! Different levels for logging
enum class LogLevel
{
    //! No logging
    eOff,
    //! Default logging
    eDefault,
    //! Verbose logging
    eVerbose,
    //! Total count
    eCount
};

//! Log type
enum class LogType
{
    //! Controlled by LogLevel, SL can show more information in eLogLevelVerbose mode
    eInfo,
    //! Always shown regardless of LogLevel
    eWarn,
    //! Always shown regardless of LogLevel
    eError,
    //! Total count
    eCount
};

//! Logging callback
//!
//! Use these callbacks to track messages posted in the log.
//! If any of the SL methods returns false, check for 
//! eLogTypeError or eLogTypeWarn messages to track down what went wrong and why.
using PFun_LogMessageCallback = void(LogType type, const char *msg);
```

> **NOTE:**
> If a logging callback is specified then SL will not use `OutputDebugString` debug API.

##### 2.2.2.3 MEMORY MANAGEMENT

SL can hand over the control over resource allocation and de-allocation, if requested. When specified, the following callbacks on the host
side will be **fully responsible for the resource allocation and destruction**.

```cpp
//! Resource allocate information
//!
SL_STRUCT_BEGIN(ResourceAllocationDesc, StructType({ 0xbb57e5, 0x49a2, 0x4c23, { 0xa5, 0x19, 0xab, 0x92, 0x86, 0xe7, 0x40, 0x14 } }))
    ResourceAllocationDesc(ResourceType _type, void* _desc, uint32_t _state, void* _heap) : BaseStructure(ResourceAllocationDesc::s_structType), type(_type),desc(_desc),state(_state),heap(_heap){};
    //! Indicates the type of resource
    ResourceType type = ResourceType::eTex2d;
    //! D3D12_RESOURCE_DESC/VkImageCreateInfo/VkBufferCreateInfo
    void* desc{};
    //! Initial state as D3D12_RESOURCE_STATES or VkMemoryPropertyFlags
    uint32_t state = 0;
    //! CD3DX12_HEAP_PROPERTIES or nullptr
    void* heap{};
SL_STRUCT_END()

//! Native resource
//! 
//! {3A9D70CF-2418-4B72-8391-13F8721C7261}
SL_STRUCT_BEGIN(Resource, StructType({ 0x3a9d70cf, 0x2418, 0x4b72, { 0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61 } }))
    //! Constructors
    //! 
    //! Resource type, native pointer are MANDATORY always
    //! Resource state is MANDATORY unless using D3D11
    //! Resource view, description etc. are MANDATORY only when using Vulkan
    //! 
    Resource(ResourceType _type, void* _native, void* _mem, void* _view, uint32_t _state = UINT_MAX) : BaseStructure(Resource::s_structType), type(_type), native(_native), memory(_mem), state(_state), view(_view){};
    Resource(ResourceType _type, void* _native, uint32_t _state = UINT_MAX) : BaseStructure(Resource::s_structType), type(_type), native(_native), state(_state) {};

    //! Conversion helpers for D3D
    inline operator ID3D12Resource* () { return reinterpret_cast<ID3D12Resource*>(native); }
    inline operator ID3D11Resource* () { return reinterpret_cast<ID3D11Resource*>(native); }
    inline operator ID3D11Buffer* () { return reinterpret_cast<ID3D11Buffer*>(native); }
    inline operator ID3D11Texture2D* () { return reinterpret_cast<ID3D11Texture2D*>(native); }

    //! Indicates the type of resource
    ResourceType type = ResourceType::eTex2d;
    //! ID3D11Resource/ID3D12Resource/VkBuffer/VkImage
    void* native{};
    //! vkDeviceMemory or nullptr
    void* memory{};
    //! VkImageView/VkBufferView or nullptr
    void* view{};
    //! State as D3D12_RESOURCE_STATES or VkImageLayout
    //! 
    //! IMPORTANT: State is MANDATORY and needs to be correct when tagged resources are actually used.
    //! 
    uint32_t state = UINT_MAX;
    //! Width in pixels
    uint32_t width{};
    //! Height in pixels
    uint32_t height{};
    //! Native format
    uint32_t nativeFormat{};
    //! Number of mip-map levels
    uint32_t mipLevels{};
    //! Number of arrays
    uint32_t arrayLayers{};
    //! Virtual address on GPU (if applicable)
    uint64_t gpuVirtualAddress{};
    //! VkImageCreateFlags
    uint32_t flags;
    //! VkImageUsageFlags
    uint32_t usage{};
    //! Reserved for internal use
    uint32_t reserved{};
SL_STRUCT_END()

//! Resource allocation/deallocation callbacks
//!
//! Use these callbacks to gain full control over 
//! resource life cycle and memory allocation tracking.
//!
//! @param device - Device to be used (vkDevice or ID3D11Device or ID3D12Device)
//!
//! IMPORTANT: Textures must have the pixel shader resource
//! and the unordered access view flags set
using PFun_ResourceAllocateCallback = Resource(const ResourceAllocationDesc* desc, void* device);
using PFun_ResourceReleaseCallback = void(Resource* resource, void* device);
```

> **NOTE:**
> Memory management done by the host is an optional feature and it is NOT required for the SL to work properly. If used, proper care needs to be taken to avoid releasing resources which are still used by the GPU.

> **IMPORTANT:**
> Streamline defers destruction of internal GPU resources by a few frames for safety. These resources are actually released when Streamline performs garbage collection each frame.
>
> - In the default integration path, Streamline’s common plugin calls `presentCommon()` once per frame (and also collects from internal pools). If `presentCommon()` is not invoked, garbage collection will not run and memory can grow across events such as swapchain resize.
> - If you use manual hooking or otherwise bypass the default interposer, you must ensure the common plugin’s `presentCommon()` function is called every frame. This is required for internal bookkeeping and garbage collection.
> - Debug tip: set a breakpoint in the common plugin’s `presentCommon()` function. Verify it is hit each frame during normal rendering and around swapchain resize.

> **If `presentCommon()` is not called:**
> - Ensure Streamline is initialized early in process startup and not disabled by configuration.
> - If you are using manual hooking or a custom integration, wire your frame presentation path so that Streamline’s common plugin executes `presentCommon()` exactly once per rendered frame (typically at the end of the frame). The recommended way is to upgrade your frame presentation interface using `slUpgradeInterface` immediately after creation (e.g., the swapchain on DirectX), as described in the Manual Hooking guide, so Streamline can invoke `presentCommon()`.
> - If your engine has multiple presentation paths (e.g., different backends or windowing modes), make sure all active paths invoke `presentCommon()`.
> - After changes, re-run with a breakpoint in `presentCommon()` to confirm it executes every frame and around window resizes.

#### 2.2.3 INITIALIZATION

To initialize an SDK instance, simply call the following method:

```cpp
//! Initializes the SL module
//!
//! Call this method when the game is initializing. 
//!
//! @param pref Specifies preferred behavior for the SL library (SL will keep a copy)
//! @param sdkVersion Current SDK version
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! This method is NOT thread safe.
SL_API sl::Result slInit(const sl::Preferences &pref, uint64_t sdkVersion = sl::kSDKVersion);
```

> **IMPORTANT:**
> Please make sure to call `slInit` very early, before any DirectX/DXGI/Vulkan API calls are made and check verbose logging for any warnings or errors.

#### 2.2.4 ERROR HANDLING

All SL functions return `sl::Result` which is defined in `sl_result.h`. However, since SL can be used as an interposer **it is not always possible to immediately report all errors or warnings after a specific SL API is invoked**. This is because some functionality can be triggered with some delay or asynchronously when DXGI/D3D/Vulkan APIs are called by the host application. Therefore, the SL SDK provides `Preferences::logMessagesCallback` so that any **asynchronous errors or warnings** can be tracked by placing breakpoints when `eLogTypeError` and/or `eLogTypeWarn` messages are received. Another useful feature is the debug console window, which should be enabled in development by setting `Preferences::showConsole` to true. In the debug console window, each error will be highlighted in red while warnings are highlighted in yellow to make it easier to notice them.

> **NOTE:**
> See [section 2.4](#24-checking-if-a-feature-is-supported) for more details on how to get detailed information if specific feature is not supported or fails to initialize.

#### 2.2.5 OVER THE AIR (OTA) UPDATES

SL allows host application to opt in (default) or out from an OTA. When enabled, SL will look for the latest SL/NGX updates and load newer versions of required feature(s) if available. To enable OTA in your application please set the following `sl::PreferenceFlags` before calling `slInit`:

```cpp
#include <sl.h>

sl::Preferences pref{};
// Inform SL that it is OK to use newer version of SL or NGX (if available)
pref.flags |= PreferenceFlag::eAllowOTA | PreferenceFlag::eLoadDownloadedPlugins;
// Set other preferences, request features etc.
if(SL_FAILED(result, slInit(pref)))
{
    // Handle error, check the logs
}
```

> **IMPORTANT:**
> Allowing OTA makes your application future proof since it will prevent certain features (like for example DLSS or DLSS-G) from failing on new yet unreleased hardware.

### 2.3 CHECKING FEATURE'S REQUIREMENTS

Once SL is initialized it is possible to obtain additional information about a specific `sl::Feature`. This includes but it is not limited to OS and driver requirements, dependencies on other features, current state and errors etc. The following method can be used to retrieve the requirements and :

```cpp
//! Returns feature's requirements
//!
//! Call this method to check what is required to run certain eFeature* (see above).
//! This method must be called after init otherwise it will always return false.
//!
//! @param feature Specifies which feature to check
//! @param requirements Data structure with feature's requirements
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! This method is NOT thread safe.
SL_API sl::Result slGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements);
```

Here is an example:

```cpp
sl::FeatureRequirements requirements{};
if (SL_FAILED(result, slGetFeatureRequirements(sl::kFeatureDLSS, requirements)))
{
    // Feature is not requested on slInit or failed to load, check logs, handle error
}
else
{
    // Feature is loaded, we can check the requirements    
    requirements.flags & FeatureRequirementFlags::eD3D11Supported
    requirements.flags & FeatureRequirementFlags::eD3D12Supported
    requirements.flags & FeatureRequirementFlags::eVulkanSupported
    requirements.maxNumViewports
}
```

> **NOTE:**
> When using Vulkan this method returns required instance and device extensions together with required 1.2 and 1.3 features and number of additional (if any) graphics or compute queues which need to be enabled by the host application.

### 2.4 CHECKING IF A FEATURE IS SUPPORTED

SL provides a unique id for each supported feature, core feature bits are declared in `sl.h`:

```cpp
//! Features supported with this SDK
//! 
//! IMPORTANT: Each feature must use a unique id
//! 
using Feature = uint32_t;

// ImGUI 
constexpr Feature kFeatureImGUI = 9999;
//! Common feature, NOT intended to be used directly
constexpr Feature kFeatureCommon = UINT_MAX;

```

The set of features will change over time, so please check the specific headers`sl_$feature.h` for the list of currently supported features and to find out what their unique ids are.

To check if a specific feature is available on the specific display adapter(s), you can call:

```cpp
//! Checks if a specific feature is supported or not.
//!
//! Call this method to check if a certain e* (see above) is available.
//!
//! @param feature Specifies which feature to use
//! @param adapterInfo Adapter to check (optional)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! NOTE: If adapter info is null SL will return general feature compatibility with the OS
//! installed drivers or any other requirements not directly related to the adapter.
//! 
//! This method is NOT thread safe.
SL_API sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo);
```

Here is some sample code showing how to check if feature is supported in general (driver, OS etc.) before even selecting an adapter:

```cpp
// We are using NULL adapter on purpose
sl::AdapterInfo adapterInfo{};
if (SL_FAILED(result, slIsFeatureSupported(sl::Feature::eDLSS, adapterInfo)))
{
    // Requested feature is not supported, let's see why
    switch (result)
    {
        case sl::Result::eErrorOSOutOfDate:              // inform user to update OS
        case sl::Result::eErrorDriverOutOfDate:          // inform user to update driver
        case sl::Result::eErrorNoSupportedAdapterFound:  // cannot use any available adapter
        // and so on ...
    };
}
else
{
    // Feature is supported on at least one adapter so now we need to figure out which one before we create our device.
}    

```

Here is another code sample showing how to use the adapter information to determine on which adapter a device should be created in order for the DLSS feature to work correctly:

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
                // Requested feature is not supported on this adapter, let's see why
                switch (result)
                {
                    case sl::Result::eErrorOSOutOfDate:             // inform user to update OS
                    case sl::Result::eErrorDriverOutOfDate:         // inform user to update driver
                    case sl::Result::eErrorNoSupportedAdapterFound: // cannot use any available adapter
                    case sl::Result::eErrorAdapterNotSupported:     // cannot use this specific adapter (older or non-NVDA GPU etc)
                    // and so on ...
                };
            }
            else
            {
                // Feature is supported on this adapter so it is safe to create a device on it.
            }
        }
        i++;
    }
}
```

> NOTE:
> When using Vulkan one can specify `VkPhysicalDevice` instead of LUID in the `sl::AdapterInfo` structure.

### 2.5 HOW TO PROVIDE CORRECT DEVICE

Applications can create multiple devices but SL only works with one. After the main D3D device has been created **the following API MUST be used** to inform SL about the device:

```cpp
//! Set D3D device to use
//! 
//! Use this method to specify which D3D device should be used.
//! 
//! @param d3dDevice D3D device to use
//! 
//!
//! This method is NOT thread safe and should be called IMMEDIATELY after base interface is created.
SL_API sl::Result slSetD3DDevice(void* d3dDevice);
```

>**IMPORTANT**
> When using d3d11 it is important to note that calling `D3D11CreateDeviceAndSwapChain` will result in that device being automatically assigned to SL. If that is not a desired behavior please use `D3D11CreateDevice` and create swap-chain independently through DXGI factory.

When using Vulkan there are two choices:

* Use `vkCreateInstance` and `vkCreateDevice` proxies provided by SL which will take care of all the extensions, features and command queues required by enabled SL features.
* Use native `vkCreateInstance` and `vkCreateDevice` and then manually setup all extensions, features and command queues required by enabled SL features.

If host is using the second option then  **the following API MUST be used** to inform SL about the Vulkan specific information:

```cpp
//! Specify Vulkan specific information
//! 
//! Use this method to provide Vulkan device, instance information to SL.
//! 
//! @param info Reference to the structure providing the information
//! 
//! This method is NOT thread safe and should be called IMMEDIATELY after base interface is created.
SL_API bool slSetVulkanInfo(const sl::VulkanInfo& info);
```

> NOTE:
> 1. For more details regarding the `slSetVulkanInfo` please see [manual hooking programming guide](./ProgrammingGuideManualHooking.md)
> 2. If the host application intends to view names of SL and NGX feature resources (e.g. DLSS textures and buffers) and SL and NGX profiling markers in tools such as Nsight, enable the `VK_EXT_debug_utils` instance extension when creating its `VkInstance`.

### 2.6 HOW TO CHECK FEATURE'S VERSION

To obtain SL and NGX (if any) version for a given feature use the following method:

```cpp

//! Specifies feature's version
//! 
//! {6D5B51F0-076B-486D-9995-5A561043F5C1}
SL_STRUCT_BEGIN(FeatureVersion, StructType({ 0x6d5b51f0, 0x76b, 0x486d, { 0x99, 0x95, 0x5a, 0x56, 0x10, 0x43, 0xf5, 0xc1 } }))
    //! SL version
    Version versionSL{};
    //! NGX version (if feature is using NGX, null otherwise)
    Version versionNGX{};
SL_STRUCT_END()

//! Returns feature's version
//!
//! Call this method to check version for a certain eFeature* (see above).
//! This method must be called after slInit otherwise it will always return an error.
//!
//! @param feature Specifies which feature to check
//! @param version Data structure with feature's version
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! This method is thread safe.
SL_API sl::Result slGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version);
```

### 2.7 ViewportHandle

Many streamline functions use sl::ViewportHandle data structure to let the caller identify the viewport with which they are working.

sl::ViewportHandle is essentially an unique integer number which application has to choose. We don't provide any function to create ViewportHandle.

An example of how application can do that is:

```cpp
sl::ViewportHandle m_viewport = {123};
```

### 2.8 EXPLICIT RESOURCE ALLOCATION AND DE-ALLOCATION

By default SL performs the so called `lazy` initialization and destruction of all resources - in other words resources are created only when used and destroyed when plugins are unloaded. If required, an explicit allocation or de-allocation of internal SL resources is also possible using the following methods:

```cpp
//! Allocates resources for the specified feature.
//!
//! Call this method to explicitly allocate resources
//! for an instance of the specified feature.
//! 
//! @param cmdBuffer Command buffer to use (must be created on device where feature is supported but can be null if not needed)
//! @param feature Feature we are working with
//! @param viewport Unique id (viewport handle)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! This method is NOT thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport);

//! Frees resources for the specified feature.
//!
//! Call this method to explicitly free resources
//! for an instance of the specified feature.
//! 
//! @param feature Feature we are working with
//! @param viewport Unique id (viewport handle)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! IMPORTANT: If slEvaluateFeature is pending on a command list, that command list must be flushed
//! before calling this method to prevent invalid resource access on the GPU.
//!
//! This method is NOT thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport);
```

> **IMPORTANT**
> If there is a pending `slEvaluateFeature` call on a command list, that specific command list must be flushed before calling `slFreeResources` to avoid potential crash on the GPU.

For example, let's assume we have two viewports using `sl::kFeatureDLSS` and we want to manage SL resource allocation explicitly - here is some sample code:

```cpp
// Viewport1
{
    // We need to setup our constants first so sl.dlss plugin has enough information
    sl::DLSSOptions dlssOptions = {};
    dlssOptions.mode = viewport1->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
    dlssOptions.outputWidth = viewport1->getOutputWidth();    // e.g 1920;
    dlssOptions.outputHeight = viewport1->getOutputHeight(); // e.g. 1080;
    // Note that we are passing viewport handle
    slDLSSSetOptions(viewport1->handle, dlssOptions);
    
    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDLSS, viewport1->handle);

    // Evaluate DLSS on viewport1, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DLSS resources would be initialized at this point
    auto inputs[] = {&viewport1->handle};
    slEvaluate(sl::kFeatureDLSS, myFrameToken, inputs, _countof(inputs), myCmdList)

    // Make sure resources are no longer used by the GPU
    flush(myCmdList);

    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDLSS, viewport1->handle);
}

// Viewport2
{
    // We need to setup our constants first so sl.dlss plugin has enough information
    sl::DLSSOptions dlssOptions = {};
    dlssOptions.mode = viewport2->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
    dlssOptions.outputWidth = viewport2->getOutputWidth();    // e.g 1920;
    dlssOptions.outputHeight = viewport2->getOutputHeight(); // e.g. 1080;
    slDLSSSetOptions(viewport2->handle, dlssOptions);
    
    // Now we can allocate our feature explicitly, again passing viewport id
    slAllocateResources(sl::kFeatureDLSS, viewport2->handle);
    
    // Evaluate DLSS on viewport2, again passing viewport id so we can map tags, constants correctly
    //
    // NOTE: If slAllocateResources is not called DLSS resources would be initialized at this point
    auto inputs[] = {&viewport2->handle};
    slEvaluate(sl::kFeatureDLSS, myFrameToken, inputs, _countof(inputs), myCmdList)

    // Make sure resources are no longer used by the GPU
    flush(myCmdList);
    
    // When we no longer need this viewport
    slFreeResources(sl::kFeatureDLSS, viewport2->handle);
}

```

### 2.9 TAGGING RESOURCES

API 'slSetTag' is deprecated and is no longer recommended to be used. A new API `slSetTagForFrame` has been introduced passing in frame-token to tag resources for a specific frame, i.e. associated with a frame index facilitating tagging for multiple frames in flight on CPU at the same time. This deprecates the API 'slSetTag' and cannot be used with the new API `slSetTagForFrame`. sl::PreferenceFlags flag `eUseFrameBasedResourceTagging` must be set to enforce frame-based resource tagging using SL APIs `slSetTagForFrame` and `slEvaluateFeature`.

The appropriate D3D11/D3D12/VK resources should be tagged globally using the `slSetTagForFrame` or locally with the `slEvaluateFeature` API and the corresponding `BufferType` unique id.  Note that the list below may not accurately represent the full set of optional and even required items in a given version's header.  The Programming Guide for each individual plugin/feature will discuss which of these tags are required and/or used by that feature.

Note that using that same command list during resource-tagging for multiple frames at the same time is at developer's discretion but correctness of operations can't be guaranteed, even if D3D12 supports cocnurrent CL recording and submission.

```cpp
//! Buffer types used for tagging
//! Depth buffer - IMPORTANT - Must be suitable to use with clipToPrevClip transformation (see Constants below)
constexpr BufferType kBufferTypeDepth = 0;
//! Object and optional camera motion vectors (see Constants below)
constexpr BufferType kBufferTypeMotionVectors = 1;
//! Color buffer with all post-processing effects applied but without any UI/HUD elements
constexpr BufferType kBufferTypeHUDLessColor = 2;
//! Color buffer containing jittered input data for the image scaling pass
constexpr BufferType kBufferTypeScalingInputColor = 3;
//! Color buffer containing results from the image scaling pass
constexpr BufferType kBufferTypeScalingOutputColor = 4;
//! Normals
constexpr BufferType kBufferTypeNormals = 5;
//! Roughness
constexpr BufferType kBufferTypeRoughness = 6;
//! Albedo
constexpr BufferType kBufferTypeAlbedo = 7;
//! Specular Albedo
constexpr BufferType kBufferTypeSpecularAlbedo = 8;
//! Indirect Albedo
constexpr BufferType kBufferTypeIndirectAlbedo = 9;
//! Specular Motion Vectors
constexpr BufferType kBufferTypeSpecularMotionVectors = 10;
//! Disocclusion Mask
constexpr BufferType kBufferTypeDisocclusionMask = 11;
//! Emissive
constexpr BufferType kBufferTypeEmissive = 12;
//! Exposure
constexpr BufferType kBufferTypeExposure = 13;
//! Buffer with normal and roughness in alpha channel
constexpr BufferType kBufferTypeNormalRoughness = 14;
//! Diffuse and camera ray length
constexpr BufferType kBufferTypeDiffuseHitNoisy = 15;
//! Diffuse denoised
constexpr BufferType kBufferTypeDiffuseHitDenoised = 16;
//! Specular and reflected ray length
constexpr BufferType kBufferTypeSpecularHitNoisy = 17;
//! Specular denoised
constexpr BufferType kBufferTypeSpecularHitDenoised = 18;
//! Shadow noisy
constexpr BufferType kBufferTypeShadowNoisy = 19;
//! Shadow denoised
constexpr BufferType kBufferTypeShadowDenoised = 20;
//! AO noisy
constexpr BufferType kBufferTypeAmbientOcclusionNoisy = 21;
//! AO denoised
constexpr BufferType kBufferTypeAmbientOcclusionDenoised = 22;
//! Optional - UI/HUD color and alpha
//! IMPORTANT: Please make sure that alpha channel has enough precision (for example do NOT use formats like R10G10B10A2)
constexpr BufferType kBufferTypeUIColorAndAlpha = 23;
//! Optional - Shadow pixels hint (set to 1 if a pixel belongs to the shadow area, 0 otherwise)
constexpr BufferType kBufferTypeShadowHint = 24;
//! Optional - Reflection pixels hint (set to 1 if a pixel belongs to the reflection area, 0 otherwise)
constexpr BufferType kBufferTypeReflectionHint = 25;
//! Optional - Particle pixels hint (set to 1 if a pixel represents a particle, 0 otherwise)
constexpr BufferType kBufferTypeParticleHint = 26;
//! Optional - Transparency pixels hint (set to 1 if a pixel belongs to the transparent area, 0 otherwise)
constexpr BufferType kBufferTypeTransparencyHint = 27;
//! Optional - Animated texture pixels hint (set to 1 if a pixel belongs to the animated texture area, 0 otherwise)
constexpr BufferType kBufferTypeAnimatedTextureHint = 28;
//! Optional - Bias for current color vs history hint - lerp(history, current, bias) (set to 1 to completely reject history)
constexpr BufferType kBufferTypeBiasCurrentColorHint = 29;
//! Optional - Ray-tracing distance (camera ray length)
constexpr BufferType kBufferTypeRaytracingDistance = 30;
//! Optional - Motion vectors for reflections
constexpr BufferType kBufferTypeReflectionMotionVectors = 31;
//! Optional - Position, in same space as eNormals
constexpr BufferType kBufferTypePosition = 32;
//! Optional - Indicates (via non-zero value) which pixels have motion/depth values that do not match the final color content at that pixel (e.g. overlaid, opaque Picture-in-Picture)
constexpr BufferType kBufferTypeInvalidDepthMotionHint = 33;
//! Alpha
constexpr BufferType kBufferTypeAlpha = 34;
//! Color buffer containing only opaque geometry
constexpr BufferType kBufferTypeOpaqueColor = 35;
//! Optional - Reduce reliance on history instead using current frame hint (0 if a pixel is not at all reactive and default composition should be used, 1 if fully reactive)
constexpr BufferType kBufferTypeReactiveMaskHint = 36;
//! Optional - Pixel lock adjustment hint (set to 1 if pixel lock should be completely removed, 0 otherwise)
constexpr BufferType kBufferTypeTransparencyAndCompositionMaskHint = 37;

//! Specifies life-cycle for the tagged resource
//!
enum ResourceLifecycle
{
    //! Resource can change, get destroyed or reused for other purposes after it is provided to SL
    //! 
    //! IMPORTANT: Use only when really needed since it can result in wasting VRAM if SL ends up making unnecessary copies.
    eOnlyValidNow,
    //! Resource does NOT change, gets destroyed or reused for other purposes from the moment it is provided to SL until the frame is presented
    eValidUntilPresent,
    //! Resource does NOT change, gets destroyed or reused for other purposes from the moment it is provided to SL until after the slEvaluateFeature call has returned.
    eValidUntilEvaluate
};

//! NOTE: sl::PreferenceFlags::eUseFrameBasedResourceTagging must be set when using this API.
//! Tags resource globally
//!
//! Call this method to tag the appropriate buffers in global scope.
//!
//! @param frame Specifies the frame this tag applies to. Frame token can be obtained using slGetNewFrameToken API.
//! @param viewport Specifies viewport this tag applies to
//! @param tags Pointer to resources tags, set to null to remove the specified tag
//! @param numTags Number of resource tags in the provided list
//! @param cmdBuffer Command buffer to use (optional and can be null if ALL tags are null or have eValidUntilPresent life-cycle)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! IMPORTANT: GPU payload that generates content for the provided tag(s) MUST be either already submitted to the provided command buffer 
//! or some other command buffer which is guaranteed, by the host application, to be executed BEFORE the provided command buffer.
//! 
//! This method is thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* resources, uint32_t numResources, sl::CommandBuffer* cmdBuffer);

//! NOTE: Following API has now been DEPRECATED in favor of the new slSetTagForFrame API above.
//! Tags resource globally
//!
//! Call this method to tag the appropriate buffers in global scope.
//!
//! @param viewport Specifies viewport this tag applies to
//! @param tags Pointer to resources tags, set to null to remove the specified tag
//! @param numTags Number of resource tags in the provided list
//! @param cmdBuffer Command buffer to use (optional and can be null if ALL tags are null or have eValidUntilPresent life-cycle)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! IMPORTANT: GPU payload that generates content for the provided tag(s) MUST be either already submitted to the provided command buffer 
//! or some other command buffer which is guaranteed, by the host application, to be executed BEFORE the provided command buffer.
//! 
//! This method is thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);
```

> **IMPORTANT:**
> GPU payload that generates content for any tagged resource MUST be either already submitted to the provided command list or some other command list which is guaranteed to be executed BEFORE.

Please note that **correct resource state MUST be provided by the host** for all tagged resources. For the volatile resources this would be their state on the command list which is passed to `slSetTagForFrame` and for the immutable resources it should be the state when resource is used by SL. 
If state of an immutable resource can change from use case to use case then resource should be tagged multiple times or passed as input to `slEvaluateFeature` with the appropriate state. Here is an example:

```cpp

sl::Preferences pref{};
// Inform SL that it is doing frame-based resource tagging.
pref.flags |= PreferenceFlag::eUseFrameBasedResourceTagging;
// Set other preferences, request features etc.
if(SL_FAILED(result, slInit(pref)))
{
    // Handle error, check the logs
}

// IMPORTANT: If using d3d11 set all resource states to 0
if(d3d11)
{    
    depthImmutableState = 0; // D3D12_RESOURCE_STATE_COMMON
    mvecImmutableState = 0; // D3D12_RESOURCE_STATE_COMMON
}

//! IMPORTANT: Tagging as immutable, resource can NOT be reused, changed or deleted after it is tagged. 

sl::Resource depth = sl::Resource{ sl::ResourceType::eTex2d, myNativeObject, nullptr, nullptr, depthImmutableState};
sl::Resource mvec = sl::Resource{ sl::ResourceType::eTex2d, myNativeObject, nullptr, nullptr, mvecImmutableState};
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };
sl::ResourceTag mvecTag = sl::ResourceTag {&mvec, sl::kBufferTypeMvec, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

sl::FrameToken* currentFrame{};
if(SL_FAILED(result, slGetNewFrameToken(currentFrame)))
{
    // Handle error, check the logs
}

// Tag as a group for simplicity
sl::ResourceTag inputs[] = {depthTag, mvecTag};
if(SL_FAILED(result, slSetTagForFrame(*currentFrame, viewport, inputs, _countof(inputs), cmdList)))
{
   // Handle error, check the logs
}
```

> **NOTE:**
* When using d3d11 please make sure to use state 0 (D3D12_RESOURCE_STATE_COMMON) for all tags.
* When using Vulkan, ensure to specify aspect mask of the image view member of depth buffer `sl::Resource`, containing both depth and stencil bits in its format, to be of type depth only, during its tagging. This is because SL features use depth data from depth-stencil buffer and as per [VUID-VkDescriptorImageInfo-imageView-01976](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkDescriptorImageInfo.html#VUID-VkDescriptorImageInfo-imageView-01976), if the Vulkan image view is being used for texturing, then its aspect mask can either be of type depth or stencil but not both.

Instead of using global scope and `slSetTagForFrame`, resources can also be tagged in local scope by passing them in directly when calling `slEvaluateFeature` if that is more convenient, here is an example:

```cpp
// Make sure DLSS is available and user selected this option in the UI
if(useDLSS) 
{
    //! We can also add all tags here
    //!
    //! NOTE: These are considered local tags and will NOT impact any tags set in global scope.
    const sl::BaseStructure* inputs[] = {&myViewport, &depthTag, &mvecTag, &colorInTag, &colorOutTag};
    if(SL_FAILED(result, slEvaluateFeature(sl::Feature::eDLSS, currentFrameToken, inputs, _countof(inputs), myCmdList)))
    {
        // Handle error, check the logs
    }
    else
    {
        // IMPORTANT: Host is responsible for restoring state on the command list used
        restoreState(myCmdList);
    }    
}
else
{
    // Default up-scaling pass like, for example, TAAU goes here
}
```

> **NOTE:**
> Tagging resources locally also allows one to use different state

If resource is tagged as `sl::ResourceLifecycle::eOnlyValidNow` SL will **make a copy but ONLY if any active feature needs that resource later on**. Good examples are depth and motion vector buffers which are needed by DLSS-G at the end of the frame:

```cpp
//! IMPORTANT: Tagging as volatile, resource can be reused, changed or even deleted after it is tagged. 
//!
//! Passing state which is valid on the command list we are using with slSetTagForFrame

sl::Resource depth = sl::Resource{ sl::ResourceType::eTex2d, myNativeDepth, nullptr, nullptr, depthStateOnCmdList};
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &depthExtent };
// SL will make a copy but ONLY if DLSS-G or any other feature is active and needs this tag later on
if(SL_FAILED(result, slSetTagForFrame(*currentFrame, viewport, &depthTag, 1, cmdList)))
{
    // Handle error, check the logs
}
```
> **IMPORTANT**
> Use `sl::ResourceLifecycle::eOnlyValidNow` ONLY if that is absolutely necessary, overuse of this flag can result in wasted VRAM.

If resource is tagged as `sl::ResourceLifecycle::eValidUntilPresent` or `sl::ResourceLifecycle::eValidUntilEvaluate` but its state is different when used in the evaluate call vs when used later on (like for example depth buffer being used on present by DLSS-G) one can tag resource several times with different state, here is how:

```cpp
// DLSS

// NOTE: Showing only depth tag for simplicity
//
// LOCAL tagging for the evaluate call, using state which is valid now on the command list we are using to evaluate DLSS
sl::Resource depth = sl::Resource{ sl::ResourceType::eTex2d, myNativeObject, nullptr, nullptr, depthStateOnEval};
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &depthExtent };
const sl::BaseStructure* inputs[] = {&myViewport, &depthTag};
if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS, currentFrameToken, inputs, _countof(inputs), myCmdList)))
{
    // Handle error, check the logs
}
else
{
    // IMPORTANT: Host is responsible for restoring state on the command list used
    restoreState(myCmdList);
}

// DLSS-G

// Now we tag depth GLOBALLY with state which is valid when frame present is called
sl::Resource depth = sl::Resource{ sl::ResourceType::eTex2d, myNativeObject, nullptr, nullptr, depthStateOnPresent};
sl::ResourceTag depthTag = sl::ResourceTag {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &depthExtent };
if(SL_FAILED(result, slSetTagForFrame(*currentFrame, viewport, &depthTag, 1, myCmdList)))
{
    // Handle error, check the logs
}
```

When resource tag of type `sl::ResourceLifecycle::eValidUntilPresent` is no longer needed (e.g. resource is about to be destroyed) host app must set the tag to null in order to release the reference held by SL:

```cpp
// Set resource to null to release references
sl::ResourceTag depthTag = sl::ResourceTag {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent};
// No need for command list when tags are null
slSetTagForFrame(*currentFrame, viewport, &depthTag, 1);
```

> **NOTE:**
> There is no need to transition tagged resources to any specific state. When used by the host application resources will always be in the state they where left before invoking any SL APIs.

For the complete list of the required buffer tags for a given feature please call `slGetFeatureRequirements`.

### Resource extensions
Some resource extensions can be supplied to `ResourceTag` to pass extra information. Extensions can be supplied in 2 ways:
* **Member variable**: Member variable to the `ResourceTag`, with the appropriate struct version number
* **Chain extension**: As part of the `ResouceTag` chain, in its `next` pointer member variable

>**NOTE:**
Chain extensions should all come immediately after the tag they belong to.

#### Resource `Extent` (member variable, `kStructVersion1`)
When a resource is suballocated as part of a bigger resource, please properly populate the `Extent` member variable as part of the `ResourceTag`. The `Extent` should indicate the top-left corner of the subresource relative to the tagged resource, as well as the width and height of the subresource.

```cpp

// Example extent: resource that starts in the middle of the tagged resource, and is at 1/8th of the tagged resource's resolution.
// Illustration: The xxx represent the subresource that the extents define
// -----------------
// -----------------
// -----------------
// --------xxxx-----
// --------xxxx-----
// -----------------
// -----------------

Extent depthExtent{};
depthExtent.top = depthResourceHeight / 2;
depthExtent.left = depthResourceWidth / 2;
depthExtent.height = depthResourceHeight / 4;
depthExtent.width= depthResourceWidth / 4;

ResourceTag depthTag{};
depthTag.extent = depthExtent;
```
#### Resource `PrecisionInfo` (chain extension)
Some buffers (e.g., `kBufferTypeBidirectionalDistortionField`) can be supplied with a `PrecisionInfo` struct as an extension (`next` ptr) to the `ResourceTag`. This struct allows a plugin to internally convert the low-precision buffer data to a higher-precision format, for better image quality purposes.

Populating the `PrecisionInfo` accurately requires knowing the bounds the high-precision data, so that good `scale` and `bias` values are supplied.

```cpp
// These max and min values should be computed as part of a reduction phase, and should be used to convert
// the high-precision data to a low-precision format
// The conversion, using a linear transform, could look like the following:
//     lowPrecisionData = (hiPrecisionData - bufferMinVal) / (bufferMaxVal - bufferMinVal)
float bufferMaxVal; // max value in the high-precision format
float bufferMinVal; // min value in the high-precision format

PrecisionInfo bufferPrecisionInfo{};
bufferPrecisionInfo.conversionFormula = PrecisionFormula::eLinearTransform;
bufferPrecisionInfo.bias = bufferMinVal;
bufferPrecisionInfo.scale = bufferMaxVal - bufferMinVal;

ResourceTag tag{};
tag.next = &bufferPrecisionInfo;
```

### 2.10 FRAME TRACKING

SL requires correct frame tracking since host application could be working on multiple frames at the same time and various SL inputs must be matched with the frame that is actually being presented. When the simulation phase for the new frame is starting in the host application the frame tracking handle for that specific frame should be obtained from the SL (this would be the exact same place where normally the `sl::ReflexMarker::eSimulationStart` would be placed, see [Reflex guide](./ProgrammingGuideReflex.md) for more details)).

```cpp
//! Gets unique frame token
//!
//! Call this method to obtain token for the unique frame identification.
//!
//! @param token Frame token to return
//! @param frameIndex Frame index (optional, if not provided SL internal frame counting is used)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//! 
//! This method is NOT thread safe.
SL_API sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex = nullptr);
```

The following pseudo code snippet shows a typical flow in a game engine:

```cpp
while(gameRunning)
{
    sl::FrameToken* currentFrame{};
    if(SL_FAILED(result, slGetNewFrameToken(currentFrame)))
    {
        // Handle error, check the logs
    }

    // Provide SL frame handle in any sl* calls which require it
    doSimulation(currentFrame);

    // Store a copy of the handle and use that in any async tasks
    startThread[currentFrame]()
    {
        // Provide SL frame handle in any sl* calls which require it
        asyncRendering(currentFrame);

        // Provide SL frame handle in any sl* calls which require it
        asyncPresentFrame(currentFrame);
    };    
}
```

> NOTE:
> If the host can reliably and correctly track the frame index then that same index can be provided as input to the `slGetNewFrameToken` which would bypass internal SL frame counting.

#### 2.9.1 ADVANCED FRAME TRACKING

Sometimes there is a need to obtain previous or next frame token. This could be needed, for example, when sending ping markers with `sl.reflex`. Here is an example showing how to obtain the previous and the next frame tokens:

```cpp
sl::FrameToken* prevFrame;
uint32_t prevIndex = *currentFrame - 1;
slGetNewFrameToken(prev, &prevIndex); // NOTE: providing optional frame index

sl::FrameToken* nextFrame;
uint32_t nextIndex = *currentFrame + 1;
slGetNewFrameToken(next, &nextIndex); // NOTE: providing optional frame index
```

### 2.11 PROVIDING ADDITIONAL INFORMATION

#### 2.11.1 COMMON CONSTANTS

Some additional information should be provided so that SL features can operate correctly. Please use `slSetConstants` to provide the required data ***as early in the frame as possible*** and make sure to set values for all fields in the following structure:

```cpp
//! Common constants, all parameters must be provided unless they are marked as optional
//! 
//! {DCD35AD7-4E4A-4BAD-A90C-E0C49EB23AFE}
SL_STRUCT_BEGIN(Constants, StructType({ 0xdcd35ad7, 0x4e4a, 0x4bad, { 0xa9, 0xc, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe } }))
    //! IMPORTANT: All matrices are row major (see float4x4 definition) and
    //! must NOT contain temporal AA jitter offset (if any). Any jitter offset
    //! should be provided as the additional parameter Constants::jitterOffset (see below)
            
    //! Specifies matrix transformation from the camera view to the clip space.
    float4x4 cameraViewToClip;
    //! Specifies matrix transformation from the clip space to the camera view space.
    float4x4 clipToCameraView;
    //! Optional - Specifies matrix transformation describing lens distortion in clip space.
    float4x4 clipToLensClip;
    //! Specifies matrix transformation from the current clip to the previous clip space.
    //! clipToPrevClip = clipToView * viewToViewPrev * viewToClipPrev
    //! Sample code can be found in sl_matrix_helpers.h
    float4x4 clipToPrevClip;
    //! Specifies matrix transformation from the previous clip to the current clip space.
    //! prevClipToClip = clipToPrevClip.inverse()
    float4x4 prevClipToClip;
        
    //! Specifies pixel space jitter offset
    float2 jitterOffset;
    //! Specifies scale factors used to normalize motion vectors (so the values are in [-1,1] range)
    float2 mvecScale;
    //! Optional - Specifies camera pinhole offset if used.
    float2 cameraPinholeOffset;
    //! Specifies camera position in world space.
    float3 cameraPos;
    //! Specifies camera up vector in world space.
    float3 cameraUp;
    //! Specifies camera right vector in world space.
    float3 cameraRight;
    //! Specifies camera forward vector in world space.
    float3 cameraFwd;
        
    //! Specifies camera near view plane distance.
    float cameraNear = INVALID_FLOAT;
    //! Specifies camera far view plane distance.
    float cameraFar = INVALID_FLOAT;
    //! Specifies camera field of view in radians.
    float cameraFOV = INVALID_FLOAT;
    //! Specifies camera aspect ratio defined as view space width divided by height.
    float cameraAspectRatio = INVALID_FLOAT;
    //! Specifies which value represents an invalid (un-initialized) value in the motion vectors buffer
    //! NOTE: This is only required if `cameraMotionIncluded` is set to false and SL needs to compute it.
    float motionVectorsInvalidValue = INVALID_FLOAT;

    //! Specifies if depth values are inverted (value closer to the camera is higher) or not.
    Boolean depthInverted = Boolean::eInvalid;
    //! Specifies if camera motion is included in the MVec buffer.
    Boolean cameraMotionIncluded = Boolean::eInvalid;
    //! Specifies if motion vectors are 3D or not.
    Boolean motionVectors3D = Boolean::eInvalid;
    //! Specifies if previous frame has no connection to the current one (i.e. motion vectors are invalid)
    Boolean reset = Boolean::eInvalid;
    //! Specifies if application is currently rendering game frames (paused in menu, playing video cut-scenes)
    Boolean renderingGameFrames = Boolean::eInvalid;
    //! Specifies if orthographic projection is used or not.
    Boolean orthographicProjection = Boolean::eFalse;
    //! Specifies if motion vectors are already dilated or not.
    Boolean motionVectorsDilated = Boolean::eFalse;
    //! Specifies if motion vectors are jittered or not.
    Boolean motionVectorsJittered = Boolean::eFalse;
SL_STRUCT_END()

//! Sets common constants.
//!
//! Call this method to provide the required data (SL will keep a copy).
//!
//! @param values Common constants required by SL plugins (SL will keep a copy)
//! @param frame Index of the current frame
//! @param viewport Unique id (can be viewport id | instance id etc.)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//! 
//! This method is thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport);
```

> **NOTE:**
> Provided projection related matrices `should not` contain any clip space jitter offset. Jitter offset (if any) should be specified as a separate `float2` constant.

#### 2.11.2 FEATURE SPECIFIC OPTIONS

Each feature requires specific data which, together with the accompanying API, is defined in a corresponding  `sl_<feature_name>.h` header file (e.g. `sl_dlss.h`, etc.). Here is an example on how to set DLSS options:

```cpp
#include <sl_dlss.h>

sl::DLSSOptions dlssOptions = {};
// These are populated based on user selection in the UI
dlssOptions.mode = myUI->getDLSSMode(); // e.g. sl::eDLSSModeBalanced;
dlssOptions.outputWidth = myUI->getOutputWidth();    // e.g 1920;
dlssOptions.outputHeight = myUI->getOutputHeight(); // e.g. 1080;
dlssOptions.sharpness = dlssSettings.sharpness; // optimal sharpness
dlssOptions.colorBuffersHDR = sl::Boolean::eTrue; // assuming HDR pipeline
// Setting options for a specific viewport
if(SL_FAILED(result, slDLSSSetOptions(viewport, dlssOptions)))
{
    // Handle error here, check the logs
}
```

> **NOTE:**
> To switch off any given feature, simply set its mode to the `off` state. This will also unload any resources used by this feature (if it was ever invoked before). For example, to disable DLSS one would set `DLSSConstants::mode = eDLSSModeOff`.

### 2.12 FEATURE SETTINGS AND STATES

Some features provide feedback to the host application specifying, for example, which rendering settings are optimal or preferred or their current state (memory consumption etc). Feature specific APIs to obtain settings and states are defined in a corresponding  `sl_<feature_name>.h` header file

For example, when using DLSS, it is mandatory to call `slDLSSGetOptimalSettings` to find out at which resolution we should render your game:

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

> **NOTE:**
> Not all features provide specific/optimal settings. Please refer to the appropriate, feature specific header for more details.

### 2.13 MARKING EVENTS IN THE PIPELINE

By design, SL SDK enables `host assisted replacement or injection of specific rendering features`. In order for SL features to work, specific sections in the rendering pipeline need to be marked with the following method:

```cpp
//! NOTE: sl::PreferenceFlags::eUseFrameBasedResourceTagging must be set when using this API to do
//! frame-based resource tagging for multiple frames in flight at the same time.
//! Evaluates feature
//! 
//! Use this method to mark the section in your rendering pipeline
//! where specific feature should be injected.
//!
//! @param feature Feature we are working with
//! @param frame Current frame handle obtained from SL
//! @param inputs The chained structures providing the input data (viewport, tags, constants etc)
//! @param numInputs Number of inputs
//! @param cmdBuffer Command buffer to use (must be created on device where feature is supported)
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//! 
//! IMPORTANT: Frame and viewport must match whatever is used to set common and or feature options and constants (if any)
//! 
//! NOTE: It is allowed to pass in buffer tags as inputs, they are considered to be a "local" tags and do NOT interact with
//! same tags sent in the global scope using slSetTag API.
//!
//! This method is NOT thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer);
```

Please note that **host is responsible for restoring the command buffer(list) state** after calling `slEvaluate`. For more details on which states are affected please see [restore pipeline section](./ProgrammingGuideManualHooking.md#70-restoring-command-listbuffer-state)

> **NOTE:**
> DLSS-G is a unique case since it already has the marker provided by the existing API (SwapChain::Present) hence there is no need to call `slEvaluateFeature` to enable DLSS-G

Here is some pseudo code showing how the host application can replace the default up-scaling method with DLSS on hardware which supports it:

```cpp
// Make sure DLSS is available and user selected this option in the UI
if(useDLSS) 
{
    // For example, here we assume user selected DLSS balanced mode and 4K resolution with no sharpening
    DLSSOptions options = {eDLSSModeBalanced, 3840, 2160, 0.0}; 
    slDLSSSetOptions(&options, myViewport);    

    // Inform SL that DLSS should be injected at this point for the given viewport
    const sl::BaseStructure* inputs[] = {&myViewport};
    if(SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS, myFrameToken, inputs, _countof(inputs), myCmdList)))
    {
        // Handle error, check the logs
    }
    else
    {
        // IMPORTANT: Host is responsible for restoring state on the command list used
        restoreState(myCmdList);
    }    
}
else
{
    // Default up-scaling pass like, for example, TAAU goes here
}
```

### 2.14 HOW TO LOAD OR UNLOAD A FEATURE (ADVANCED)

All requested features are loaded by default. To explicitly unload a specific feature use the following method:

```cpp
//! Sets the specified feature to either loaded or unloaded state.
//!
//! Call this method to load or unload certain e*. 
//!
//! NOTE: All requested features are loaded by default and have to be unloaded explicitly if needed.
//!
//! @param feature Specifies which feature to check
//! @param loaded Value specifying if feature should be loaded or unloaded.
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! NOTE: When this method is called no other DXGI/D3D/Vulkan APIs should be invoked in parallel so
//! make sure to flush your pipeline before calling this method.
//!
//! This method is NOT thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slSetFeatureLoaded(sl::Feature feature, bool loaded);
```

Unloaded features are **unhooked from the DXGI/DX/VK APIs and are effectively unusable** - they cannot be turned on or off.

> **NOTE:**
> Please make sure to flush your pipeline and do not invoke DXGI/D3D/VULKAN API while this method is running.

You may also query whether a particular feature is currently loaded or not with the following method:

```cpp
//! Checks if specified feature is loaded or not.
//!
//! Call this method to check if feature is loaded.
//! All requested features are loaded by default and have to be unloaded explicitly if needed.
//!
//! @param feature Specifies which feature to check
//! @param loaded Value specifying if feature is loaded or unloaded.
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! This method is NOT thread safe and requires DX/VK device to be created before calling it.
SL_API sl::Result slIsFeatureLoaded(sl::Feature feature, bool& loaded);
```

> **NOTE:**
> If feature is not loaded (requested) via `slInit` it cannot be loaded or unloaded.

> **IMPORTANT** 
> Switching a feature on or off should not be confused with loading or unloading it. `slSetFeatureLoaded` is needed **ONLY in advanced scenarios** when, for example, multiple swap-chains are used and DLSS-G should be enabled just on one of them.

### 2.15 SHUTDOWN

To release the SDK instance and all resources allocated with it, use the following method:

```cpp
//! Shuts down the SL module
//!
//! Call this method when the game is shutting down. 
//!
//! @return sl::ResultCode::eOk if successful, error code otherwise (see sl_result.h for details)
//!
//! This method is NOT thread safe.
SL_API sl::Result slShutdown();
```

> **NOTE:**
> If shutdown is called too early, any SL features which are enabled and running will stop functioning and the host application will fallback to the
default implementation. For example, if DLSS is enabled and running and shutdown is called, the `sl.dlss` plugin will be unloaded, hence any `evaluate` or `slIsFeatureSupported` calls will return an error and the host application should fallback to the default implementation (for example TAAU)

### 2.16 MULTIPLE SWAP CHAINS

The current tagging API in Streamline is designed with a single swap chain in mind.  When a resource is tagged with a life-cycle of `eValidUntilPresent` there is no way to specify on which swap chain the Present() is.

So, if Streamline sees multiple swap chains- it will hook the first one, and it will log a warning for all other swap chains.

If an application needs to create multiple swap chains, it can use [manual hooking](ProgrammingGuideManualHooking.md)- and use `slUpgradeInterface()` to inform Streamline which swap chain it needs to attach to.  That way the warning about multiple swap chains may be avoided.

3 VALIDATING SL INTEGRATION WHEN REPLACING PLATFORM LIBRARIES
-----------------------------

If you are integrating Streamline by replacing the standard libraries with sl.interposer.lib, ensure that `sl.interposer.dll` is the only dependency in your application and no dependency on `dxgi.dll`, `d3d11.dll`, `d3d12.dll` or `vulkan-1.dll` remains.  To do so, please do the following:

* Open a Developer Command Prompt for Visual Studio
* Run "dumpbin /dependents `my engine exe/dll that links sl.interposer.dll`"
* Look for `dxgi.dll`, `d3d11.dll`, `d3d12.dll` or `vulkan-1.dll` in the list of dependents. If you see either of those or if `sl.interposer.dll` is missing in the list then **SL was NOT integrated correctly** in your application.

Note that if you are using a different method of integration where you continue to link to the standard platform libraries, this method does not apply.

4 DISTRIBUTING SL WITH YOUR APPLICATION
-----------------------------

SL SDK comes with several binaries which need to be distributed with your application. Here are the mandatory modules:

* sl.interposer.dll
* sl.common.dll

The remaining modules are optional depending on which features are enabled in your application. For example if DLSS/NIS are used, you need to include:

* sl.dlss.dll, nvngx_dlss.dll
* sl.nis.dll

> **NOTE:**
> If SL binaries are not installed next to the host executable, please make sure to specify the absolute paths to look for them using the `Preferences`

5 COMMON ISSUES AND HOW TO RESOLVE THEM
------------------------------------------------

* If Steam overlay is not showing up please ensure that SteamAPI_Init() is called **before** any D3D or DXGI calls (device create, factory create etc.)
* If D3D debug layer is complaining about incorrect resource states please provide correct state when tagging SL resources (see sl::Resource structure)
* If you get a crash in Swapchain::Present or some similar unexpected behavior please double check that you are NOT linking dxgi.lib/d3d12.lib together with the sl.interposer.dll. See [section 3](#3-validating-sl-integration-when-replacing-platform-libraries) in this guide.
* If SL does not work correctly make sure that some other library which includes `dxgi` or `d3d11/12` is not linked in your application (like for example `WindowsApp.lib`)
* Third party overlays can disable SL interposer, always make sure to call `slSetD3DDevice` or `slSetVulkanInfo`
* Make sure that all matrices are multiplied in the correct order and provided in row-major order **without any jitter**
* Motion vector scaling factors in `sl::Constants` transform values from `eMvec` to the {-1,1} range
* Jitter offset should be provided in pixel space

### 5.1 SL AND THIRD PARTY SDKs

If your application is using 3rd party SDKs like AMD AGS or similar they might bypass SL and create D3D/DXGI base interfaces which will result in SL plugins not loading or functioning correctly. In that scenario base interfaces have to be upgraded using the following API:

```cpp
AGSDX12DeviceCreationParams creationParams;
AGSDX12ReturnedParams returnedParams;

creationParams.pAdapter = targetAdapter.Get();
creationParams.FeatureLevel = m_DeviceParams.featureLevel;
creationParams.iid = IID_PPV_ARGS(&m_Device12);

AGSReturnCode ret = agsDriverExtensionsDX12_CreateDevice(agsContext, &creationParams, nullptr, &returnedParams);

if (ret == AGS_SUCCESS)
{
    // AGS bypasses SL interposer so we have to let SL know to use this device
    if (SL_FAILED(result, slSetD3DDevice(returnedParams.pDevice)))
    {
        donut::log::error("Failed to upgrade AGS device using Streamline - result code %d.", result);
    }
    else
    {
        // At this point all requested and supported SL features have been initialized with the provided device
        m_Device12.Attach(returnedParams.pDevice);
    }
}

agsDriverExtensionsDX12_DestroyDevice(agsContext, m_Device12, nullptr);
```

> **NOTE:**
> To ensure SL is functioning correctly `slUpgradeInterface` API must be called **before** any of the base interface methods are invoked.

### 5.2 SL AND THIRD PARTY OVERLAYS

The following rules should be followed in order for 3rd party overlays to work correctly with SL:

* Overlays in general **must not make assumptions about swap-chain and command queues, when DLSS-G is active there could be multiple command queues and multiple asynchronous presents.**
* Overlays should intercept `IDXGIFactory::CreateSwapChainXXX` to obtain correct swapchain and command queue used to present frames.
* If integrated in engine, overlays should initialize before `slInit` is called
* There is an optional flag `sl::PreferenceFlags::eUseDXGIFactoryProxy` which can be used to avoid injecting SL hooks in DXGI factory v-table, this might help resolve some issues with overlays.

### 5.3 HOW TO CHECK IF SL PROXIES ARE USED

Since third party libraries (debug tools, overlays etc.) do not have access to `slGetNativeInterface` there is an alternative way to check if SL proxies are used and to obtain native interfaces:

```cpp
// Use special GUID to to obtain the underlying native interface if SL proxy is used, returns null otherwise
IID riid;
IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}",&riid);
// This can be ID3D12Device, IDXGIFactory, ID3D12GraphicsCommandList or any other interface which has SL proxy
nativeInterface = nullptr;
incomingInterface->QueryInterface(riid, reinterpret_cast<void**>(&nativeInterface));
if (nativeInterface)
{
    // SL proxy returned native interface
    nativeInterface->Release();
}
else
{
    // SL proxy is not active, use incoming interface as is
    nativeInterface = incomingInterface;
}
```

> NOTE
> Third party libraries SHOULD NOT use SL proxies to avoid app instability.

### 5.4 CMAKE SUPPORT

A `CMakeLists.txt` file is included for integrations using [CMake](https://cmake.org). Uncompress the Streamline package (e.g. to `streamline`) and in one of your `CMakeLists.txt` add:
```
add_directory(streamline)
```

It contains the following CMake Options:
| Option Name | Description | Default |
|--|--|--|
| STREAMLINE_FEATURE_DLSS_SR | Include DLSS-SR dll | OFF
| STREAMLINE_FEATURE_IMGUI | Include Imgui dll | OFF
| STREAMLINE_FEATURE_REFLEX | Include Reflex dll | OFF
| STREAMLINE_FEATURE_NIS | Include NIS dll | OFF
| STREAMLINE_FEATURE_DLSS_FG | Include DLSS-FG dll | OFF
| STREAMLINE_IMPORT_AS_INTERFACE | Import Streamline as an Interface without lib | OFF

6 EXCEPTION HANDLING
------------------------------------------------

If an exception is thrown while executing SL code mini-dump will be written to `ProgramData/NVIDIA/Streamline/$exe_name/$unique_id`. Exception which occur outside of SL code will continue to be captured by the host.

7 SUPPORT
------------------------------------------------

For any SL related questions, please email StreamlineSupport@nvidia.com.
