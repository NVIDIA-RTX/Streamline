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

#include <dxgi1_6.h>
#include <sstream>
#include <atomic>
#include <future>
#include <unordered_set>

#include "include/sl.h"
#include "include/sl_dlss.h"
#include "include/sl_struct.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.api/internalDataSharing.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/core/sl.file/file.h"
#include "source/core/sl.extra/extra.h"
#include "source/core/sl.param/parameters.h"
#include "external/json/include/nlohmann/json.hpp"
#include "nvapi.h"

#include "source/platforms/sl.chi/vulkan.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "source/plugins/sl.dlss/versions.h"
#include "source/plugins/sl.imgui/imgui.h"
#include "source/plugins/sl.dlss/dlss_shared.h"

#include "source/platforms/sl.chi/capture.h"

#include "_artifacts/shaders/mvec_cs.h"
#include "_artifacts/shaders/mvec_spv.h"
#include "_artifacts/json/dlss_json.h"
#include "_artifacts/gitVersion.h"

#include "external/ngx-sdk/include/nvsdk_ngx.h"
#include "external/ngx-sdk/include/nvsdk_ngx_helpers.h"
#include "external/ngx-sdk/include/nvsdk_ngx_helpers_vk.h"
#include "external/ngx-sdk/include/nvsdk_ngx_defs.h"

using json = nlohmann::json;

namespace sl
{

using funNGXInit = NVSDK_NGX_Result(*)(unsigned long long InApplicationId, const wchar_t *InApplicationDataPath, ID3D12Device *InDevice, NVSDK_NGX_Version InSDKVersion);
using funNGXShutdown = NVSDK_NGX_Result(*)(void);
using funNGXCreate = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList *InCmdList, NVSDK_NGX_Feature InFeatureId, const NVSDK_NGX_Parameter *InParameters, NVSDK_NGX_Handle **OutHandle);
using funNGXRelease = NVSDK_NGX_Result(*)(NVSDK_NGX_Handle *InHandle);
using funNGXEval = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList *InCmdList, const NVSDK_NGX_Handle *InHandle, const NVSDK_NGX_Parameter *InParameters, PFN_NVSDK_NGX_ProgressCallback InCallback);

struct DLSSViewport
{
    uint32_t id = {};
    DLSSOptions consts{};
    DLSSOptimalSettings settings;
    NVSDK_NGX_Handle* handle = {};
    sl::chi::Resource mvec;
    sl::chi::Resource output;
    float2 inputTexelSize;

    // Note: since SR can have multiple viewports, we can never know which one is the "main" one
    // so sharing data across plugins is tricky without the app telling us
    // Maybe we should ask for it?
};

struct UIStats
{
    std::mutex mtx{};
    std::string mode{};
    std::string viewport{};
    std::string runtime{};
    std::string vram{};
};

namespace dlss
{
struct DLSSContext
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(DLSSContext);
    void onCreateContext() {};
    void onDestroyContext() {};

    std::future<bool> initLambda;

    Constants* commonConsts{};

    UIStats uiStats{};

    uint32_t adapterMask{};

    common::NGXContext* ngxContext = {};
    sl::chi::ICompute* compute;
#ifdef SL_CAPTURE
    sl::chi::ICapture* capture;
#endif
    sl::chi::Kernel mvecKernel;

#ifndef SL_PRODUCTION
    std::string ngxVersion{};
#endif

    common::PFunRegisterEvaluateCallbacks* registerEvaluateCallbacks{};
    common::ViewportIdFrameData<4, false> constsPerViewport = { "dlss" };
    std::map<void*, chi::ResourceState> cachedStates = {};
    std::map<void*, NVSDK_NGX_Resource_VK> cachedVkResources = {};
    std::map<uint32_t, DLSSViewport> viewports = {};
    DLSSViewport* viewport = {};

    RenderAPI platform;

    NVSDK_NGX_Resource_VK* cachedVkResource(sl::Resource* res)
    {
        auto it = cachedVkResources.find(res->native);
        return it == cachedVkResources.end() ? nullptr : &(*it).second;
    }

    void cacheState(chi::Resource res, uint32_t nativeState = 0)
    {
        //if (cachedStates.find(res) == cachedStates.end())
        {
            // Providing state is now mandatory, defaults to "common" which is 0
            chi::ResourceState state;
            compute->getResourceState(nativeState, state);
            cachedStates[res->native] = state;

            if (res && platform == RenderAPI::eVulkan)
            {
                NVSDK_NGX_Resource_VK ngx = {};
                if (res->native)
                {
                    chi::ResourceDescription desc = {};
                    desc.state = state;
                    CHI_CHECK_RV(compute->getResourceDescription(res, desc));
                    ngx.Resource.ImageViewInfo.ImageView = (VkImageView)res->view;
                    ngx.Resource.ImageViewInfo.Image = (VkImage)res->native;
                    sl::SubresourceRange* subresource = findStruct<SubresourceRange>(res);
                    if (subresource)
                    {
                        ngx.Resource.ImageViewInfo.SubresourceRange = { subresource->aspectMask, subresource->baseMipLevel, subresource->levelCount, subresource->baseArrayLayer, subresource->layerCount };
                    }
                    else
                    {
                        ngx.Resource.ImageViewInfo.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
                    }
                    ngx.Resource.ImageViewInfo.Format = (VkFormat)desc.nativeFormat;
                    ngx.Resource.ImageViewInfo.Width = desc.width;
                    ngx.Resource.ImageViewInfo.Height = desc.height;
                    ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
                    ngx.ReadWrite = (desc.flags & chi::ResourceFlags::eShaderResourceStorage) != 0;
                    cachedVkResources[res->native] = ngx;
                }
            }
        }
    }
};
}

constexpr uint32_t kMaxNumViewports = 4;

static std::string JSON = std::string(dlss_json, &dlss_json[dlss_json_len]);

