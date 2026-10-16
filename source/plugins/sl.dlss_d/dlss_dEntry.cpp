/*
* Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
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
#include "include/sl_dlss_d.h"
#include "include/sl_struct.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/core/sl.file/file.h"
#include "source/core/sl.extra/extra.h"
#include "source/core/sl.param/parameters.h"
#include "external/json/include/nlohmann/json.hpp"
#include "nvapi.h"

#include "source/platforms/sl.chi/vulkan.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "source/plugins/sl.dlss_d/versions.h"
#include "source/plugins/sl.imgui/imgui.h"

#include "_artifacts/shaders/mvec_cs.h"
#include "_artifacts/shaders/mvec_spv.h"
#include "_artifacts/json/dlss_d_json.h"
#include "_artifacts/gitVersion.h"


#include "external/ngx-sdk/include/nvsdk_ngx.h"
#include "external/ngx-sdk/include/nvsdk_ngx_helpers.h"
#include "external/ngx-sdk/include/nvsdk_ngx_helpers_vk.h"
#include "external/ngx-sdk/include/nvsdk_ngx_defs_dlssd.h"

using json = nlohmann::json;

namespace sl
{

using funNGXInit = NVSDK_NGX_Result(*)(unsigned long long InApplicationId, const wchar_t *InApplicationDataPath, ID3D12Device *InDevice, NVSDK_NGX_Version InSDKVersion);
using funNGXShutdown = NVSDK_NGX_Result(*)(void);
using funNGXCreate = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList *InCmdList, NVSDK_NGX_Feature InFeatureId, const NVSDK_NGX_Parameter *InParameters, NVSDK_NGX_Handle **OutHandle);
using funNGXRelease = NVSDK_NGX_Result(*)(NVSDK_NGX_Handle *InHandle);
using funNGXEval = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList *InCmdList, const NVSDK_NGX_Handle *InHandle, const NVSDK_NGX_Parameter *InParameters, PFN_NVSDK_NGX_ProgressCallback InCallback);

struct DLSSDViewport
{
    uint32_t id = {};
    DLSSDOptions consts{};
    DLSSDOptimalSettings settings;
    NVSDK_NGX_Handle* handle = {};
    sl::chi::Resource mvec;
    float2 inputTexelSize;
};

struct UIStats
{
    std::mutex mtx{};
    std::string mode{};
    std::string viewport{};
    std::string runtime{};
    std::string vram{};
};

namespace dlss_d
{
struct DLSSDContext
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(DLSSDContext);
    void onCreateContext() {};
    void onDestroyContext() {};

    std::future<bool> initLambda;

    Constants* commonConsts{};

    UIStats uiStats{};

    uint32_t adapterMask{};

    common::NGXContext* ngxContext = {};
    sl::chi::ICompute* compute;
    sl::chi::Kernel mvecKernel;

#ifndef SL_PRODUCTION
    std::string ngxVersion{};
#endif

    common::PFunRegisterEvaluateCallbacks* registerEvaluateCallbacks{};
    common::ViewportIdFrameData<4, false> constsPerViewport = { "dlss_d" };
    std::map<void*, chi::ResourceState> cachedStates = {};
    std::map<void*, NVSDK_NGX_Resource_VK> cachedVkResources = {};
    std::map<uint32_t, DLSSDViewport> viewports = {};
    DLSSDViewport* viewport = {};

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
                    ngx.Resource.ImageViewInfo.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
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

static std::string JSON = std::string(dlss_d_json, &dlss_d_json[dlss_d_json_len]);

void updateEmbeddedJSON(json& config);

SL_PLUGIN_DEFINE("sl.dlss_d", Version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH), Version(0, 0, 1), JSON.c_str(), updateEmbeddedJSON, dlss_d, DLSSDContext)

void updateEmbeddedJSON(json& config)
{
    // Check if plugin is supported or not on this platform and set the flag accordingly
    common::SystemCaps* caps = {};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kSystemCaps, &caps);
    common::PFunUpdateCommonEmbeddedJSONConfig* updateCommonEmbeddedJSONConfig{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunUpdateCommonEmbeddedJSONConfig, &updateCommonEmbeddedJSONConfig);
    common::PFunNGXGetFeatureCaps* getFeatureRequirements{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunNGXGetFeatureRequirements, &getFeatureRequirements);

    // DLSSD min driver
    // ngx_core's getFeatureRequirements implmentation has a bug so it can cause a crash when called with the new dlssd feature enum
    // It's fixed in 535.68. Before the fix, dlssd feature is enabled for a short time, enabled in 535.15 and disabled in 535.59. So we don't filter out thos drivers as well.
    // So, getFeatureRequirements is working with 
    // 1) a driver between 535.15 and 535.58 (inclusive)
    // 2) a driver >= 535.68

    sl::Version minDriver(535, 68, 0);
    sl::Version minDriverFeatureEnabled(535, 15, 0);
    sl::Version maxDriverFeatureEnabled(535, 58, 0);
    sl::Version detectedDriver(caps->driverVersionMajor, caps->driverVersionMinor, 0);

    common::PluginInfo info{};
    info.SHA = GIT_LAST_COMMIT_SHORT;
    info.minGPUArchitecture = NV_GPU_ARCHITECTURE_TU100;
    info.minOS = Version(10, 0, 0);
    info.needsNGX = true;
    info.requiredTags = { { kBufferTypeDepth, ResourceLifecycle::eValidUntilEvaluate}, {kBufferTypeMotionVectors, ResourceLifecycle::eValidUntilEvaluate},
                          { kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate}, { kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate} };
    info.minDriver = minDriverFeatureEnabled;

    config["external"]["feature"]["supported"] = true;
    if (caps && updateCommonEmbeddedJSONConfig && getFeatureRequirements)
    {
        bool supported = false;
        if (!((detectedDriver >= minDriverFeatureEnabled && detectedDriver <= maxDriverFeatureEnabled) || detectedDriver >= minDriver))
        {
            SL_LOG_WARN("%s requires a driver supporting DLSSD. Please update driver.", api::getPluginName());
        }
        else
        {
            if (!getFeatureRequirements(NVSDK_NGX_Feature_RayReconstruction, info))
            {
                SL_LOG_WARN("DLSSD feature is not supported. Please check if you have a valid nvngx_dlssd.dll or your driver is supporting DLSSD.");
            }
            else
            {
                supported = true;
            }
        }

        updateCommonEmbeddedJSONConfig(&config, info);
        if (!supported) config["external"]["feature"]["supported"] = false;

        auto& ctx = (*dlss_d::getContext());
        ctx.adapterMask = config.contains("supportedAdapters") ? config["supportedAdapters"].operator uint32_t() : 0;

        if (ctx.adapterMask && supported)
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
                func("nvngx_dlssd.dll", "FileVersion", ngxVersion);
                std::replace(ngxVersion.begin(), ngxVersion.end(), ',', '.');
                config["external"]["version"]["ngx"] = ngxVersion;
            }
        }
    }
}

Result slGetData(const BaseStructure* inputs, BaseStructure* output, CommandBuffer* cmdBuffer);
Result slSetData(const BaseStructure* inputs, CommandBuffer* cmdBuffer)
{
    auto consts = findStruct<DLSSDOptions>(inputs);
    auto viewport = findStruct<ViewportHandle>(inputs);

    if (!consts || !viewport)
    {
        SL_LOG_ERROR( "Invalid input data");
        return Result::eErrorMissingInputParameter;
    }

    auto& ctx = (*dlss_d::getContext());

    ctx.constsPerViewport.set(0, *viewport, consts);

    if (consts->structVersion >= kStructVersion3)
    {
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, (uint32_t)consts->dlaaPreset);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, (uint32_t)consts->qualityPreset);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, (uint32_t)consts->balancedPreset);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, (uint32_t)consts->performancePreset);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, (uint32_t)consts->ultraPerformancePreset);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, (uint32_t)consts->ultraQualityPreset);
    }
    
    // NOTE: Nothing to do here when mode is set to off.
    // 
    // Host can use slFreeResources to release NGX instance if needed.
    // We show warning if evaluate is called while DLSSDContext is off.

    return Result::eOk;
}

Result dlssdBeginEvent(chi::CommandList pCmdList, const common::EventData& data, const sl::BaseStructure** inputs, uint32_t numInputs)
{
    auto parameters = api::getContext()->parameters;
    auto& ctx = (*dlss_d::getContext());

    // Disable DLSSDContext by default
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
        SL_LOG_WARN("Exceeded max number (%u) of allowed viewports for DLSS_D", kMaxNumViewports);
    }

    auto& viewport = ctx.viewports[data.id];
    viewport.id = data.id;

    // Our options are per viewport, frame index is just 0 always
    DLSSDOptions* consts{};
    if (!ctx.constsPerViewport.get({ data.id, 0 }, &consts))
    {
        // Can't find DLSSDContext constants, warn/error logged by the above function
        return Result::eErrorMissingConstants;
    }

    // Nothing to do if DLSSDContext mode is set to off
    if (consts->mode == DLSSMode::eOff)
    {
        SL_LOG_WARN("DLSSDOptions::mode is set to off, slEvaluateFeature(eDLSS_D) should not be called");
        return Result::eErrorInvalidIntegration;
    }

    // Must check here, before we overwrite viewport.consts
    bool modeOrSizeChanged = consts->mode != viewport.consts.mode || consts->outputWidth != viewport.consts.outputWidth || consts->outputHeight != viewport.consts.outputHeight || consts->normalRoughnessMode != viewport.consts.normalRoughnessMode;
    if (consts->structVersion >= kStructVersion3)
    {
        modeOrSizeChanged = modeOrSizeChanged  ||
                            consts->dlaaPreset != viewport.consts.dlaaPreset ||
                            consts->qualityPreset != viewport.consts.qualityPreset ||
                            consts->balancedPreset != viewport.consts.balancedPreset ||
                            consts->performancePreset != viewport.consts.performancePreset ||
                            consts->ultraPerformancePreset != viewport.consts.ultraPerformancePreset ||
                            consts->ultraQualityPreset != viewport.consts.ultraQualityPreset;
    }

    ctx.viewport = &viewport;
    viewport.consts = *consts;  // mandatory

    if(!viewport.handle || modeOrSizeChanged)
    {
        ctx.commonConsts->reset = Boolean::eTrue;
        ctx.cachedStates.clear();
        slGetData(consts, &viewport.settings, pCmdList);

        if(ctx.ngxContext)
        {
            if (viewport.handle)
            {
                SL_LOG_INFO("Detected resize, recreating DLSSDContext feature");
                // Errors logged by sl.common

                // Delayed releaseFeature call by 3 frames to avoid race conditions
                auto releaseFeatureLambda = [&ctx, handle = viewport.handle](void) -> void {
                    ctx.ngxContext->releaseFeature(handle, api::getPluginName());
                };
                CHI_VALIDATE(ctx.compute->destroy(releaseFeatureLambda, 3));

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
                if (consts->structVersion >= kStructVersion2 && consts->alphaUpscalingEnabled == Boolean::eTrue)
                {
                    dlssCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
                }

                // Mandatory
                CommonResource colorIn{};
                CommonResource colorOut{};
                CommonResource linearDepth{};
                CommonResource hwDepth{};
                CommonResource mvec{};

                SL_CHECK(getTaggedResource(kBufferTypeScalingInputColor, colorIn, data.frame, ctx.viewport->id, false, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeScalingOutputColor, colorOut, data.frame, ctx.viewport->id, false, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeLinearDepth, linearDepth, data.frame, ctx.viewport->id, true, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeDepth, hwDepth, data.frame, ctx.viewport->id, true, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeMotionVectors, mvec, data.frame, ctx.viewport->id, false, inputs, numInputs));

                CommonResource& depth = linearDepth ? linearDepth : hwDepth;

                if (!depth)
                {
                    SL_LOG_ERROR("Missing depth input. You need to tag kBufferTypeLinearDepth or kBufferTypeDepth.");
                    return Result::eErrorMissingInputParameter;
                }

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
                    SL_LOG_INFO("Detected high resolution mvec for DLSSDContext");
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
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, 
                    viewport.consts.normalRoughnessMode == DLSSDNormalRoughnessMode::eUnpacked ? NVSDK_NGX_DLSS_Roughness_Mode_Unpacked : NVSDK_NGX_DLSS_Roughness_Mode_Packed);
                ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Use_HW_Depth, linearDepth ? NVSDK_NGX_DLSS_Depth_Type_Linear : NVSDK_NGX_DLSS_Depth_Type_HW);

                if (ctx.ngxContext->createFeature(pCmdList, NVSDK_NGX_Feature_RayReconstruction, &viewport.handle, api::getPluginName()))
                {
                    SL_LOG_INFO("Created DLSSDContext feature (%u,%u)(optimal) -> (%u,%u) for viewport %u", viewport.settings.optimalRenderWidth, viewport.settings.optimalRenderHeight, viewport.consts.outputWidth, viewport.consts.outputHeight, data.id);
                    // Log the extent information for easier debugging
                    SL_LOG_INFO("DLSSDContext color_in extents (%u,%u,%u,%u)", colorInExt.left, colorInExt.top, colorInExt.width, colorInExt.height);
                    SL_LOG_INFO("DLSSDContext color_out extents (%u,%u,%u,%u)", colorOutExt.left, colorOutExt.top, colorOutExt.width, colorOutExt.height);
                    SL_LOG_INFO("DLSSDContext depth extents (%u,%u,%u,%u)", depthExt.left, depthExt.top, depthExt.width, depthExt.height);
                    SL_LOG_INFO("DLSSDContext mvec extents (%u,%u,%u,%u)", mvecExt.left, mvecExt.top, mvecExt.width, mvecExt.height);
                }
                else
                {
                    return Result::eErrorNGXFailed;
                }
            }
        }
    }
    return Result::eOk;
}

Result dlssdEndEvent(chi::CommandList pCmdList, const common::EventData& data, const sl::BaseStructure** inputs, uint32_t numInputs)
{
    auto& ctx = (*dlss_d::getContext());
    if(ctx.viewport)
    {
        // Run DLSSDContext, we skipped dispatch for in-engine TAAU
        auto parameters = api::getContext()->parameters;
        Constants* consts = ctx.commonConsts;
        
        {
            CommonResource colorIn{};
            CommonResource colorOut{};
            CommonResource linearDepth{};
            CommonResource hwDepth{};
            CommonResource mvec{};
            CommonResource albedo{};
            CommonResource specularAlbedo{};
            CommonResource normals{};
            CommonResource roughness{};
            CommonResource reflectedAlbedo{};
            CommonResource colorBeforeParticles{};
            CommonResource colorBeforeTransparency{};
            CommonResource colorBeforeFog{};
            CommonResource diffuseHitDistance{};
            CommonResource specularHitDistance{};
            CommonResource diffuseRayDirection{};
            CommonResource specularRayDirection{};
            CommonResource diffuseRayDirectionHitDistance{};
            CommonResource specularRayDirectionHitDistance{};
            CommonResource hiResDepth{};
            CommonResource specularMotionVector{};
            CommonResource transparency{};
            CommonResource exposure{};
            CommonResource biasCurrentColor{};
            CommonResource particle{};
            CommonResource animTexture{};
            CommonResource positionViewSpace{ };
            CommonResource rayTraceDist{};
            CommonResource mvecReflections{};
            CommonResource transparencyLayer{};
            CommonResource transparencyLayerOpacity{};
            CommonResource colorAfterParticles{};
            CommonResource colorAfterTransparency{};
            CommonResource colorAfterFog{};
            CommonResource screenSpaceSubsurfaceScatteringGuide{};
            CommonResource colorBeforeScreenSpaceSubsurfaceScattering{};
            CommonResource colorAfterScreenSpaceSubsurfaceScattering{};
            CommonResource screenSpaceRefractionGuide{};
            CommonResource colorBeforeScreenSpaceRefraction{};
            CommonResource colorAfterScreenSpaceRefraction{};
            CommonResource depthOfFieldGuide{};
            CommonResource colorBeforeDepthOfField{};
            CommonResource colorAfterDepthOfField{};
            CommonResource disocclusionMask{};
            CommonResource scalingOutputAlpha{};
            CommonResource alpha{};
            CommonResource responsivityMask{};

            SL_CHECK(getTaggedResource(kBufferTypeScalingInputColor, colorIn, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeScalingOutputColor, colorOut, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeDepth, hwDepth, data.frame, ctx.viewport->id, true, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeLinearDepth, linearDepth, data.frame, ctx.viewport->id, true, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeMotionVectors, mvec, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeAlbedo, albedo, data.frame, ctx.viewport->id, false, inputs, numInputs));
            SL_CHECK(getTaggedResource(kBufferTypeSpecularAlbedo, specularAlbedo, data.frame, ctx.viewport->id, false, inputs, numInputs));
            if (ctx.viewport->consts.normalRoughnessMode == DLSSDNormalRoughnessMode::ePacked)
            {
                SL_CHECK(getTaggedResource(kBufferTypeNormalRoughness, normals, data.frame, ctx.viewport->id, false, inputs, numInputs));
            }
            else
            {
                SL_CHECK(getTaggedResource(kBufferTypeNormals, normals, data.frame, ctx.viewport->id, false, inputs, numInputs));
                SL_CHECK(getTaggedResource(kBufferTypeRoughness, roughness, data.frame, ctx.viewport->id, false, inputs, numInputs));
            }

            getTaggedResource(kBufferTypeReflectedAlbedo, reflectedAlbedo, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorBeforeParticles, colorBeforeParticles, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorBeforeTransparency, colorBeforeTransparency, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorBeforeFog, colorBeforeFog, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeDiffuseHitDistance, diffuseHitDistance, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeSpecularHitDistance, specularHitDistance, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeDiffuseRayDirection, diffuseRayDirection, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeSpecularRayDirection, specularRayDirection, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeDiffuseRayDirectionHitDistance, diffuseRayDirectionHitDistance, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeSpecularRayDirectionHitDistance, specularRayDirectionHitDistance, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeHiResDepth, hiResDepth, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeSpecularMotionVectors, specularMotionVector, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeTransparencyHint, transparency, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeExposure, exposure, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeBiasCurrentColorHint, biasCurrentColor, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeParticleHint, particle, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeAnimatedTextureHint, animTexture, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypePosition, positionViewSpace, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeRaytracingDistance, rayTraceDist, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeReflectionMotionVectors, mvecReflections, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeTransparencyLayer, transparencyLayer, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeTransparencyLayerOpacity, transparencyLayerOpacity, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorAfterParticles, colorAfterParticles, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorAfterTransparency, colorAfterTransparency, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorAfterFog, colorAfterFog, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeScreenSpaceSubsurfaceScatteringGuide, screenSpaceSubsurfaceScatteringGuide, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorBeforeScreenSpaceSubsurfaceScattering, colorBeforeScreenSpaceSubsurfaceScattering, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorAfterScreenSpaceSubsurfaceScattering, colorAfterScreenSpaceSubsurfaceScattering, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeScreenSpaceRefractionGuide, screenSpaceRefractionGuide, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorBeforeScreenSpaceRefraction, colorBeforeScreenSpaceRefraction, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorAfterScreenSpaceRefraction, colorAfterScreenSpaceRefraction, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeDepthOfFieldGuide, depthOfFieldGuide, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorBeforeDepthOfField, colorBeforeDepthOfField, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeColorAfterDepthOfField, colorAfterDepthOfField, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeDisocclusionMask, disocclusionMask, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeScalingOutputAlpha, scalingOutputAlpha, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeAlpha, alpha, data.frame, ctx.viewport->id, true, inputs, numInputs);
            getTaggedResource(kBufferTypeResponsivityMask, responsivityMask, data.frame, ctx.viewport->id, true, inputs, numInputs);

            CommonResource& depth = linearDepth ? linearDepth : hwDepth;

            if (!depth || !mvec || !colorIn || !colorOut || !albedo || !specularAlbedo || !normals ||
                (ctx.viewport->consts.normalRoughnessMode == DLSSDNormalRoughnessMode::eUnpacked && !roughness))
            {
                SL_LOG_ERROR("Missing DLSSDContext inputs");
                return Result::eErrorMissingInputParameter;
            }

            sl::Extent colorInExt = colorIn.getExtent();
            sl::Extent colorOutExt = colorOut.getExtent();
            sl::Extent mvecExt = mvec.getExtent();
            sl::Extent depthExt = depth.getExtent();
            sl::Extent albedoExt = albedo.getExtent();
            sl::Extent specAlbedoExt = specularAlbedo.getExtent();
            sl::Extent normalsExt = normals.getExtent();
            sl::Extent roughnessExt = roughness.getExtent();
            sl::Extent reflectedAlbedoExt = reflectedAlbedo.getExtent();
            sl::Extent colorBeforeParticlesExt = colorBeforeParticles.getExtent();
            sl::Extent colorBeforeTransparencyExt = colorBeforeTransparency.getExtent();
            sl::Extent colorBeforeFogExt = colorBeforeFog.getExtent();
            sl::Extent diffuseHitDistanceExt = diffuseHitDistance.getExtent();
            sl::Extent specularHitDistanceExt = specularHitDistance.getExtent();
            sl::Extent diffuseRayDirectionExt = diffuseRayDirection.getExtent();
            sl::Extent specularRayDirectionExt = specularRayDirection.getExtent();
            sl::Extent diffuseRayDirectionHitDistanceExt = diffuseRayDirectionHitDistance.getExtent();
            sl::Extent specularRayDirectionHitDistanceExt = specularRayDirectionHitDistance.getExtent();
            sl::Extent hiResDepthExt = hiResDepth.getExtent();
            sl::Extent specularMotionVectorExt = specularMotionVector.getExtent();
            sl::Extent transparencyExt = transparency.getExtent();
            sl::Extent exposureExt = exposure.getExtent();
            sl::Extent biasCurrentColorExt = biasCurrentColor.getExtent();
            sl::Extent particleExt = particle.getExtent();
            sl::Extent animTextureExt = animTexture.getExtent();
            sl::Extent positionViewSpaceExt = positionViewSpace.getExtent();
            sl::Extent rayTraceDistExt = rayTraceDist.getExtent();
            sl::Extent mvecReflectionsExt = mvecReflections.getExtent();
            sl::Extent transparencyLayerExt = transparencyLayer.getExtent();
            sl::Extent transparencyLayerOpacityExt = transparencyLayerOpacity.getExtent();
            sl::Extent colorAfterParticlesExt = colorAfterParticles.getExtent();
            sl::Extent colorAfterTransparencyExt = colorAfterTransparency.getExtent();
            sl::Extent colorAfterFogExt = colorAfterFog.getExtent();
            sl::Extent screenSpaceSubsurfaceScatteringGuideExt = screenSpaceSubsurfaceScatteringGuide.getExtent();
            sl::Extent colorBeforeScreenSpaceSubsurfaceScatteringExt = colorBeforeScreenSpaceSubsurfaceScattering.getExtent();
            sl::Extent colorAfterScreenSpaceSubsurfaceScatteringExt = colorAfterScreenSpaceSubsurfaceScattering.getExtent();
            sl::Extent screenSpaceRefractionGuideExt = screenSpaceRefractionGuide.getExtent();
            sl::Extent colorBeforeScreenSpaceRefractionExt = colorBeforeScreenSpaceRefraction.getExtent();
            sl::Extent colorAfterScreenSpaceRefractionExt = colorAfterScreenSpaceRefraction.getExtent();
            sl::Extent depthOfFieldGuideExt = depthOfFieldGuide.getExtent();
            sl::Extent colorBeforeDepthOfFieldExt = colorBeforeDepthOfField.getExtent();
            sl::Extent colorAfterDepthOfFieldExt = colorAfterDepthOfField.getExtent();
            sl::Extent disocclusionMaskExt = disocclusionMask.getExtent();
            sl::Extent scalingOutputAlphaExt = scalingOutputAlpha.getExtent();
            sl::Extent alphaExt = alpha.getExtent();
            sl::Extent responsivityMaskExt = responsivityMask.getExtent();

            // Depending if camera motion is provided or not we can use input directly or not
            auto mvecIn = mvec;

#if SL_ENABLE_TIMING
            CHI_VALIDATE(ctx.compute->beginPerfSection(pCmdList, SL_RESOURCE_NAME("perf.evaluate").c_str()));
#endif
            {
                ctx.cacheState(colorIn, colorIn.getState());
                ctx.cacheState(colorOut, colorOut.getState());
                ctx.cacheState(depth, depth.getState());
                ctx.cacheState(mvecIn, mvec.getState());
                ctx.cacheState(albedo, albedo.getState());
                ctx.cacheState(specularAlbedo, specularAlbedo.getState());
                ctx.cacheState(normals, normals.getState());
                ctx.cacheState(roughness, roughness.getState());
                ctx.cacheState(reflectedAlbedo, reflectedAlbedo.getState());
                ctx.cacheState(colorBeforeParticles, colorBeforeParticles.getState());
                ctx.cacheState(colorBeforeTransparency, colorBeforeTransparency.getState());
                ctx.cacheState(colorBeforeFog, colorBeforeFog.getState());
                ctx.cacheState(diffuseHitDistance, diffuseHitDistance.getState());
                ctx.cacheState(specularHitDistance, specularHitDistance.getState());
                ctx.cacheState(diffuseRayDirection, diffuseRayDirection.getState());
                ctx.cacheState(specularRayDirection, specularRayDirection.getState());
                ctx.cacheState(diffuseRayDirectionHitDistance, diffuseRayDirectionHitDistance.getState());
                ctx.cacheState(specularRayDirectionHitDistance, specularRayDirectionHitDistance.getState());
                ctx.cacheState(hiResDepth, hiResDepth.getState());
                ctx.cacheState(specularMotionVector, specularMotionVector.getState());
                ctx.cacheState(transparency, transparency.getState());
                ctx.cacheState(exposure, exposure.getState());
                ctx.cacheState(biasCurrentColor, biasCurrentColor.getState());
                ctx.cacheState(particle, particle.getState());
                ctx.cacheState(animTexture, animTexture.getState());
                ctx.cacheState(positionViewSpace, positionViewSpace.getState());
                ctx.cacheState(rayTraceDist, rayTraceDist.getState());
                ctx.cacheState(mvecReflections, mvecReflections.getState());
                ctx.cacheState(transparencyLayer, transparencyLayer.getState());
                ctx.cacheState(transparencyLayerOpacity, transparencyLayerOpacity.getState());
                ctx.cacheState(colorAfterParticles, colorAfterParticles.getState());
                ctx.cacheState(colorAfterTransparency, colorAfterTransparency.getState());
                ctx.cacheState(colorAfterFog, colorAfterFog.getState());
                ctx.cacheState(screenSpaceSubsurfaceScatteringGuide, screenSpaceRefractionGuide.getState());
                ctx.cacheState(colorBeforeScreenSpaceSubsurfaceScattering, colorBeforeScreenSpaceSubsurfaceScattering.getState());
                ctx.cacheState(colorAfterScreenSpaceSubsurfaceScattering, colorAfterScreenSpaceSubsurfaceScattering.getState());
                ctx.cacheState(screenSpaceRefractionGuide, screenSpaceRefractionGuide.getState());
                ctx.cacheState(colorBeforeScreenSpaceRefraction, colorBeforeScreenSpaceRefraction.getState());
                ctx.cacheState(colorAfterScreenSpaceRefraction, colorAfterScreenSpaceRefraction.getState());
                ctx.cacheState(depthOfFieldGuide, depthOfFieldGuide.getState());
                ctx.cacheState(colorBeforeDepthOfField, colorBeforeDepthOfField.getState());
                ctx.cacheState(colorAfterDepthOfField, colorAfterDepthOfField.getState());
                ctx.cacheState(disocclusionMask, disocclusionMask.getState());
                ctx.cacheState(scalingOutputAlpha, scalingOutputAlpha.getState());
                ctx.cacheState(alpha, alpha.getState());
                ctx.cacheState(responsivityMask, responsivityMask.getState());

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

                    // TODO - this is not optimal in the case of dynamic resizing, but cameraMotionIncluded should be true for most existing DLSSDContext titles.
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
                        ctx.compute->beginVRAMSegment(api::getPluginName());
                        sl::chi::ResourceDescription desc(renderWidth, renderHeight, sl::chi::eFormatRG16F,sl::chi::HeapType::eHeapTypeDefault, sl::chi::ResourceState::eTextureRead);
                        CHI_VALIDATE(ctx.compute->createTexture2D(desc, ctx.viewport->mvec, SL_RESOURCE_NAME("tex2d.mvec").c_str()));
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
                        {mvecIn, chi::ResourceState::eStorageRW, chi::ResourceState::eTextureRead}
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

                if (ctx.ngxContext)
                {
                    // DLSSDContext
                    extra::ScopedTasks revTransitions;
                    chi::ResourceTransition transitions[] =
                    {
                        {colorIn, chi::ResourceState::eTextureRead, ctx.cachedStates[colorIn]},
                        {colorOut, chi::ResourceState::eStorageRW, ctx.cachedStates[colorOut]},
                        {depth, chi::ResourceState::eTextureRead, ctx.cachedStates[depth]},
                        {mvecIn, chi::ResourceState::eTextureRead, ctx.cachedStates[mvecIn]},
                        {albedo, chi::ResourceState::eTextureRead, ctx.cachedStates[albedo]},
                        {specularAlbedo, chi::ResourceState::eTextureRead, ctx.cachedStates[specularAlbedo]},
                        {normals, chi::ResourceState::eTextureRead, ctx.cachedStates[normals]},
                        {roughness, chi::ResourceState::eTextureRead, ctx.cachedStates[roughness]},
                        {reflectedAlbedo, chi::ResourceState::eTextureRead, ctx.cachedStates[reflectedAlbedo]},
                        {colorBeforeParticles, chi::ResourceState::eTextureRead, ctx.cachedStates[colorBeforeParticles]},
                        {colorBeforeTransparency, chi::ResourceState::eTextureRead, ctx.cachedStates[colorBeforeTransparency]},
                        {colorBeforeFog, chi::ResourceState::eTextureRead, ctx.cachedStates[colorBeforeFog]},
                        {diffuseHitDistance, chi::ResourceState::eTextureRead, ctx.cachedStates[diffuseHitDistance]},
                        {specularHitDistance, chi::ResourceState::eTextureRead, ctx.cachedStates[specularHitDistance]},
                        {diffuseRayDirection, chi::ResourceState::eTextureRead, ctx.cachedStates[diffuseRayDirection]},
                        {specularRayDirection, chi::ResourceState::eTextureRead, ctx.cachedStates[specularRayDirection]},
                        {diffuseRayDirectionHitDistance, chi::ResourceState::eTextureRead, ctx.cachedStates[diffuseRayDirectionHitDistance]},
                        {specularRayDirectionHitDistance, chi::ResourceState::eTextureRead, ctx.cachedStates[specularRayDirectionHitDistance]},
                        {hiResDepth, chi::ResourceState::eTextureRead, ctx.cachedStates[hiResDepth]},
                        {specularMotionVector, chi::ResourceState::eTextureRead, ctx.cachedStates[specularMotionVector]},
                        {transparency, chi::ResourceState::eTextureRead, ctx.cachedStates[transparency]},
                        {exposure, chi::ResourceState::eTextureRead, ctx.cachedStates[exposure]},
                        {biasCurrentColor, chi::ResourceState::eTextureRead, ctx.cachedStates[biasCurrentColor]},
                        {particle, chi::ResourceState::eTextureRead, ctx.cachedStates[particle]},
                        {animTexture, chi::ResourceState::eTextureRead, ctx.cachedStates[animTexture]},
                        {positionViewSpace, chi::ResourceState::eTextureRead, ctx.cachedStates[positionViewSpace]},
                        {rayTraceDist, chi::ResourceState::eTextureRead, ctx.cachedStates[rayTraceDist]},
                        {mvecReflections, chi::ResourceState::eTextureRead, ctx.cachedStates[mvecReflections]},
                        {transparencyLayer, chi::ResourceState::eTextureRead, ctx.cachedStates[transparencyLayer]},
                        {transparencyLayerOpacity, chi::ResourceState::eTextureRead, ctx.cachedStates[transparencyLayerOpacity]},
                        {colorAfterParticles, chi::ResourceState::eTextureRead, ctx.cachedStates[colorAfterParticles]},
                        {colorAfterTransparency, chi::ResourceState::eTextureRead, ctx.cachedStates[colorAfterTransparency]},
                        {colorAfterFog, chi::ResourceState::eTextureRead, ctx.cachedStates[colorAfterFog]},
                        {screenSpaceSubsurfaceScatteringGuide, chi::ResourceState::eTextureRead, ctx.cachedStates[screenSpaceSubsurfaceScatteringGuide]},
                        {colorBeforeScreenSpaceSubsurfaceScattering, chi::ResourceState::eTextureRead, ctx.cachedStates[colorBeforeScreenSpaceSubsurfaceScattering]},
                        {colorAfterScreenSpaceRefraction, chi::ResourceState::eTextureRead, ctx.cachedStates[colorAfterScreenSpaceSubsurfaceScattering]},
                        {screenSpaceRefractionGuide, chi::ResourceState::eTextureRead, ctx.cachedStates[screenSpaceRefractionGuide]},
                        {colorBeforeScreenSpaceRefraction, chi::ResourceState::eTextureRead, ctx.cachedStates[colorBeforeScreenSpaceRefraction]},
                        {colorAfterScreenSpaceRefraction, chi::ResourceState::eTextureRead, ctx.cachedStates[colorAfterScreenSpaceRefraction]},
                        {depthOfFieldGuide, chi::ResourceState::eTextureRead, ctx.cachedStates[depthOfFieldGuide]},
                        {colorBeforeDepthOfField, chi::ResourceState::eTextureRead, ctx.cachedStates[colorBeforeDepthOfField]},
                        {colorAfterDepthOfField, chi::ResourceState::eTextureRead, ctx.cachedStates[colorAfterDepthOfField]},
                        {disocclusionMask, chi::ResourceState::eTextureRead, ctx.cachedStates[disocclusionMask]},
                        {scalingOutputAlpha, chi::ResourceState::eTextureRead, ctx.cachedStates[scalingOutputAlpha]},
                        {alpha, chi::ResourceState::eTextureRead, ctx.cachedStates[alpha]},
                        {responsivityMask, chi::ResourceState::eTextureRead, ctx.cachedStates[responsivityMask]},
                    };
                    ctx.compute->transitionResources(pCmdList, transitions, (uint32_t)countof(transitions), &revTransitions);

                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Reset, consts->reset == Boolean::eTrue);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MV_Scale_X, (mvecPixelSpace ? 1.0f : (float)(consts->mvecScale.x * renderWidth)));
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, (mvecPixelSpace ? 1.0f : (float)(consts->mvecScale.y * renderHeight)));
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, consts->jitterOffset.x);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, consts->jitterOffset.y);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Sharpness, ctx.viewport->consts.sharpness);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, ctx.viewport->consts.preExposure);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, ctx.viewport->consts.exposureScale);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, renderWidth);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, renderHeight);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, ctx.viewport->consts.indicatorInvertAxisX);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, ctx.viewport->consts.indicatorInvertAxisY);

                    if (ctx.platform == RenderAPI::eVulkan)
                    {
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Color, ctx.cachedVkResource(colorIn));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Output, ctx.cachedVkResource(colorOut));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Depth, ctx.cachedVkResource(depth));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectors, ctx.cachedVkResource(mvecIn));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, ctx.cachedVkResource(albedo));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_SpecularAlbedo, ctx.cachedVkResource(specularAlbedo));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_GBuffer_Normals, ctx.cachedVkResource(normals));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, ctx.cachedVkResource(roughness));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ReflectedAlbedo, ctx.cachedVkResource(reflectedAlbedo));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, ctx.cachedVkResource(colorBeforeParticles));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, ctx.cachedVkResource(colorBeforeTransparency));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, ctx.cachedVkResource(colorBeforeFog));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance, ctx.cachedVkResource(diffuseHitDistance));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, ctx.cachedVkResource(specularHitDistance));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirection, ctx.cachedVkResource(diffuseRayDirection));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirection, ctx.cachedVkResource(specularRayDirection));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance, ctx.cachedVkResource(diffuseRayDirectionHitDistance));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance, ctx.cachedVkResource(specularRayDirectionHitDistance));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DepthHighRes, ctx.cachedVkResource(hiResDepth));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, ctx.cachedVkResource(specularMotionVector));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_TransparencyMask, ctx.cachedVkResource(transparency));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_ExposureTexture, ctx.cachedVkResource(exposure));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, ctx.cachedVkResource(biasCurrentColor));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_IsParticleMask, ctx.cachedVkResource(particle));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, ctx.cachedVkResource(animTexture));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Position_ViewSpace, ctx.cachedVkResource(positionViewSpace));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayTracingHitDistance, ctx.cachedVkResource(rayTraceDist));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, ctx.cachedVkResource(mvecReflections));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer, ctx.cachedVkResource(transparencyLayer));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity, ctx.cachedVkResource(transparencyLayerOpacity));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterParticles, ctx.cachedVkResource(colorAfterParticles));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterTransparency, ctx.cachedVkResource(colorAfterTransparency));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterFog, ctx.cachedVkResource(colorAfterFog));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide, ctx.cachedVkResource(screenSpaceSubsurfaceScatteringGuide));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering, ctx.cachedVkResource(colorBeforeScreenSpaceSubsurfaceScattering));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceSubsurfaceScattering, ctx.cachedVkResource(colorAfterScreenSpaceSubsurfaceScattering));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceRefractionGuide, ctx.cachedVkResource(screenSpaceRefractionGuide));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceRefraction, ctx.cachedVkResource(colorBeforeScreenSpaceRefraction));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceRefraction, ctx.cachedVkResource(colorAfterScreenSpaceRefraction));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, ctx.cachedVkResource(depthOfFieldGuide));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeDepthOfField, ctx.cachedVkResource(colorBeforeDepthOfField));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterDepthOfField, ctx.cachedVkResource(colorAfterDepthOfField));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask, ctx.cachedVkResource(disocclusionMask));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_Alpha, ctx.cachedVkResource(alpha));
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_OutputAlpha, ctx.cachedVkResource(scalingOutputAlpha));

                    }
                    else
                    {
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Color, (void*)colorIn);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Output, (void*)colorOut);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Depth, (void*)depth);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectors, (void*)mvecIn);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, (void*)albedo);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_SpecularAlbedo, (void*)specularAlbedo);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_GBuffer_Normals, (void*)normals);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, (void*)roughness);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ReflectedAlbedo, (void*)reflectedAlbedo);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, (void*)colorBeforeParticles);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, (void*)colorBeforeTransparency);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, (void*)colorBeforeFog);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance, (void*)diffuseHitDistance);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, (void*)specularHitDistance);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirection, (void*)diffuseRayDirection);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirection, (void*)specularRayDirection);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance, (void*)diffuseRayDirectionHitDistance);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance, (void*)specularRayDirectionHitDistance);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DepthHighRes, (void*)hiResDepth);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, (void*)specularMotionVector);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_TransparencyMask, (void*)transparency);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*)exposure);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void*)biasCurrentColor);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_IsParticleMask, (void*)particle);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, (void*)animTexture);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Position_ViewSpace, (void*)positionViewSpace);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RayTracingHitDistance, (void*)rayTraceDist);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, (void*)mvecReflections);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer, (void*)transparencyLayer);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity, (void*)transparencyLayerOpacity);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterParticles, (void*)colorAfterParticles);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterTransparency, (void*)colorAfterTransparency);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterFog, (void*)colorAfterFog);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide, (void*)screenSpaceSubsurfaceScatteringGuide);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering, (void*)colorBeforeScreenSpaceSubsurfaceScattering);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceSubsurfaceScattering, (void*)colorAfterScreenSpaceSubsurfaceScattering);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceRefractionGuide, (void*)screenSpaceRefractionGuide);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceRefraction, (void*)colorBeforeScreenSpaceRefraction);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceRefraction, (void*)colorAfterScreenSpaceRefraction);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, (void*)depthOfFieldGuide);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeDepthOfField, (void*)colorBeforeDepthOfField);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterDepthOfField, (void*)colorAfterDepthOfField);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask, (void*)disocclusionMask);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_OutputAlpha, (void*)scalingOutputAlpha);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_Alpha, (void*)alpha);
                        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask, (void*)responsivityMask);
                        
                    }

                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, colorInExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, colorInExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, colorOutExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, colorOutExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, depthExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depthExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, mvecExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, mvecExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X, albedoExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, albedoExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X, specAlbedoExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, specAlbedoExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X, normalsExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, normalsExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X, roughnessExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, roughnessExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ReflectedAlbedo_Subrect_Base_X, reflectedAlbedoExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ReflectedAlbedo_Subrect_Base_Y, reflectedAlbedoExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles_Subrect_Base_X, colorBeforeParticlesExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles_Subrect_Base_Y, colorBeforeParticlesExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_X, colorBeforeTransparencyExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_Y, colorBeforeTransparencyExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog_Subrect_Base_X, colorBeforeFogExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog_Subrect_Base_Y, colorBeforeFogExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance_Subrect_Base_X, diffuseRayDirectionExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance_Subrect_Base_Y, diffuseRayDirectionExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X, specularHitDistanceExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, specularHitDistanceExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirection_Subrect_Base_X, diffuseRayDirectionExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirection_Subrect_Base_Y, diffuseRayDirectionExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirection_Subrect_Base_X, specularRayDirectionExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirection_Subrect_Base_Y, specularRayDirectionExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance_Subrect_Base_X, diffuseRayDirectionHitDistanceExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance_Subrect_Base_Y, diffuseRayDirectionHitDistanceExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X, specularRayDirectionHitDistanceExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, specularRayDirectionHitDistanceExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, transparencyExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, transparencyExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, biasCurrentColorExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, biasCurrentColorExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X, transparencyLayerExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y, transparencyLayerExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X, transparencyLayerOpacityExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y, transparencyLayerOpacityExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterParticles_Subrect_Base_X, colorAfterParticlesExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterParticles_Subrect_Base_Y, colorAfterParticlesExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterTransparency_Subrect_Base_X, colorAfterTransparencyExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterTransparency_Subrect_Base_Y, colorAfterTransparencyExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_X, screenSpaceSubsurfaceScatteringGuideExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_Y, screenSpaceSubsurfaceScatteringGuideExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering_Subrect_Base_X, colorBeforeScreenSpaceSubsurfaceScatteringExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering_Subrect_Base_Y, colorBeforeScreenSpaceSubsurfaceScatteringExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceSubsurfaceScattering_Subrect_Base_X, colorAfterScreenSpaceSubsurfaceScatteringExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceSubsurfaceScattering_Subrect_Base_Y, colorAfterScreenSpaceSubsurfaceScatteringExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceRefractionGuide_Subrect_Base_X, screenSpaceRefractionGuideExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceRefractionGuide_Subrect_Base_Y, screenSpaceRefractionGuideExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceRefraction_Subrect_Base_X, colorBeforeScreenSpaceRefractionExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceRefraction_Subrect_Base_Y, colorBeforeScreenSpaceRefractionExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceRefraction_Subrect_Base_X, colorAfterScreenSpaceRefractionExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceRefraction_Subrect_Base_Y, colorAfterScreenSpaceRefractionExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide_Subrect_Base_X, depthOfFieldGuideExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide_Subrect_Base_Y, depthOfFieldGuideExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeDepthOfField_Subrect_Base_X, colorBeforeDepthOfFieldExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeDepthOfField_Subrect_Base_Y, colorBeforeDepthOfFieldExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterDepthOfField_Subrect_Base_X, colorAfterDepthOfFieldExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ColorAfterDepthOfField_Subrect_Base_Y, colorAfterDepthOfFieldExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X, disocclusionMaskExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y, disocclusionMaskExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_OutputAlpha_Subrect_Base_X, scalingOutputAlphaExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_OutputAlpha_Subrect_Base_Y, scalingOutputAlphaExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_Alpha_Subrect_Base_X, alphaExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_Alpha_Subrect_Base_Y, alphaExt.top);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_X, responsivityMaskExt.left);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSSD_ResponsivityMask_Subrect_Base_Y, responsivityMaskExt.top);

                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, &ctx.viewport->consts.worldToCameraView);
                    ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, &ctx.commonConsts->cameraViewToClip);

                    ctx.ngxContext->evaluateFeature(pCmdList, ctx.viewport->handle, api::getPluginName());
                }

                float ms = 0;
#if SL_ENABLE_TIMING
                CHI_VALIDATE(ctx.compute->endPerfSection(pCmdList, SL_RESOURCE_NAME("perf.evaluate").c_str(), ms));
#endif

#ifndef SL_PRODUCTION
                /*static std::string s_stats;
                auto v = api::getContext()->pluginVersion;
                
                s_stats = extra::format("{} {} - NGX {} - ({}x{})->({}x{}) - {}ms", api::getPluginName(), v.toStr() + "." + GIT_LAST_COMMIT_SHORT, ctx.ngxVersion,
                    renderWidth, renderHeight, ctx.viewport->consts.outputWidth, ctx.viewport->consts.outputHeight, ms);
                parameters->set(sl::param::dlss_d::kStats, (void*)s_stats.c_str());*/

                {
                    uint64_t bytes;
                    ctx.compute->getAllocatedBytes(bytes, api::getPluginName());
                    std::scoped_lock lock(ctx.uiStats.mtx);
                    ctx.uiStats.mode = getDLSSModeAsStr(ctx.viewport->consts.mode);
                    ctx.uiStats.viewport = extra::format("Viewport {}x{} -> {}x{}", renderWidth, renderHeight, ctx.viewport->consts.outputWidth, ctx.viewport->consts.outputHeight);
                    ctx.uiStats.runtime = extra::format("{}ms", ms);
                    ctx.uiStats.vram = extra::format("{}GB", bytes / (1024.0 * 1024.0 * 1024.0));
                }
