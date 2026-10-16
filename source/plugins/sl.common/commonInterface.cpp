/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <map>
#include <vector>

// Prevent warnings from MS headers
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <Winternl.h>
#include <d3dkmthk.h>
#include <d3dkmdt.h>

#include "include/sl.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.thread/thread.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/core/sl.extra/extra.h"
#include "source/core/sl.param/parameters.h"
#include "source/core/sl.interposer/hook.h"
#include "source/core/sl.interposer/d3d12/d3d12.h"
#include "source/core/sl.interposer/vulkan/layer.h"
#include "source/platforms/sl.chi/d3d11.h"
#include "source/platforms/sl.chi/d3d12.h"
#include "source/platforms/sl.chi/vulkan.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "source/plugins/sl.imgui/imgui.h"

#include "_artifacts/gitVersion.h"
#include "nvapi.h"
#include "external/json/include/nlohmann/json.hpp"
#include <atomic>
using json = nlohmann::json;

namespace sl
{

using namespace common;


struct CommonInterfaceContext
{
    RenderAPI platform{};
    sl::chi::ICompute* compute{};
    sl::chi::ICompute* computeDX11On12{};
    sl::chi::IResourcePool* pool{};
    uint64_t currentFrame{};

    // Present hook health tracking
    std::atomic<uint32_t> evalSinceLastPresent{ 0 };

    IDXGIAdapter3* adapter{};

    sl::PreferenceFlags flags{};
    bool interposerEnabled = true;
    bool manageVRAMBudget = true;
    bool emulateLowVRAMScenario = false;

    thread::ThreadContext<chi::D3D11ThreadContext>* threadsD3D11{};
    thread::ThreadContext<chi::D3D12ThreadContext>* threadsD3D12{};
    thread::ThreadContext<chi::VulkanThreadContext>* threadsVulkan{};

    std::map<Feature, EvaluateCallbacks> evalCallbacks;

    NvPhysicalGpuHandle nvGPUHandle[NVAPI_MAX_PHYSICAL_GPUS]{};
    NvU32 nvGPUCount = 0;

    common::SystemCaps sysCaps{};

    chi::CommonThreadContext& getThreadContext()
    {
        if (platform == RenderAPI::eD3D11)
        {
            if (!threadsD3D11)
            {
                threadsD3D11 = new thread::ThreadContext<chi::D3D11ThreadContext>();
            }
            return threadsD3D11->getContext();
        }
        else if (platform == RenderAPI::eD3D12)
        {
            if (!threadsD3D12)
            {
                threadsD3D12 = new thread::ThreadContext<chi::D3D12ThreadContext>();
            }
            return threadsD3D12->getContext();
        }

        if (!threadsVulkan)
        {
            threadsVulkan = new thread::ThreadContext<chi::VulkanThreadContext>();
        }
        return threadsVulkan->getContext();
    }

    UINT64 m_maxMemoryUsage = 0;

};

//! Our secondary context
CommonInterfaceContext ctx;

extern void recycleFramePresentEndResourceTags();

uint64_t getCurrentFrame()
{
    return ctx.currentFrame;
}


//! Get GPU information and share with other plugins
//! 
bool getSystemCaps(common::SystemCaps*& info)
{
    ctx.sysCaps = {};
    info = &ctx.sysCaps;

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus))
    {
        // https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status
        ctx.sysCaps.laptopDevice = powerStatus.BatteryFlag != 128; // No system battery according to MS docs
    }

    PFND3DKMT_ENUMADAPTERS2 pfnEnumAdapters2{};
    PFND3DKMT_QUERYADAPTERINFO pfnQueryAdapterInfo{};
    PFND3DKMT_CLOSEADAPTER pfnCloseAdapter{};

    // We support up to kMaxNumSupportedGPUs adapters (currently 8)
    SL_LOG_INFO("Enumerating up to %u adapters but only one of them can be used to create a device - no mGPU support in this SDK", kMaxNumSupportedGPUs);

    D3DKMT_ADAPTERINFO adapterInfo[kMaxNumSupportedGPUs]{};
    D3DKMT_ENUMADAPTERS2 enumAdapters2{};

    auto modGDI32 = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (modGDI32)
    {
        pfnEnumAdapters2 = (PFND3DKMT_ENUMADAPTERS2)GetProcAddress(modGDI32, "D3DKMTEnumAdapters2");
        pfnQueryAdapterInfo = (PFND3DKMT_QUERYADAPTERINFO)GetProcAddress(modGDI32, "D3DKMTQueryAdapterInfo");
        pfnCloseAdapter = (PFND3DKMT_CLOSEADAPTER)GetProcAddress(modGDI32, "D3DKMTCloseAdapter");

        // Request adapter info from KMT
        if (pfnEnumAdapters2)
        {
            enumAdapters2.NumAdapters = kMaxNumSupportedGPUs;
            enumAdapters2.pAdapters = adapterInfo;
            HRESULT enumRes = pfnEnumAdapters2(&enumAdapters2);
            if (!NT_SUCCESS(enumRes))
            {
                if (enumRes == STATUS_BUFFER_TOO_SMALL)
                {
                    SL_LOG_WARN("Enumerating up to %u adapters on a system with more than that many adapters: internal error", kMaxNumSupportedGPUs);
                    assert("The fixed max number of adapters is too small for the system" && false);
                }
                else
                {
                    SL_LOG_WARN("Adapter enumeration has failed - cannot determine adapter capabilities; Some features may be unavailable");
                }
                // Clear everything, no adapter infos available!
                enumAdapters2 = {};
            }
        }
    }

    ctx.nvGPUCount = 0;