void updateEmbeddedJSON(json& config);
internal::shared::Status getSharedData(BaseStructure* requestedData, const BaseStructure* requesterInfo);

SL_PLUGIN_DEFINE("sl.dlss", Version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH), Version(0, 0, 1), JSON.c_str(), updateEmbeddedJSON, dlss, DLSSContext)

void updateEmbeddedJSON(json& config)
{
    // Check if plugin is supported or not on this platform and set the flag accordingly
    common::SystemCaps* caps = {};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kSystemCaps, &caps);
    common::PFunUpdateCommonEmbeddedJSONConfig* updateCommonEmbeddedJSONConfig{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunUpdateCommonEmbeddedJSONConfig, &updateCommonEmbeddedJSONConfig);
    common::PFunNGXGetFeatureCaps* getFeatureRequirements{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunNGXGetFeatureRequirements, &getFeatureRequirements);

    // DLSS min driver
    sl::Version minDriver(512, 15, 0);

    common::PluginInfo info{};
    info.SHA = GIT_LAST_COMMIT_SHORT;
    info.minGPUArchitecture = NV_GPU_ARCHITECTURE_TU100;
    info.minOS = Version(10, 0, 0);
    info.needsNGX = true;
    info.requiredTags = { { kBufferTypeDepth, ResourceLifecycle::eValidUntilEvaluate}, {kBufferTypeMotionVectors, ResourceLifecycle::eValidUntilEvaluate},
                          { kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate}, { kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate} };

    if (caps && updateCommonEmbeddedJSONConfig && getFeatureRequirements)
    {
        // Ask NGX about min specs, if successful it will overwrite SL defaults
        if (!getFeatureRequirements(NVSDK_NGX_Feature_SuperSampling, info))
        {
            SL_LOG_WARN("Failed to obtain DLSS min spec requirements from NGX, using SL defaults");
        }
        updateCommonEmbeddedJSONConfig(&config, info);
    }

    auto& ctx = (*dlss::getContext());
    ctx.adapterMask = config.contains("supportedAdapters") ? config["supportedAdapters"].operator uint32_t() : 0;

    if (caps && ctx.adapterMask)
    {
        sl::Version detectedDriver(caps->driverVersionMajor, caps->driverVersionMinor, 0);
        if (detectedDriver < minDriver)
        {
            SL_LOG_WARN("sl.dlss requires driver %s or newer - detected %s - sl.dlss will be disabled", minDriver.toStr().c_str(), detectedDriver.toStr().c_str());
        }
        else
        {
            // We are supported, tell plugin manager what VK extension we need.
            // 
            // Note that at this point we know that we are on NVDA hardware with
            // driver which meets minimum spec so we know that all these extensions will work.
            std::unordered_set<std::string> instanceExtensions =
            {
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME
            };
            std::unordered_set<std::string> deviceExtensions =
            {
                VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
                VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
                VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
            };

            instanceExtensions.insert(info.vkInstanceExtensions.begin(), info.vkInstanceExtensions.end());
            deviceExtensions.insert(info.vkDeviceExtensions.begin(), info.vkDeviceExtensions.end());

            config["external"]["vk"]["instance"]["extensions"] = instanceExtensions;
            config["external"]["vk"]["device"]["extensions"] = deviceExtensions;

            config["external"]["vk"]["device"]["1.2_features"] = { "timelineSemaphore", "descriptorIndexing", "bufferDeviceAddress" };

            config["external"]["feature"]["viewport"]["maxCount"] = kMaxNumViewports;

            // Version
            config["external"]["version"]["sl"] = extra::format("{}.{}.{}", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
            common::PFunGetStringFromModule* func{};
            param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunGetStringFromModule, &func);
            if (func)
            {
                std::string ngxVersion;
                func("nvngx_dlss.dll", "FileVersion", ngxVersion);
                std::replace(ngxVersion.begin(), ngxVersion.end(), ',', '.');
                config["external"]["version"]["ngx"] = ngxVersion;
            }
        }
    }
    else
    {
        SL_LOG_WARN("sl.dlss not supported on current hardware");
    }
}

Result slGetData(const BaseStructure* inputs, BaseStructure* output, CommandBuffer* cmdBuffer);
Result slSetData(const BaseStructure* inputs, CommandBuffer* cmdBuffer)
{
    auto consts = findStruct<DLSSOptions>(inputs);
    auto viewport = findStruct<ViewportHandle>(inputs);

    if (!consts || !viewport)
    {
        SL_LOG_ERROR( "Invalid input data");
        return Result::eErrorMissingInputParameter;
    }

    auto& ctx = (*dlss::getContext());

    ctx.constsPerViewport.set(0, *viewport, consts);

    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (uint32_t)consts->dlaaPreset);
    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, (uint32_t)consts->qualityPreset);
    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, (uint32_t)consts->balancedPreset);
    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, (uint32_t)consts->performancePreset);
    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, (uint32_t)consts->ultraPerformancePreset);

    // NOTE: Nothing to do here when mode is set to off.
    // 
    // Host can use slFreeResources to release NGX instance if needed.
    // We show warning if evaluate is called while DLSSContext is off.

    return Result::eOk;
}