#endif

                uint32_t frame = 0;
                ctx.compute->getFinishedFrameIndex(frame);
                parameters->set(sl::param::dlss_d::kCurrentFrame, frame + 1);
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
    auto& ctx = (*dlss_d::getContext());

    getPointerParam(parameters, param::global::kNGXContext, &ctx.ngxContext);
    if (!ctx.ngxContext)
    {
        SL_LOG_ERROR( "NGX context is missing, please make sure DLSSDContext feature is enabled and supported on the platform");
        return Result::eErrorMissingOrInvalidAPI;
    }
    auto state = findStruct<DLSSDState>(output);
    auto settings = findStruct<DLSSDOptimalSettings>(output);
    auto consts = findStruct<const DLSSDOptions>(inputs);

    if ((!consts || !settings) && !state)
    {
        SL_LOG_ERROR( "Invalid input data");
        return Result::eErrorMissingInputParameter;
    }

    // Settings
    if (consts && settings)
    {
        void* callback = NULL;
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &callback);
        if (!callback)
        {
            SL_LOG_ERROR( "DLSSDContext 'getOptimalSettings' callback is missing, please make sure DLSSDContext feature is up to date");
            return Result::eErrorNGXFailed;
        }

        // These are selections made by user in UI
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Width, consts->outputWidth);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_Height, consts->outputHeight);
        // SL DLSSDContext modes start with 'off' so subtract one, the rest is mapped 1:1
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (NVSDK_NGX_PerfQuality_Value)((uint32_t)consts->mode - 1));
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_RTXValue, false);

        NVSDK_NGX_Result res = NVSDK_NGX_Result_Success;
        auto getOptimalSettings = (PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback;
        res = getOptimalSettings(ctx.ngxContext->params);
        if (NVSDK_NGX_FAILED(res))
        {
            SL_LOG_ERROR( "DLSSDContext 'getOptimalSettings' callback failed - error %u", res);
            return Result::eErrorNGXFailed;
        }
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_OutWidth, &settings->optimalRenderWidth);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_OutHeight, &settings->optimalRenderHeight);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_Sharpness, &settings->optimalSharpness);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &settings->renderWidthMax);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &settings->renderHeightMax);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &settings->renderWidthMin);
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &settings->renderHeightMin);
        if (settings->optimalRenderWidth == 0 || settings->optimalRenderHeight == 0)
        {
            SL_LOG_WARN("DLSSContext 'getOptimalSettings' returns invalid size (%d x %d), due to unsupported mode or output size", settings->optimalRenderWidth, settings->optimalRenderHeight);
            return Result::eErrorInvalidParameter;
        }
    }
    
    // Stats
    if(state)
    {
        void* callback = NULL;
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_DLSSDGetStatsCallback, &callback);
        if (!callback)
        {
            SL_LOG_ERROR( "DLSSDContext 'getStats' callback is missing, please make sure DLSSDContext feature is up to date");
            return Result::eErrorNGXFailed;
        }
        auto getStats = (PFN_NVSDK_NGX_DLSS_GetStatsCallback)callback;
        auto res = getStats(ctx.ngxContext->params);
        if (NVSDK_NGX_FAILED(res))
        {
            SL_LOG_ERROR( "DLSSDContext 'getStats' callback failed - error %u", res);
            return Result::eErrorNGXFailed;
        }
        // TODO: This has to return the correct estimate regardless if callback is present or not.
        ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_SizeInBytes, &state->estimatedVRAMUsageInBytes);
    }
    return Result::eOk;
}