#ifndef SL_PRODUCTION
    bool forceNonNVDA = (*(json*)api::getContext()->loaderConfig)["forceNonNVDA"];
#endif

    IDXGIFactory4* factory;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
    {
        uint32_t i = 0;
        IDXGIAdapter3* adapter;
        while (factory->EnumAdapters(i++, reinterpret_cast<IDXGIAdapter**>(&adapter)) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                // Intel, AMD or NVIDIA physical GPUs only
                auto vendor = (chi::VendorId)desc.VendorId;
                
#ifndef SL_PRODUCTION
                //! Test - force non-NVDA
                if (forceNonNVDA && isVendorNvidia(vendor))
                {
                    vendor = chi::VendorId::eAMD;
                }
#endif

                if (isVendorNvidia(vendor) || vendor == chi::VendorId::eIntel || vendor == chi::VendorId::eAMD)
                {
                    info->adapters[info->gpuCount].nativeInterface = adapter;
                    info->adapters[info->gpuCount].vendor = vendor;
                    info->adapters[info->gpuCount].bit = 1 << info->gpuCount; 
                    info->adapters[info->gpuCount].id = desc.AdapterLuid;
                    info->adapters[info->gpuCount].deviceId = desc.DeviceId;
                    info->gpuCount++;
                    
                    if (info->gpuCount == kMaxNumSupportedGPUs) break;

                    if (isVendorNvidia(vendor))
                    {
                        ctx.nvGPUCount++;
                    }
                    // Check HWS for this LUID if NVDA adapter
                    if (!ctx.sysCaps.hwsSupported && enumAdapters2.NumAdapters > 0 && pfnQueryAdapterInfo)
                    {
                        for (uint32_t k = 0; k < enumAdapters2.NumAdapters; k++)
                        {
                            if (adapterInfo[k].AdapterLuid.HighPart == desc.AdapterLuid.HighPart &&
                                adapterInfo[k].AdapterLuid.LowPart == desc.AdapterLuid.LowPart)
                            {
                                D3DKMT_QUERYADAPTERINFO info{};
                                info.hAdapter = adapterInfo[k].hAdapter;
                                info.Type = KMTQAITYPE_WDDM_2_7_CAPS;
                                D3DKMT_WDDM_2_7_CAPS data{};
                                info.pPrivateDriverData = &data;
                                info.PrivateDriverDataSize = sizeof(data);
                                NTSTATUS err = pfnQueryAdapterInfo(&info);
                                if (NT_SUCCESS(err) && data.HwSchEnabled)
                                {
                                    ctx.sysCaps.hwsSupported = true;
                                }
                                break;
                            }
                        }
                    }

                    // Adapter released on shutdown
                }
                else
                {
                    adapter->Release();
                }
            }
            
        }
        factory->Release();
    }

    if (ctx.nvGPUCount > 0)
    {
        // Detected at least one NVDA GPU, we can use NVAPI
        if (NvAPI_EnumPhysicalGPUs(ctx.nvGPUHandle, &ctx.nvGPUCount) == NVAPI_OK)
        {
            ctx.nvGPUCount = std::min((NvU32)kMaxNumSupportedGPUs, ctx.nvGPUCount);
            NvU32 driverVersion;
            NvAPI_ShortString driverName;
            NVAPI_VALIDATE_RF(NvAPI_SYS_GetDriverAndBranchVersion(&driverVersion, driverName));
            SL_LOG_INFO(">-----------------------------------------");
            ctx.sysCaps.driverVersionMajor = driverVersion / 100;
            ctx.sysCaps.driverVersionMinor = driverVersion % 100;
            SL_LOG_INFO("NVIDIA driver %u.%u", ctx.sysCaps.driverVersionMajor, ctx.sysCaps.driverVersionMinor);
            for (NvU32 gpu = 0; gpu < ctx.nvGPUCount; ++gpu)
            {
                // Find LUID for NVDA physical device
                LUID id;
                NvLogicalGpuHandle hLogicalGPU;
                NVAPI_VALIDATE_RF(NvAPI_GetLogicalGPUFromPhysicalGPU(ctx.nvGPUHandle[gpu], &hLogicalGPU));
                NV_LOGICAL_GPU_DATA lData{};
                lData.version = NV_LOGICAL_GPU_DATA_VER;
                lData.pOSAdapterId = &id;
                NVAPI_VALIDATE_RF(NvAPI_GPU_GetLogicalGpuInfo(hLogicalGPU, &lData));
                
                // Now find adapter by matching the LUID
                for (uint32_t i = 0; i < ctx.sysCaps.gpuCount; i++)
                {
                    if (ctx.sysCaps.adapters[i].id.HighPart == id.HighPart && ctx.sysCaps.adapters[i].id.LowPart == id.LowPart)
                    {
                        auto& adapter = ctx.sysCaps.adapters[i];

                        NV_GPU_ARCH_INFO archInfo;
                        archInfo.version = NV_GPU_ARCH_INFO_VER;
                        NVAPI_VALIDATE_RF(NvAPI_GPU_GetArchInfo(ctx.nvGPUHandle[gpu], &archInfo));
                        adapter.architecture = archInfo.architecture;
                        adapter.implementation = archInfo.implementation;
                        adapter.revision = archInfo.revision;
                        SL_LOG_INFO("Adapter %u architecture 0x%x implementation 0x%x revision 0x%x - bit 0x%0x - LUID %u.%u", gpu, adapter.architecture, adapter.implementation, adapter.revision, adapter.bit, adapter.id.HighPart, adapter.id.LowPart);
                        break;
                    }
                }

            };
            SL_LOG_INFO("-----------------------------------------<");
        }
        else
        {
            SL_LOG_WARN("NVAPI failed to initialize, please update your driver if running on NVIDIA hardware");
        }
    }

    if (pfnCloseAdapter)
    {
        for (uint32_t k = 0; k < enumAdapters2.NumAdapters; k++)
        {
            const D3DKMT_CLOSEADAPTER adapter { adapterInfo[k].hAdapter };
            auto res = pfnCloseAdapter(&adapter);
            if (!NT_SUCCESS(res))
            {
                SL_LOG_WARN("Failed to close adapter 0x%x: %u", adapter.hAdapter, res);
            }
        }
        enumAdapters2 = {};
    }

    if (modGDI32)
    {
        FreeLibrary(modGDI32);
    }
    return true;
}