Result dlssBeginEvent(chi::CommandList pCmdList, const common::EventData& data, const sl::BaseStructure** inputs, uint32_t numInputs)
{
    auto parameters = api::getContext()->parameters;
    auto& ctx = (*dlss::getContext());

    // Disable DLSSContext by default
    ctx.viewport = {};

    if (!common::getConsts(data, &ctx.commonConsts))
    {
        // Can't find common constants, warn/error logged by the above function
        return Result::eErrorMissingConstants;
    }

    auto it = ctx.viewports.find(data.id);
    if (it == ctx.viewports.end())
    {
        ctx.viewports[data.id] = {};
    }
    
    if (ctx.viewports.size() > (size_t)kMaxNumViewports)
    {
        SL_LOG_WARN("Exceeded max number (%u) of allowed viewports for DLSS", kMaxNumViewports);
    }

    auto& viewport = ctx.viewports[data.id];
    viewport.id = data.id;

    // Our options are per viewport, frame index is just 0 always
    DLSSOptions* consts{};
    if (!ctx.constsPerViewport.get({ data.id, 0 }, &consts))
    {
        // Can't find DLSSContext constants, warn/error logged by the above function
        return Result::eErrorMissingConstants;
    }

    // Nothing to do if DLSSContext mode is set to off
    if (consts->mode == DLSSMode::eOff)
    {
        SL_LOG_WARN("DLSSOptions::mode is set to off, slEvaluateFeature(eDLSS) should not be called");
        return Result::eErrorInvalidIntegration;
    }

    // Must check here, before we overwrite viewport.consts
    bool modeOrSizeOrPresetChanged = consts->mode != viewport.consts.mode || consts->outputWidth != viewport.consts.outputWidth || consts->outputHeight != viewport.consts.outputHeight
        || consts->dlaaPreset != viewport.consts.dlaaPreset
        || consts->qualityPreset  != viewport.consts.qualityPreset 
        || consts->balancedPreset  != viewport.consts.balancedPreset 
        || consts->performancePreset  != viewport.consts.performancePreset 
        || consts->ultraPerformancePreset  != viewport.consts.ultraPerformancePreset;

    ctx.viewport = &viewport;
    viewport.consts = *consts;  // mandatory

    if(!viewport.handle || modeOrSizeOrPresetChanged)
    {
        ctx.commonConsts->reset = Boolean::eTrue;
        ctx.cachedStates.clear();
        slGetData(consts, &viewport.settings, pCmdList);

        if(ctx.ngxContext)
        {
            if (viewport.handle)
            {
                SL_LOG_INFO("Detected resize, recreating DLSSContext feature");
                // Errors logged by sl.common
                ctx.ngxContext->releaseFeature(viewport.handle, "sl.dlss");
                viewport.handle = {};
                ctx.compute->destroyResource(viewport.mvec);
            }

            {
                int dlssCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
                if (consts->colorBuffersHDR == Boolean::eTrue)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
                }
                if (consts->sharpness > 0.0f)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                }
                if (ctx.commonConsts->depthInverted == Boolean::eTrue)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
                }
                if (ctx.commonConsts->motionVectorsJittered == Boolean::eTrue)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
                }
                if (consts->structVersion >= kStructVersion3 && consts->alphaUpscalingEnabled == Boolean::eTrue)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
                }

                // Optional
                CommonResource exposure = {};
                getTaggedResource(kBufferTypeExposure, exposure, data.frame, ctx.viewport->id, true, inputs, numInputs);

                if ((ctx.viewport->consts.structVersion >= sl::kStructVersion2 && ctx.viewport->consts.useAutoExposure) || !exposure)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }

                // Mandatory
                CommonResource colorIn{};
                CommonResource colorOut{};
                CommonResource depth{};
                CommonResource mvec{};

                SL_CHECK(getTaggedResource(kBufferTypeScalingInputColor, colorIn, data.frame, ctx.viewport->id, false, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeScalingOutputColor, colorOut, data.frame, ctx.viewport->id, false, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeDepth, depth, data.frame, ctx.viewport->id, false, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeMotionVectors, mvec, data.frame, ctx.viewport->id, false, inputs, numInputs));

                auto colorInExt = colorIn.getExtent();
                auto colorOutExt = colorOut.getExtent();
                auto depthExt = depth.getExtent();
                auto mvecExt = mvec.getExtent();

                // We will log the extent information for easier debugging, if not specified assuming the full buffer size
                chi::ResourceDescription desc;
                if (!colorInExt)
                {
                    ctx.compute->getResourceState(colorIn.getState(), desc.state);
                    ctx.compute->getResourceDescription(colorIn, desc);
                    colorInExt = { 0,0,desc.width,desc.height };
                }
                if (!colorOutExt)
                {
                    ctx.compute->getResourceState(colorOut.getState(), desc.state);
                    ctx.compute->getResourceDescription(colorOut, desc);
                    colorOutExt = { 0,0,desc.width,desc.height };
                }
                if (!mvecExt)
                {
                    ctx.compute->getResourceState(mvec.getState(), desc.state);
                    ctx.compute->getResourceDescription(mvec, desc);
                    mvecExt = { 0,0,desc.width,desc.height };
                }
                if (!depthExt)
                {
                    ctx.compute->getResourceState(depth.getState(), desc.state);
                    ctx.compute->getResourceDescription(depth, desc);
                    depthExt = { 0,0,desc.width,desc.height };
                }

                if (mvecExt.width > colorInExt.width || mvecExt.height > colorInExt.height)
                {
                    SL_LOG_INFO("Detected high resolution mvec for DLSSContext");
                    dlssCreateFlags &= ~NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
                }

                NVSDK_NGX_PerfQuality_Value perfQualityValue = (NVSDK_NGX_PerfQuality_Value)((uint32_t)viewport.consts.mode - 1);

                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Width, viewport.settings.optimalRenderWidth);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Height, viewport.settings.optimalRenderHeight);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_OutWidth, viewport.consts.outputWidth);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_OutHeight, viewport.consts.outputHeight);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_PerfQualityValue, perfQualityValue);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dlssCreateFlags);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
                if (ctx.ngxContext->createFeature(pCmdList, NVSDK_NGX_Feature_SuperSampling, &viewport.handle, "sl.dlss"))
                {
                    SL_LOG_INFO("Created DLSSContext feature (%u,%u)(optimal) -> (%u,%u) for viewport %u", viewport.settings.optimalRenderWidth, viewport.settings.optimalRenderHeight, viewport.consts.outputWidth, viewport.consts.outputHeight, data.id);
                    // Log the extent information for easier debugging
                    SL_LOG_INFO("DLSSContext color_in extents (%u,%u,%u,%u)", colorInExt.left, colorInExt.top, colorInExt.width, colorInExt.height);
                    SL_LOG_INFO("DLSSContext color_out extents (%u,%u,%u,%u)", colorOutExt.left, colorOutExt.top, colorOutExt.width, colorOutExt.height);
                    SL_LOG_INFO("DLSSContext depth extents (%u,%u,%u,%u)", depthExt.left, depthExt.top, depthExt.width, depthExt.height);
                    SL_LOG_INFO("DLSSContext mvec extents (%u,%u,%u,%u)", mvecExt.left, mvecExt.top, mvecExt.width, mvecExt.height);
                }
            }
        }
    }
    return Result::eOk;
}