Result slAllocateResources(sl::CommandBuffer* cmdBuffer, Feature feature, const sl::ViewportHandle& viewport)
{
    auto& ctx = (*dlss_d::getContext());
    common::EventData data{ viewport, 0 };
    dlssdBeginEvent(cmdBuffer, data, nullptr, 0);
    auto it = ctx.viewports.find(viewport);
    return it != ctx.viewports.end() && (*it).second.handle != nullptr ? Result::eOk : Result::eErrorInvalidParameter;
}

Result slFreeResources(Feature feature, const sl::ViewportHandle& viewport)
{
    auto& ctx = (*dlss_d::getContext());
    auto it = ctx.viewports.find(viewport);
    if (it != ctx.viewports.end())
    {
        auto& instance = (*it).second;
        if (instance.handle)
        {
            SL_LOG_INFO("Releasing DLSSDContext instance id %u", viewport);
            ctx.ngxContext->releaseFeature(instance.handle, api::getPluginName());
            // OK to release null resources
            CHI_VALIDATE(ctx.compute->destroyResource(instance.mvec));

            // Reset denoise mode after releasing. Otherwise the param is alwasy ON after toggling dlss-d.
            // This should be functionally unnecessary since no other features read the denoise mode param,
            // but that can cause confusion for who reads telemetry data.
            ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_Off);
        }
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, (void*)nullptr);
        ctx.ngxContext->params->Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, (void*)nullptr);
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

    auto& ctx = (*dlss_d::getContext());

    auto parameters = api::getContext()->parameters;

    getPointerParam(parameters, param::global::kNGXContext, &ctx.ngxContext);

    if (!ctx.ngxContext)
    {
        SL_LOG_ERROR( "Missing NGX context - DLSSDContext cannot run");
        return false;
    }

    if (!ctx.ngxContext->params)
    {
        SL_LOG_ERROR( "Missing NGX default parameters - DLSSDContext cannot run");
        return false;
    }

    int supported = 0;
    NVSDK_NGX_Result ngxResult = ctx.ngxContext->params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &supported);
    if (NVSDK_NGX_FAILED(ngxResult))
    {
        SL_LOG_ERROR( "NGX parameter indicating DLSSDContext support cannot be found (0x%x) - DLSSDContext cannot run", ngxResult);
        return false;
    }

    if (!supported)
    {
        SL_LOG_ERROR( "NGX indicates DLSSDContext is not available - DLSSDContext cannot run");
        return false;
    }

    // Register our event callbacks
    if (!param::getPointerParam(parameters, param::common::kPFunRegisterEvaluateCallbacks, &ctx.registerEvaluateCallbacks))
    {
        SL_LOG_ERROR( "Cannot obtain `registerEvaluateCallbacks` interface - check that sl.common was initialized correctly");
        return false;
    }
    ctx.registerEvaluateCallbacks(kFeatureDLSS_RR, dlssdBeginEvent, dlssdEndEvent);

    param::getPointerParam(parameters, sl::param::common::kComputeAPI, &ctx.compute);

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

    // Update our DLSS feature if update is available and host opted in
    ctx.ngxContext->updateFeature(NVSDK_NGX_Feature_RayReconstruction);