//! Current thread context
chi::CommonThreadContext* getThreadContext()
{
    return &ctx.getThreadContext();
}

namespace common
{

//! Create compute API which is then shared with all active plugins
std::pair<sl::chi::ICompute*, sl::chi::ICompute*> createCompute(void* device, RenderAPI deviceType, bool dx11On12)
{
    PFun_ResourceAllocateCallback* allocate = {};
    PFun_ResourceReleaseCallback* release = {};
    param::getPointerParam(api::getContext()->parameters, param::global::kPFunAllocateResource, &allocate);
    param::getPointerParam(api::getContext()->parameters, param::global::kPFunReleaseResource, &release);

    ctx.platform = (RenderAPI)deviceType;

    ctx.compute = {};
    ctx.computeDX11On12 = {};

    if (deviceType == RenderAPI::eD3D11)
    {
        ctx.compute = sl::chi::getD3D11();
        if (dx11On12)
        {
            ctx.computeDX11On12 = sl::chi::getD3D12();
        }
    }
    else if (deviceType == RenderAPI::eD3D12)
    {
        ctx.compute = sl::chi::getD3D12();
    }
    else
    {
        ctx.compute = sl::chi::getVulkan();
    }
    // Allow resource allocations in `init` to be hooked by host
    CHI_VALIDATE(ctx.compute->setCallbacks(allocate, release, getThreadContext));
    CHI_VALIDATE(ctx.compute->init(device, api::getContext()->parameters));

    api::getContext()->parameters->set(sl::param::common::kComputeAPI, ctx.compute);

    if (ctx.computeDX11On12)
    {
        CHI_VALIDATE(ctx.computeDX11On12->init(device, api::getContext()->parameters));
        // No callbacks here, d3d11 engines cannot allocate/deallocate d3d12 resources
        api::getContext()->parameters->set(sl::param::common::kComputeDX11On12API, ctx.computeDX11On12);
    }

    return { ctx.compute, ctx.computeDX11On12 };
}

//! Destroy compute API when sl.common is released
bool destroyCompute()
{
    if (ctx.computeDX11On12)
    {
        CHI_CHECK_RF(ctx.computeDX11On12->shutdown());
    }

    CHI_CHECK_RF(ctx.compute->shutdown());
    if (ctx.threadsD3D11)
    {
        delete ctx.threadsD3D11;
    }
    if (ctx.threadsD3D12)
    {
        delete ctx.threadsD3D12;
    }
    if (ctx.threadsVulkan)
    {
        delete ctx.threadsVulkan;
    }
    ctx.threadsD3D11 = {};
    ctx.threadsD3D12 = {};
    ctx.threadsVulkan = {};

    return true;
}

//! Common register callbacks from other plugins
//! 
//! Used to dispatch evaluate calls to the correct plugin.
//! 
void registerEvaluateCallbacks(Feature feature, PFunBeginEndEvent* beginEvaluate, PFunBeginEndEvent* endEvaluate)
{
    ctx.evalCallbacks[feature] = { beginEvaluate, endEvaluate };
}

//! Checks if proxies are used and returns correct command buffer to use
CommandBuffer* getNativeCommandBuffer(CommandBuffer* cmdBuffer, bool* slProxy)
{
    // First we need to get to the correct base interface we need to use
    CommandBuffer* cmdList = nullptr;
    if (cmdBuffer)
    {
        if (ctx.platform == RenderAPI::eD3D11)
        {
            // No interposing for d3d11 
            cmdList = cmdBuffer;
        }
        else if (ctx.platform == RenderAPI::eD3D12)
        {
            // Check if this is our proxy
            IUnknown* unknown = (IUnknown*)cmdBuffer;
            interposer::D3D12GraphicsCommandList* proxy{};
            unknown->QueryInterface(&proxy);
            if (proxy)
            {
                if(slProxy) *slProxy = true;
                ((IUnknown*)proxy)->Release();
                auto& thread = (chi::D3D12ThreadContext&)ctx.getThreadContext();
                thread.cmdList = proxy;
                cmdList = proxy->m_base;
            }
            else
            {
                // Not our proxy, either native command list or host's proxy
                cmdList = cmdBuffer;
            }
        }
        else
        {
            // No interface override in case of Vulkan
            cmdList = (VkCommandBuffer)cmdBuffer;
        }
    }
    return cmdList;
}

bool onLoad(const void* managerConfigPtr, const void* extraConfigPtr, chi::IResourcePool* pool)
{
    // Plugin manager provides various settings through JSON
    json& managerConfig = *(json*)managerConfigPtr;
    json& extraConfig = *(json*)extraConfigPtr;
    // This is always provided by our manager 
    ctx.flags = managerConfig["preferences"]["flags"];
    ctx.interposerEnabled = managerConfig["interposerEnabled"];
    ctx.pool = pool;
    if (extraConfig.contains("manageVRAMBudget"))
    {
        ctx.manageVRAMBudget = extraConfig["manageVRAMBudget"];
    }
    if (extraConfig.contains("emulateLowVRAMScenario"))
    {
        ctx.emulateLowVRAMScenario = extraConfig["emulateLowVRAMScenario"];
    }
    return true; 
}
}