Result dlssEndEvent(chi::CommandList pCmdList, const common::EventData& data, const sl::BaseStructure** inputs, uint32_t numInputs)
{
    auto& ctx = (*dlss::getContext());
    if(ctx.viewport)
    {
        // Run DLSSContext, we skipped dispatch for in-engine TAAU
        auto parameters = api::getContext()->parameters;
        Constants* consts = ctx.commonConsts;
        
        {
            CommonResource colorIn{};
            CommonResource colorOut{};
            CommonResource depth{};
            CommonResource mvec{};
            CommonResource transparency{};
            CommonResource exposure{};
            CommonResource animTexture{};
            CommonResource mvecReflections{};
            CommonResource rayTraceDist{};
            CommonResource currentColorBias{};
            CommonResource particleMask{};

            // Mandatory
            SL_CHECK(getTaggedResource(kBufferTypeScalingInputColor, colorIn, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeScalingOutputColor, colorOut, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeDepth, depth, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeMotionVectors, mvec, data.frame, ctx.viewport->id, false, inputs, numInputs));

            // Optional
            getTaggedResource(kBufferTypeTransparencyHint, transparency, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeExposure, exposure, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeAnimatedTextureHint, animTexture, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeReflectionMotionVectors, mvecReflections, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeRaytracingDistance, rayTraceDist, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeBiasCurrentColorHint, currentColorBias, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeParticleHint, particleMask, data.frame, ctx.viewport->id, true, inputs, numInputs);

            if (!depth || !mvec || !colorIn || !colorOut)
            {
                SL_LOG_ERROR( "Missing DLSSContext inputs");
                return Result::eErrorMissingInputParameter;
            }

            sl::Extent colorInExt = colorIn.getExtent();
            sl::Extent colorOutExt = colorOut.getExtent();
            sl::Extent mvecExt = mvec.getExtent();
            sl::Extent depthExt = depth.getExtent();
            sl::Extent transparencyExt = transparency.getExtent();
            sl::Extent currentColorBiasExt = currentColorBias.getExtent();

#ifdef SL_CAPTURE

            // Capture
            if (extra::keyboard::getInterface()->wasKeyPressed("capture")) ctx.capture->startRecording("DLSSContext");

            if (ctx.capture->getIsCapturing()) {
                double time = ctx.capture->getTimeSinceStart();
                int captureIndex = ctx.capture->getCaptureIndex();

                ctx.capture->appendGlobalConstantDump(captureIndex, time, consts);

                const std::vector<int> dlssStructureSizes = std::vector<int>{ sizeof(sl::DLSSOptions) , sizeof(sl::DLSSOptimalSettings)};
                ctx.capture->appendFeatureStructureDump(captureIndex, 0, &ctx.viewport->consts, dlssStructureSizes[0]);
                ctx.capture->appendFeatureStructureDump(captureIndex, 1, &ctx.viewport->settings, dlssStructureSizes[1]);

                ctx.capture->dumpResource(captureIndex, kBufferTypeScalingInputColor,    colorInExt, pCmdList,   colorIn );
                ctx.capture->dumpResource(captureIndex, kBufferTypeDepth            ,    depthExt  , pCmdList,   depth   );
                ctx.capture->dumpResource(captureIndex, kBufferTypeMotionVectors             ,    mvecExt   , pCmdList,   mvec    );

                ctx.capture->incrementCaptureIndex();

            }

            if (ctx.capture->getIndexHasReachedMaxCapatureIndex()) ctx.capture->dumpPending();
#endif

            /*chi::ResourceDescription desc[4];
            ctx.compute->getResourceDescription(depth, desc[0]);
            ctx.compute->getResourceDescription(mvec, desc[1]);
            ctx.compute->getResourceDescription(colorIn, desc[2]);
            ctx.compute->getResourceDescription(colorOut, desc[3]);*/

            // Depending if camera motion is provided or not we can use input directly or not
            auto mvecIn = mvec;

#if SL_ENABLE_TIMING
            CHI_VALIDATE(ctx.compute->beginPerfSection(pCmdList, "sl.dlss"));
#endif

            {
                ctx.cacheState(colorIn, colorIn.getState());
                ctx.cacheState(colorOut, colorOut.getState());
                ctx.cacheState(depth, depth.getState());
                ctx.cacheState(mvecIn, mvec.getState());
                ctx.cacheState(transparency);
                ctx.cacheState(exposure, exposure.getState());
                ctx.cacheState(animTexture);
                ctx.cacheState(mvecReflections);
                ctx.cacheState(rayTraceDist);
                ctx.cacheState(currentColorBias, currentColorBias.getState());
                ctx.cacheState(particleMask);

                unsigned int renderWidth = colorInExt.width;
                unsigned int renderHeight = colorInExt.height;
                if (renderWidth == 0 || renderHeight == 0)
                {
                    chi::ResourceDescription desc;
                    ctx.compute->getResourceState(colorIn.getState(), desc.state);
                    ctx.compute->getResourceDescription(colorIn, desc);
                    renderWidth = desc.width;
                    renderHeight = desc.height;
                }

                bool mvecPixelSpace = false;

                if (consts->cameraMotionIncluded == Boolean::eFalse)
                {
                    // Need to compute camera motion ourselves

                    // TODO - this is not optimal in the case of dynamic resizing, but cameraMotionIncluded should be true for most existing DLSSContext titles.
                    // To optimize this, we would want to realloc only when the size is larger than we've seen before, and use subrects
                    if (ctx.viewport->mvec)
                    {
                        chi::ResourceDescription desc;
                        ctx.compute->getResourceDescription(ctx.viewport->mvec, desc);
                        if (desc.width != renderWidth || desc.height != renderHeight)
                        {
                            ctx.compute->destroyResource(ctx.viewport->mvec);
                            ctx.viewport->mvec = nullptr;
                        }
                    }
                    if (!ctx.viewport->mvec)
                    {
                        ctx.compute->beginVRAMSegment("sl.dlss");
                        sl::chi::ResourceDescription desc(renderWidth, renderHeight, sl::chi::eFormatRG16F,sl::chi::HeapType::eHeapTypeDefault, sl::chi::ResourceState::eTextureRead);
                        CHI_VALIDATE(ctx.compute->createTexture2D(desc, ctx.viewport->mvec, "sl.dlss.mvec"));
                        ctx.cacheState(ctx.viewport->mvec);
                        ctx.compute->endVRAMSegment();
                    }

                    mvecIn = ctx.viewport->mvec;

                    // In this case we will always convert to pixel space
                    mvecPixelSpace = true;

                    // No camera motion, need to compute ourselves and store in ctx.mvec
                    extra::ScopedTasks revTransitions;
                    chi::ResourceTransition transitions[] =
                    {
                        {mvecIn, chi::ResourceState::eStorageRW, ctx.cachedStates[mvecIn]}
                    };
                    ctx.compute->transitionResources(pCmdList, transitions, (uint32_t)countof(transitions), &revTransitions);
                    CHI_VALIDATE(ctx.compute->bindSharedState(pCmdList));

                    struct MVecParamStruct
                    {
                        sl::float4x4 clipToPrevClip;
                        sl::float4 texSize;
                        sl::float2 mvecScale;
                        uint32_t debug;
                    };
                    MVecParamStruct cb;
                    cb.texSize.x = (float)renderWidth;
                    cb.texSize.y = (float)renderHeight;
                    cb.texSize.z = 1.0f / renderWidth;
                    cb.texSize.w = 1.0f / renderHeight;
                    cb.mvecScale = { consts->mvecScale.x, consts->mvecScale.y }; // scaling everything to -1,1 range then to -width,width
                    cb.debug = 0;
                    memcpy(&cb.clipToPrevClip, &consts->clipToPrevClip, sizeof(float) * 16);
                    CHI_VALIDATE(ctx.compute->bindKernel(ctx.mvecKernel));
                    CHI_VALIDATE(ctx.compute->bindTexture(0, 0, mvec));
                    CHI_VALIDATE(ctx.compute->bindTexture(1, 1, depth));
                    CHI_VALIDATE(ctx.compute->bindRWTexture(2, 0, ctx.viewport->mvec));
                    CHI_VALIDATE(ctx.compute->bindConsts(3, 0, &cb, sizeof(MVecParamStruct), kMaxNumViewports * 3));
                    uint32_t grid[] = { (renderWidth + 16 - 1) / 16, (renderHeight + 16 - 1) / 16, 1 };
                    CHI_VALIDATE(ctx.compute->dispatch(grid[0], grid[1], grid[2]));
                }
#if 0
                if (ctx.viewport->output)
                {
                    chi::ResourceDescription desc;
                    ctx.compute->getResourceDescription(ctx.viewport->output, desc);
                    if (desc.width != colorOutExt.width || desc.height != colorOutExt.height)
                    {
                        ctx.compute->destroyResource(ctx.viewport->output);
                        ctx.viewport->output = nullptr;
                    }
                }

                if (!ctx.viewport->output)
                {
                    ctx.compute->beginVRAMSegment("sl.dlss");
                    ctx.compute->cloneResource(colorOut, ctx.viewport->output, "sl.dlss.output-cached", chi::ResourceState::eStorageRW);
                    ctx.cacheState(ctx.viewport->output);
                    ctx.compute->endVRAMSegment();
                }
#endif

                if (ctx.ngxContext)
                {
                    // DLSSContext
                    extra::ScopedTasks revTransitions;
                    chi::ResourceTransition transitions[] =
                    {
                        {mvecIn, chi::ResourceState::eTextureRead, ctx.cachedStates[mvecIn]},
                        {depth, chi::ResourceState::eTextureRead, ctx.cachedStates[depth]},
                        {colorIn, chi::ResourceState::eTextureRead, ctx.cachedStates[colorIn]},
                        {colorOut, chi::ResourceState::eStorageRW, ctx.cachedStates[colorOut]},
                        {transparency, chi::ResourceState::eTextureRead, ctx.cachedStates[transparency]},
                        {exposure, chi::ResourceState::eTextureRead, ctx.cachedStates[exposure]},
                        {rayTraceDist, chi::ResourceState::eTextureRead, ctx.cachedStates[rayTraceDist]},
                        {animTexture, chi::ResourceState::eTextureRead, ctx.cachedStates[animTexture]},
                        {mvecReflections, chi::ResourceState::eTextureRead, ctx.cachedStates[mvecReflections]},
                        {particleMask, chi::ResourceState::eTextureRead, ctx.cachedStates[particleMask]},
                        {currentColorBias, chi::ResourceState::eTextureRead, ctx.cachedStates[currentColorBias]},
                    };
                    ctx.compute->transitionResources(pCmdList, transitions, (uint32_t)countof(transitions), &revTransitions);

                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Reset, consts->reset == Boolean::eTrue);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MV_Scale_X, (mvecPixelSpace ? 1.0f : (float)(consts->mvecScale.x * renderWidth)));
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, (mvecPixelSpace ? 1.0f : (float)(consts->mvecScale.y * renderHeight)));
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, consts->jitterOffset.x);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, consts->jitterOffset.y);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, ctx.viewport->consts.preExposure);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, ctx.viewport->consts.exposureScale);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Sharpness, ctx.viewport->consts.sharpness);

                    if (ctx.platform == RenderAPI::eVulkan)
                    {
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Color, ctx.cachedVkResource(colorIn));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectors, ctx.cachedVkResource(mvecIn));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Output, ctx.cachedVkResource(colorOut));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_TransparencyMask, ctx.cachedVkResource(transparency));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_ExposureTexture, ctx.cachedVkResource(exposure));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Depth, ctx.cachedVkResource(depth));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, ctx.cachedVkResource(currentColorBias));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, ctx.cachedVkResource(animTexture));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayTracingHitDistance, ctx.cachedVkResource(rayTraceDist));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, ctx.cachedVkResource(mvecReflections));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_IsParticleMask, ctx.cachedVkResource(particleMask));
                    }
                    else
                    {
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Color, (void*)colorIn);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectors, (void*)mvecIn);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Output, (void*)colorOut);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_TransparencyMask, (void*)transparency);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*)exposure);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Depth, (void*)depth);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void*)currentColorBias);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, (void*)animTexture);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayTracingHitDistance, (void*)rayTraceDist);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, (void*)mvecReflections);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_IsParticleMask, (void*)particleMask);
                    }

                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, colorInExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, colorInExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, depthExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depthExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, mvecExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, mvecExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, transparencyExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, transparencyExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, currentColorBiasExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, currentColorBiasExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, colorOutExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, colorOutExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, renderWidth);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, renderHeight);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, ctx.viewport->consts.indicatorInvertAxisX);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, ctx.viewport->consts.indicatorInvertAxisY);

                    ctx.ngxContext->evaluateFeature(pCmdList, ctx.viewport->handle, "sl.dlss");