#ifndef SL_PRODUCTION
    common::PFunGetStringFromModule* func{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunGetStringFromModule, &func);
    if(func)
    {
        func("nvngx_dlssd.dll", "FileVersion", ctx.ngxVersion);
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
                if (ui->collapsingHeader(extra::format("{} v{}", api::getPluginName(), (v.toStr() + "." + GIT_LAST_COMMIT_SHORT)).c_str(), imgui::kTreeNodeFlagDefaultOpen))
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
    auto& ctx = (*dlss_d::getContext());

    ctx.registerEvaluateCallbacks(kFeatureDLSS_RR, nullptr, nullptr);

    // Common shutdown
    plugin::onShutdown(api::getContext());

    for(auto v : ctx.viewports)
    {
        ctx.ngxContext->releaseFeature(v.second.handle, api::getPluginName());
        CHI_VALIDATE(ctx.compute->destroyResource(v.second.mvec));
    }
    CHI_VALIDATE(ctx.compute->destroyKernel(ctx.mvecKernel));
}

sl::Result slDLSSDGetOptimalSettings(const sl::DLSSDOptions& options, sl::DLSSDOptimalSettings& settings)
{
    return slGetData(&options, &settings, nullptr);
}

sl::Result slDLSSDGetState(const sl::ViewportHandle& viewport, sl::DLSSDState& state)
{
    return slGetData(&viewport, &state, nullptr);
}

sl::Result slDLSSDSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSDOptions& options)
{
    auto v = viewport;
    v.next = (sl::BaseStructure*)&options;
    return slSetData(&v, nullptr);
}

sl::Result slIsSupported(const sl::AdapterInfo& adapterInfo)
{
    auto& ctx = (*dlss_d::getContext());
    sl::common::PFunFindAdapter* findAdapter{};
    param::getPointerParam(api::getContext()->parameters, param::common::kPFunFindAdapter, &findAdapter);
    if (!findAdapter)
    {
        SL_LOG_ERROR("sl.common not loaded");
        return Result::eErrorFeatureMissing;
    }
    return findAdapter(adapterInfo, ctx.adapterMask);
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

    SL_EXPORT_FUNCTION(slDLSSDSetOptions);
    SL_EXPORT_FUNCTION(slDLSSDGetOptimalSettings);
    SL_EXPORT_FUNCTION(slDLSSDGetState);

    return nullptr;
}

}