//! Common evaluate feature
//! 
//! Here we intercept evaluate calls from host and figure out the 
//! callbacks for the requested feature (sl plugin)
sl::Result slEvaluateFeatureInternal(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer)
{
    auto evalCallbacks = ctx.evalCallbacks[feature];
    if (!evalCallbacks.beginEvaluate || !evalCallbacks.endEvaluate)
    {
        SL_LOG_ERROR_ONCE( "Could not find 'evaluateFeature' callbacks for feature %u", feature);
        return Result::eErrorMissingOrInvalidAPI;
    }

    uint32_t id = 0;
    auto viewport = findStruct<ViewportHandle>((const void**)inputs, numInputs);
    if (viewport)
    {
        id = *viewport;
    }

    bool slProxy = false;
    auto cmdList = getNativeCommandBuffer(cmdBuffer, &slProxy);

    // This allows us to map correct constants and tags to this evaluate call
    common::EventData event = { id, frame };

    // Push the state (d3d11 only, nop otherwise)
    CHI_CHECK_RR(ctx.compute->pushState(cmdList));

    auto res = evalCallbacks.beginEvaluate(cmdList, event, inputs, numInputs);
    if (res == sl::Result::eOk)
    {
        res = evalCallbacks.endEvaluate(cmdList, event, inputs, numInputs);
    }

    // Pop the state (d3d11 only, nop otherwise)
    CHI_CHECK_RR(ctx.compute->popState(cmdList));

    // Detect missing Present hook: trigger when many evaluates occur without any Present
    {
        // Increment evaluates since last Present
        uint32_t evals = ++ctx.evalSinceLastPresent;

        // Conservative threshold (frames) to avoid false positives on loading screens
        constexpr uint32_t kEvalThreshold = 60;

        if (evals > kEvalThreshold)
        {
            SL_LOG_ERROR_ONCE("Streamline presentCommon() was not observed. Ensure the common plugin's presentCommon() function is invoked every frame. Without presentCommon(), internal bookkeeping and garbage collection will not run.");
        }
    }

    // Moving to host being responsible for this but still supporting legacy apps as much as possible
    if (slProxy && (ctx.flags & PreferenceFlags::eUseManualHooking) == 0 && ctx.interposerEnabled)
    {
        // Restore the pipeline so host can continue running like we never existed
        CHI_CHECK_RR(ctx.compute->restorePipeline(cmdList));
    }

    //! Check for out of VRAM error
    //! 
    //! Note that we are not stomping the error returned by evaluate (if any)
    if (ctx.currentFrame > 0 && res == Result::eOk)
    {
        uint64_t availableBytes{};
        if (ctx.compute->getVRAMBudget(availableBytes) == chi::ComputeStatus::eOk)
        {
            if (availableBytes == 0)
            {
                res = Result::eWarnOutOfVRAM;
                SL_LOG_WARN("Exceeded VRAM budget, various performance issues including stuttering can be expected");
            }
        }
    }

    return res;
}