#if 0
                    {
                        // Cache the output
                        extra::ScopedTasks revTransitions;
                        chi::ResourceTransition transitions[] =
                        {
                            {ctx.viewport->output, chi::ResourceState::eCopyDestination, ctx.viewport->output->state},
                            {colorOut, chi::ResourceState::eCopySource, colorOut.getState()},
                        };
                        ctx.compute->transitionResources(pCmdList, transitions, (uint32_t)countof(transitions), &revTransitions);
                        ctx.compute->copyResource(pCmdList, ctx.viewport->output, colorOut);
                    }
#endif
                }

                float ms = 0;
#if SL_ENABLE_TIMING
                CHI_VALIDATE(ctx.compute->endPerfSection(pCmdList, "sl.dlss", ms));
#endif

#ifndef SL_PRODUCTION
                /*static std::string s_stats;
                auto v = api::getContext()->pluginVersion;
                
                s_stats = extra::format("sl.dlss {} - NGX {} - ({}x{})->({}x{}) - {}ms", v.toStr() + "." + GIT_LAST_COMMIT_SHORT, ctx.ngxVersion,
                    renderWidth, renderHeight, ctx.viewport->consts.outputWidth, ctx.viewport->consts.outputHeight, ms);
                parameters->set(sl::param::dlss::kStats, (void*)s_stats.c_str());*/

                {
                    uint64_t bytes;
                    ctx.compute->getAllocatedBytes(bytes, "sl.dlss");
                    std::scoped_lock lock(ctx.uiStats.mtx);
                    ctx.uiStats.mode = getDLSSModeAsStr(ctx.viewport->consts.mode);
                    ctx.uiStats.viewport = extra::format("Viewport {}x{} -> {}x{}", renderWidth, renderHeight, ctx.viewport->consts.outputWidth, ctx.viewport->consts.outputHeight);
                    ctx.uiStats.runtime = extra::format("{}ms", ms);
                    ctx.uiStats.vram = extra::format("{}GB", bytes / (1024.0 * 1024.0 * 1024.0));
                }
#endif

                uint32_t frame = 0;
                ctx.compute->getFinishedFrameIndex(frame);
                parameters->set(sl::param::dlss::kCurrentFrame, frame + 1);
            }
        }
    }
    return Result::eOk;
}

//! -------------------------------------------------------------------------------------------------
//! Required interface

Result slGetData(const BaseStructure *inputs, BaseStructure *output, CommandBuffer* cmdBuffer)
{
    auto parameters = api::getContext()->parameters;
    auto& ctx = (*dlss::getContext());

    getPointerParam(parameters, param::global::kNGXContext, &ctx.ngxContext);
    if (!ctx.ngxContext)
    {
        SL_LOG_ERROR( "NGX context is missing, please make sure DLSSContext feature is enabled and supported on the platform");
        return Result::eErrorMissingOrInvalidAPI;
    }
    auto state = findStruct<DLSSState>(output);
    auto settings = findStruct<DLSSOptimalSettings>(output);
    auto consts = findStruct<const DLSSOptions>(inputs);

    if ((!consts || !settings) && !state)
    {
        SL_LOG_ERROR( "Invalid input data");
        return Result::eErrorMissingInputParameter;
    }

    // Settings
    if (consts && settings)
    {
        void* callback = NULL;
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
        if (!callback)
        {
            SL_LOG_ERROR( "DLSSContext 'getOptimalSettings' callback is missing, please make sure DLSSContext feature is up to date");
            return Result::eErrorNGXFailed;
        }

        // These are selections made by user in UI
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Width, consts->outputWidth);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Height, consts->outputHeight);
        // SL DLSSContext modes start with 'off' so subtract one, the rest is mapped 1:1
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (NVSDK_NGX_PerfQuality_Value)((uint32_t)consts->mode - 1));
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RTXValue, false);

        NVSDK_NGX_Result res = NVSDK_NGX_Result_Success;
        auto getOptimalSettings = (PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback;
        res = getOptimalSettings(ctx.ngxContext->params);
        if (NVSDK_NGX_FAILED(res))
        {
            SL_LOG_ERROR( "DLSSContext 'getOptimalSettings' callback failed - error %u", res);
            return Result::eErrorNGXFailed;
        }
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_OutWidth, &settings->optimalRenderWidth);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_OutHeight, &settings->optimalRenderHeight);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_Sharpness, &settings->optimalSharpness);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &settings->renderWidthMax);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &settings->renderHeightMax);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &settings->renderWidthMin);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &settings->renderHeightMin);
    }
    
    // Stats
    if(state)
    {
        void* callback = NULL;
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSSGetStatsCallback, &callback);
        if (!callback)
        {
            SL_LOG_ERROR( "DLSSContext 'getStats' callback is missing, please make sure DLSSContext feature is up to date");
            return Result::eErrorNGXFailed;
        }
        auto getStats = (PFN_NVSDK_NGX_DLSS_GetStatsCallback)callback;
        auto res = getStats(ctx.ngxContext->params);
        if (NVSDK_NGX_FAILED(res))
        {
            SL_LOG_ERROR( "DLSSContext 'getStats' callback failed - error %u", res);
            return Result::eErrorNGXFailed;
        }
        // TODO: This has to return the correct estimate regardless if callback is present or not.
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_SizeInBytes, &state->estimatedVRAMUsageInBytes);
    }
    return Result::eOk;
}