//! Hooks

//! D3D12

//! Gets the LUID of the adapter that the app device was created on.
//! Returns false when the LUID is not available (for example on Vulkan).
bool getAppDeviceAdapterLuid(LUID& luid)
{
    chi::Device device{};
    if (!ctx.compute || ctx.compute->getDevice(device) != chi::ComputeStatus::eOk || !device)
    {
        return false;
    }

    if (ctx.platform == RenderAPI::eD3D12)
    {
        luid = static_cast<ID3D12Device*>(device)->GetAdapterLuid();
        return true;
    }

    if (ctx.platform == RenderAPI::eD3D11)
    {
        bool found = false;
        IDXGIDevice* dxgiDevice{};
        if (SUCCEEDED(static_cast<ID3D11Device*>(device)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)))
        {
            IDXGIAdapter* dxgiAdapter{};
            if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)))
            {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(dxgiAdapter->GetDesc(&desc)))
                {
                    luid = desc.AdapterLuid;
                    found = true;
                }
                dxgiAdapter->Release();
            }
            dxgiDevice->Release();
        }
        return found;
    }

    return false;
}

void presentCommon(UINT Flags, void* nativeSwapChain = nullptr, const std::vector<chi::SyncPoint>& presentSyncPoints = {}, chi::Fence* additionalPresentWaitSemaphore = nullptr)
{
    if (additionalPresentWaitSemaphore)
    {
        *additionalPresentWaitSemaphore = {};
    }

    if ((Flags & DXGI_PRESENT_TEST))
    {
        return;
    }

    if (ctx.compute)
    {
        // Reset evaluates counter when Present is observed
        ctx.evalSinceLastPresent.store(0);

        if (ctx.manageVRAMBudget)
        {
            if (ctx.emulateLowVRAMScenario)
            {
                ctx.compute->setVRAMBudget(UINT64_MAX, UINT64_MAX);
            }
            else if (ctx.adapter)
            {
                //! IMPORTANT: Overhead for calling 'QueryVideoMemoryInfo' is 0.01ms 
                DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
                ctx.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);
                ctx.compute->setVRAMBudget(videoMemoryInfo.CurrentUsage, videoMemoryInfo.Budget);
                // to reduce the LOG spam - print only when the new maximum is reached
                if (videoMemoryInfo.CurrentUsage > ctx.m_maxMemoryUsage)
                {
                    ctx.m_maxMemoryUsage = videoMemoryInfo.CurrentUsage;
                    SL_LOG_VERBOSE("Total VRAM used: %.2lf GB", ctx.m_maxMemoryUsage / (double)(1024 * 1024 * 1024));
                }
            }
        }
        else
        {
            // Do not manage any budget, assume endless resources
            ctx.compute->setVRAMBudget(0, UINT64_MAX);
        }

        if (!ctx.currentFrame)
        {
            // First run, find best NVDA adapter.
            //
            // Only use an NVDA adapter that the app renders on. On a hybrid system (for example
            // a laptop with an NVIDIA dGPU and an eGPU from another vendor) the NVDA adapter can
            // be idle in runtime D3. QueryVideoMemoryInfo on that adapter then waits for the GPU
            // power arbiter and stalls every Present for ~150 ms. If no NVDA adapter matches,
            // ctx.adapter stays null, which is the same path as a system without an NVDA adapter.
            // If the app device LUID is not available, keep the previous selection.
            LUID appLuid{};
            const bool hasAppLuid = getAppDeviceAdapterLuid(appLuid);
            uint32_t hwArch = 0;
            for (uint32_t i = 0; i < kMaxNumSupportedGPUs; i++)
            {
                const bool rendersOnAdapter = !hasAppLuid ||
                    (ctx.sysCaps.adapters[i].id.HighPart == appLuid.HighPart && ctx.sysCaps.adapters[i].id.LowPart == appLuid.LowPart);
                if (isVendorNvidia(ctx.sysCaps.adapters[i].vendor) && ctx.sysCaps.adapters[i].architecture > hwArch && rendersOnAdapter)
                {
                    hwArch = ctx.sysCaps.adapters[i].architecture;
                    ctx.adapter = reinterpret_cast<IDXGIAdapter3*>(ctx.sysCaps.adapters[i].nativeInterface);
                }
            }

#ifndef SL_PRODUCTION
            // Check for UI and register our callback
            imgui::ImGUI* ui{};
            param::getPointerParam(api::getContext()->parameters, param::imgui::kInterface, &ui);
            if (ui)
            {
                // Runs async from the present thread where UI is rendered just before frame is presented
                auto renderUI = [](imgui::ImGUI* ui, bool finalFrame)->void
                {
                    imgui::Float4 highlightColor{ 153.0f / 255.0f, 217.0f / 255.0f, 234.0f / 255.0f,1 };
                    imgui::Float4 warnColor{ 1.0f, 0.6f, 0, 1.0f };
                    auto v = api::getContext()->pluginVersion;
                    if (ui->collapsingHeader(extra::format("{} v{}", api::getPluginName(), (v.toStr() + "." + GIT_LAST_COMMIT_SHORT)).c_str(), imgui::kTreeNodeFlagDefaultOpen))
                    {
                        // Data does not change here so no thread sync needed
                        uint64_t bytes, commonBytes;
                        ctx.compute->getAllocatedBytes(bytes);
                        if (ctx.computeDX11On12)
                        {
                            uint64_t extraBytes;
                            ctx.computeDX11On12->getAllocatedBytes(extraBytes);
                            bytes += extraBytes;
                        }
                        // Our resource pool for volatile tags
                        ctx.compute->getAllocatedBytes(commonBytes, api::getPluginName());
                        static std::string s_platforms[] = { "D3D11","D3D12","Vulkan" };
                        ui->labelColored(highlightColor, "Computer: ", "%s", extra::format("{}", ctx.sysCaps.laptopDevice ? "Laptop" : "PC").c_str());
                        ui->labelColored(highlightColor, "OS: ", "%s", extra::format("{}.{}.{}", ctx.sysCaps.osVersionMajor, ctx.sysCaps.osVersionMinor, ctx.sysCaps.osVersionBuild).c_str());
                        ui->labelColored(highlightColor, "Driver: ", "%s", extra::format("{}.{}", ctx.sysCaps.driverVersionMajor, ctx.sysCaps.driverVersionMinor).c_str());
                        ui->labelColored(highlightColor, "GPU: ", "%s", extra::format("Arch {} Rev {} Impl {}", ctx.sysCaps.adapters[0].architecture, ctx.sysCaps.adapters[0].revision, ctx.sysCaps.adapters[0].implementation).c_str());
                        ui->labelColored(highlightColor, "Render API: ", "%s", s_platforms[(uint32_t)ctx.platform].c_str());
                        ui->labelColored(highlightColor, "Volatile VRAM: ", "%.2fMB", commonBytes / (1024.0 * 1024.0));
                        if (ctx.adapter)
                        {
                            DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
                            ctx.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);
                            if (ctx.manageVRAMBudget && ctx.emulateLowVRAMScenario)
                            {
                                ui->labelColored(highlightColor, "VRAM: ", "SL %.2fGB - EMULATING LOW VRAM", bytes / (1024.0 * 1024.0 * 1024.0));
                            }
                            else
                            {
                                ui->labelColored(videoMemoryInfo.Budget > videoMemoryInfo.CurrentUsage ? highlightColor : warnColor, "VRAM: ", "SL %.2fGB Total %.2fGB Budget %.2fGB", bytes / (1024.0 * 1024.0 * 1024.0), videoMemoryInfo.CurrentUsage / (1024.0 * 1024.0 * 1024.0), videoMemoryInfo.Budget / (1024.0 * 1024.0 * 1024.0));
                            }

                            const size_t kMaxNumGraphValues = 120;
                            static std::vector<double> xAxis;
                            static std::vector<double> yAxis[2];
                            static double graphMinY = 0;
                            static double graphMaxY = 0;
                            if (xAxis.empty())
                            {
                                for (size_t i = 0; i < kMaxNumGraphValues; i++)
                                {
                                    xAxis.push_back((double)i);
                                }
                            }

                            if (yAxis[0].size() == kMaxNumGraphValues)
                            {
                                yAxis[0].erase(yAxis[0].begin());
                            }
                            yAxis[0].push_back(videoMemoryInfo.CurrentUsage / (1024.0 * 1024.0 * 1024.0));

                            if (yAxis[1].size() == kMaxNumGraphValues)
                            {
                                if (yAxis[1].front() == graphMinY) graphMinY = 1e20;
                                if (yAxis[1].front() == graphMaxY) graphMaxY = 0;
                                yAxis[1].erase(yAxis[1].begin());
                            }
                            yAxis[1].push_back(videoMemoryInfo.Budget / (1024.0 * 1024.0 * 1024.0));

                            graphMaxY = std::max(graphMaxY, yAxis[1].back());

                            {
                                imgui::Graph g = { "##vram", "VRAM", "GB", 0.0, (double)kMaxNumGraphValues, graphMinY, ((graphMaxY + 5) / 5) * 5, xAxis.data(), (uint32_t)yAxis[0].size() };
                                std::vector<imgui::GraphValues> values = { {"Current",yAxis[0].data(),(uint32_t)yAxis[0].size(), imgui::GraphFlags::eShaded}, {"Budget",yAxis[1].data(),(uint32_t)yAxis[1].size(), imgui::GraphFlags::eNone} };
                                ui->plotGraph(g, values);
                            }
                        }
                        else
                        {
                            ui->labelColored(highlightColor, "Total VRAM: ", "%.2fMB", bytes / (1024.0 * 1024.0));
                        }
                    }
                };
                ui->registerRenderCallbacks(renderUI, nullptr);
            }
#endif
        }

        ++ctx.currentFrame;

        // This will release any resources scheduled to be destroyed few frames behind
        CHI_VALIDATE(ctx.compute->collectGarbage((uint32_t)ctx.currentFrame));
        // This will release unused recycled resources (volatile tag copies)
        ctx.pool->collectGarbage();
    }

    // Our stats including GPU load info
    //static std::string s_stats;
    //auto v = api::getContext()->pluginVersion;
    
    // Can be expensive to query every frame, comment out for now
    /*for (uint32_t i = 0; i < ctx.nvGPUCount; i++)
    {
        NV_GPU_DYNAMIC_PSTATES_INFO_EX gpuLoads{};
        gpuLoads.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;
        NvAPI_GPU_GetDynamicPstatesInfoEx(ctx.nvGPUHandle[i], &gpuLoads);
        ctx.sysCaps.gpuLoad[i] = gpuLoads.utilization[i].percentage;
    }*/

    /*static std::string s_platforms[] = { "D3D11","D3D12","VK" };
    s_stats = extra::format("{} {} - {}", api::getPluginName(), v.toStr() + "." + GIT_LAST_COMMIT_SHORT, s_platforms[ctx.platform]);
    api::getContext()->parameters->set(sl::param::common::kStats, (void*)s_stats.c_str());*/
}