Result slAllocateResources(sl::CommandBuffer* cmdBuffer, Feature feature, const sl::ViewportHandle& viewport)
{
    auto& ctx = (*dlss::getContext());
    common::EventData data{ viewport, 0 };
    dlssBeginEvent(cmdBuffer, data, nullptr, 0);
    auto it = ctx.viewports.find(viewport);
    return it != ctx.viewports.end() && (*it).second.handle != nullptr ? Result::eOk : Result::eErrorInvalidParameter;
}

Result slFreeResources(Feature feature, const sl::ViewportHandle& viewport)
{
    auto& ctx = (*dlss::getContext());
    auto it = ctx.viewports.find(viewport);
    if (it != ctx.viewports.end())
    {
        auto& instance = (*it).second;
        if (instance.handle)
        {
            SL_LOG_INFO("Releasing DLSSContext instance id %u", viewport);
            ctx.ngxContext->releaseFeature(instance.handle, "sl.dlss");
            // OK to release null resources
            CHI_VALIDATE(ctx.compute->destroyResource(instance.mvec));
            CHI_VALIDATE(ctx.compute->destroyResource(instance.output));
        }
        ctx.viewports.erase(it);
        return Result::eOk;
    }
    return Result::eErrorInvalidParameter;
}

//! Plugin startup
//!
//! Called only if plugin reports `supported : true` in the JSON config.
//! Note that supported flag can flip back to false if this method fails.
//!
//! @param device Either ID3D12Device or struct VkDevices (see internal.h)
bool slOnPluginStartup(const char* jsonConfig, void* device)
{
    SL_PLUGIN_COMMON_STARTUP();

    auto& ctx = (*dlss::getContext());

    auto parameters = api::getContext()->parameters;

    getPointerParam(parameters, param::global::kNGXContext, &ctx.ngxContext);

    if (!ctx.ngxContext)
    {
        SL_LOG_ERROR( "Missing NGX context - DLSSContext cannot run");
        return false;
    }

    if (!ctx.ngxContext->params)
    {
        SL_LOG_ERROR( "Missing NGX default parameters - DLSSContext cannot run");
        return false;
    }

    int supported = 0;
    NVSDK_NGX_Result ngxResult = ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    if (NVSDK_NGX_FAILED(ngxResult))
    {
        SL_LOG_ERROR( "NGX parameter indicating DLSSContext support cannot be found (0x%x) - DLSSContext cannot run", ngxResult);
        return false;
    }

    if (!supported)
    {
        SL_LOG_ERROR( "NGX indicates DLSSContext is not available - DLSSContext cannot run");
        return false;
    }

    // Register our event callbacks
    if (!param::getPointerParam(parameters, param::common::kPFunRegisterEvaluateCallbacks, &ctx.registerEvaluateCallbacks))
    {
        SL_LOG_ERROR( "Cannot obtain `registerEvaluateCallbacks` interface - check that sl.common was initialized correctly");
        return false;
    }
    ctx.registerEvaluateCallbacks(kFeatureDLSS, dlssBeginEvent, dlssEndEvent);

    param::getPointerParam(parameters, sl::param::common::kComputeAPI, &ctx.compute);

#ifdef SL_CAPTURE
    extra::keyboard::getInterface()->registerKey("capture", extra::keyboard::VirtKey('U', true, true));
    param::getPointerParam(parameters, sl::param::common::kCaptureAPI, &ctx.capture);
#endif

    {
        json& config = *(json*)api::getContext()->loaderConfig;
        int appId = 0;
        config.at("appId").get_to(appId);
    }
    
    ctx.compute->getRenderAPI(ctx.platform);
    if (ctx.platform == RenderAPI::eVulkan)
    {
        CHI_CHECK_RF(ctx.compute->createKernel((void*)mvec_spv, mvec_spv_len, "mvec.cs", "main", ctx.mvecKernel));
    }
    else
    {
        CHI_CHECK_RF(ctx.compute->createKernel((void*)mvec_cs, mvec_cs_len, "mvec.cs", "main", ctx.mvecKernel));
    }

    // Update our feature if update is available and host opted in
    ctx.ngxContext->updateFeature(NVSDK_NGX_Feature_SuperSampling);

    parameters->set(internal::shared::getParameterNameForFeature(kFeatureDLSS).c_str(), (void*)getSharedData);
#ifndef SL_PRODUCTION
    common::PFunGetStringFromModule* func{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunGetStringFromModule, &func);
    if(func)
    {
        func("nvngx_dlss.dll", "FileVersion", ctx.ngxVersion);
        std::replace(ctx.ngxVersion.begin(), ctx.ngxVersion.end(), ',', '.');
    }

    // Check for UI and register our callback
    imgui::ImGUI* ui{};
    param::getPointerParam(parameters, param::imgui::kInterface, &ui);
    if (ui)
    {
        // Runs async from the present thread where UI is rendered just before frame is presented
        auto renderUI = [&ctx](imgui::ImGUI* ui, bool finalFrame)->void
        {
            imgui::Float4 greenColor{ 0,1,0,1 };
            imgui::Float4 highlightColor{ 153.0f / 255.0f, 217.0f / 255.0f, 234.0f / 255.0f,1 };

            auto v = api::getContext()->pluginVersion;
            std::scoped_lock lock(ctx.uiStats.mtx);
            uint32_t lastFrame, frame;
            if (api::getContext()->parameters->get(sl::param::dlss::kCurrentFrame, &lastFrame))
            {
                ctx.compute->getFinishedFrameIndex(frame);
                if (lastFrame < frame)
                {
                    ctx.uiStats.mode = "Mode: Off";
                    ctx.uiStats.viewport = ctx.uiStats.runtime = {};
                }
                if (ui->collapsingHeader(extra::format("sl.dlss v{}", (v.toStr() + "." + GIT_LAST_COMMIT_SHORT)).c_str(), imgui::kTreeNodeFlagDefaultOpen))
                {
                    ui->text("NGX v%s ", ctx.ngxVersion.c_str());
                    ui->text(ctx.uiStats.mode.c_str());
                    if (!ctx.uiStats.viewport.empty())
                    {
                        ui->text(ctx.uiStats.viewport.c_str());
                        ui->labelColored(greenColor, "Execution time: ", "%s", ctx.uiStats.runtime.c_str());
                        ui->labelColored(highlightColor, "VRAM: ", "%s", ctx.uiStats.vram.c_str());
                    }
                }
            }
        };
        ui->registerRenderCallbacks(renderUI, nullptr);
    }
#endif

    return true;
}