void afterPresentCommon(UINT Flags)
{
    if ((Flags & DXGI_PRESENT_TEST))
    {
        return;
    }

    recycleFramePresentEndResourceTags();
}

HRESULT slHookCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain, bool& Skip)
{
    return S_OK;
}

HRESULT slHookCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain, bool& Skip)
{
    return S_OK;
}

HRESULT slHookCreateSwapChainForCoreWindow(IDXGIFactory2* pFactory, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain, bool& Skip)
{
    return S_OK;
}


HRESULT slHookPresent1(IDXGISwapChain* swapChain, UINT SyncInterval, UINT Flags, DXGI_PRESENT_PARAMETERS* params, bool& Skip)
{
    presentCommon(Flags, (void*)swapChain);
    return S_OK;
}

HRESULT slHookPresent(IDXGISwapChain* swapChain, UINT SyncInterval, UINT Flags, bool& Skip)
{
    presentCommon(Flags, (void*)swapChain);
    return S_OK;
}

HRESULT slHookAfterPresent(UINT Flags)
{
    afterPresentCommon(Flags);
    return S_OK;
}

HRESULT slHookResizeSwapChainPre(IDXGISwapChain* swapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, bool& Skip)
{
    CHI_VALIDATE(ctx.compute->clearCache());
    return S_OK;
}

//! VULKAN

VkResult slHookVkPresent(VkQueue Queue, const VkPresentInfoKHR* PresentInfo, bool& Skip)
{
    void* nativeSwapChain = (PresentInfo && PresentInfo->swapchainCount > 0) ? (void*)PresentInfo->pSwapchains[0] : nullptr;

    presentCommon(0, nativeSwapChain);

    return VK_SUCCESS;
}

VkResult slHookVkAfterPresent()
{
    afterPresentCommon(0);

    return VK_SUCCESS;
}

void slHookVkCmdBindPipeline(VkCommandBuffer CommandBuffer, VkPipelineBindPoint PipelineBindPoint, VkPipeline Pipeline)
{
    auto& thread = (chi::VulkanThreadContext&)ctx.getThreadContext();
    if (PipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        thread.PipelineBindPoint = PipelineBindPoint;
        thread.Pipeline = Pipeline;
    }
}

void slHookVkCmdBindDescriptorSets(VkCommandBuffer CommandBuffer, VkPipelineBindPoint PipelineBindPoint, VkPipelineLayout Layout, uint32_t FirstSet, uint32_t DescriptorSetCount, const VkDescriptorSet* DescriptorSets, uint32_t DynamicOffsetCount, const uint32_t* DynamicOffsets)
{
    auto& thread = (chi::VulkanThreadContext&)ctx.getThreadContext();
    if (PipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        thread.PipelineBindPointDesc = PipelineBindPoint;
        thread.Layout = Layout;
        thread.FirstSet = FirstSet;
        thread.DescriptorCount = DescriptorSetCount;
        thread.DynamicOffsetCount = DynamicOffsetCount;
        if (DynamicOffsetCount > chi::kDynamicOffsetCount)
        {
            SL_LOG_WARN("Dynamic offsets exceeding cached size");
        }
        if (DescriptorSetCount > chi::kDescriptorCount)
        {
            SL_LOG_WARN("Descriptor sets count exceeding cached size");
        }
        for (uint32_t i = 0; i < chi::kDynamicOffsetCount; i++)
        {
            if (i >= DynamicOffsetCount) break;
            thread.DynamicOffsets[i] = DynamicOffsets[i];
        }
        for (uint32_t i = 0; i < chi::kDescriptorCount; i++)
        {
            if (i >= DescriptorSetCount) break;
            thread.DescriptorSets[i] = DescriptorSets[i];
        }
    }
}

void slHookVkBeginCommandBuffer(VkCommandBuffer CommandBuffer, const VkCommandBufferBeginInfo* BeginInfo)
{
    auto& thread = (chi::VulkanThreadContext&)ctx.getThreadContext();
    thread = {};
}

}