//! Plugin shutdown
//!
//! Called by loader when unloading the plugin
void slOnPluginShutdown()
{
    auto& ctx = (*dlss::getContext());

    ctx.registerEvaluateCallbacks(kFeatureDLSS, nullptr, nullptr);

    // Common shutdown
    plugin::onShutdown(api::getContext());

    for(auto v : ctx.viewports)
    {
        ctx.ngxContext->releaseFeature(v.second.handle, "sl.dlss");
        CHI_VALIDATE(ctx.compute->destroyResource(v.second.mvec));
    }
    CHI_VALIDATE(ctx.compute->destroyKernel(ctx.mvecKernel));
}

sl::Result slDLSSGetOptimalSettings(const sl::DLSSOptions& options, sl::DLSSOptimalSettings& settings)
{
    return slGetData(&options, &settings, nullptr);
}

sl::Result slDLSSGetState(const sl::ViewportHandle& viewport, sl::DLSSState& state)
{
    return slGetData(&viewport, &state, nullptr);
}

sl::Result slDLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options)
{
    auto v = viewport;
    v.next = (sl::BaseStructure*)&options;
    return slSetData(&v, nullptr);
}

sl::Result slIsSupported(const sl::AdapterInfo& adapterInfo)
{
    auto& ctx = (*dlss::getContext());
    sl::common::PFunFindAdapter* findAdapter{};
    param::getPointerParam(api::getContext()->parameters, param::common::kPFunFindAdapter, &findAdapter);
    if (!findAdapter)
    {
        SL_LOG_ERROR("sl.common not loaded");
        return Result::eErrorFeatureMissing;
    }
    return findAdapter(adapterInfo, ctx.adapterMask);
}

internal::shared::Status getSharedData(BaseStructure* requestedData, const BaseStructure* requesterInfo)
{
    if (!requestedData || requestedData->structType != dlss::DLSSInternalSharedData::s_structType)
    {
        SL_LOG_ERROR("Invalid request is made for shared data");
        return internal::shared::Status::eInvalidRequestedData;
    }
    auto& ctx = (*dlss::getContext());

    auto remote = static_cast<dlss::DLSSInternalSharedData*>(requestedData);
    
    // v1
    remote->cachedUpscaledOutput = ctx.viewport ? ctx.viewport->output : nullptr;
    
    //! Make sure we let newer requester know what our current version is and what data is available
    if(remote->structVersion > kStructVersion1)
    {
        remote->structVersion = kStructVersion1;
    }

    return internal::shared::Status::eOk;
}

SL_EXPORT void *slGetPluginFunction(const char *functionName)
{
    // Core API
    SL_EXPORT_FUNCTION(slOnPluginLoad);
    SL_EXPORT_FUNCTION(slOnPluginShutdown);
    SL_EXPORT_FUNCTION(slOnPluginStartup);
    SL_EXPORT_FUNCTION(slSetData);
    SL_EXPORT_FUNCTION(slGetData);
    SL_EXPORT_FUNCTION(slAllocateResources);
    SL_EXPORT_FUNCTION(slFreeResources);
    SL_EXPORT_FUNCTION(slIsSupported);

    SL_EXPORT_FUNCTION(slDLSSSetOptions);
    SL_EXPORT_FUNCTION(slDLSSGetOptimalSettings);
    SL_EXPORT_FUNCTION(slDLSSGetState);    

    return nullptr;
}

}
