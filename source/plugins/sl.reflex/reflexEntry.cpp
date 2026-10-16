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

#include <unordered_set>
#include <functional>

#include "include/sl.h"
#include "include/sl_consts.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.api/internalDataSharing.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/core/sl.param/parameters.h"
#include "source/platforms/sl.chi/compute.h"
#include "source/plugins/sl.reflex/versions.h"
#include "source/plugins/sl.reflex/reflex_shared.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "source/plugins/sl.imgui/imgui.h"
#include "_artifacts/json/reflex_json.h"
#include "_artifacts/gitVersion.h"
#include "nvapi.h"
#include "external/json/include/nlohmann/json.hpp"
#include <optional>
#include "include/sl_matrix_helpers.h"

using json = nlohmann::json;
using namespace std::chrono_literals;


// VK_NV_low_latency2
#include "source/platforms/sl.chi/vulkan.h"
#include "source/platforms/sl.chi/vulkannv.h"

// DEPRECATED (reflex-pcl):
#include "source/core/sl.plugin-manager/pluginManager.h"

//! Number of frames to keep in simulation timing history
constexpr size_t kSimulationTimingHistorySize = 120;

namespace sl
{

namespace reflex
{

struct UIStats
{
    std::mutex mtx;
    std::string mode;
    std::string markers;
    std::string fpsCap;
    std::string presentFrame;
    std::string sleeping;
};

template <typename T>
class ReflexCameraDataManager
{
    std::vector<std::pair<uint32_t, T>> cameraData;
    std::mutex framesMutex;
    uint32_t lastFrame = 0;
    std::condition_variable getFrameCV;

public:
    ReflexCameraDataManager()
    {
        cameraData.resize(MAX_FRAMES_IN_FLIGHT);
    }

    void insertCameraData(const uint32_t frameID, const T& inCameraData)
    {
        std::lock_guard<std::mutex> _(framesMutex);
        if (frameID <= 0)
        {
            return; // first frame data not used
        }
        auto& availableCameraData = cameraData.at(frameID % MAX_FRAMES_IN_FLIGHT);
        if (frameID == availableCameraData.first)
        {
            SL_LOG_WARN("Camera data for frame %d already set!", frameID);
            return;
        }
        if (lastFrame + 1 != frameID)
        {
            SL_LOG_WARN("Out of order camera data detected! last: %d, pushing: %d", lastFrame, frameID);
        }
        availableCameraData = std::make_pair(frameID, inCameraData);
        lastFrame = frameID;
        getFrameCV.notify_one();
    }

    std::optional<T> getCameraData(uint32_t frameID)
    {
        while (true)
        {
            // Look for frame ID.
            {
                std::lock_guard<std::mutex> _(framesMutex);

                auto& availableCameraData = cameraData.at(frameID % MAX_FRAMES_IN_FLIGHT);
                if (frameID == availableCameraData.first)
                {
                    return availableCameraData.second;
                }
            }

            // Wait for next frame.
            {
                std::unique_lock<std::mutex> lock(framesMutex);

                // UE often doesn't send first few frames, make sure we don't block
                // Depend on the engine, this also loosely define minimum support framerate, as 1/timeout
                auto timeout = frameID < 5 ? 0ms : 100ms;
                if (!getFrameCV.wait_for(lock, timeout, [&] { return frameID == cameraData.at(frameID % MAX_FRAMES_IN_FLIGHT).first; }))
                {
                    if (timeout > 0us)
                    {
                        SL_LOG_WARN("Time out trying to get data for frame %d (wait %dms)", frameID, std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
                    }
                    return std::nullopt;
                }
            }
        }
    }
};

//! Our common context
//! 
//! Here we can keep whatever global state we need
//! 
struct LatencyContext
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(LatencyContext);
    void onCreateContext() {};
    void onDestroyContext() {};

    common::PFunRegisterEvaluateCallbacks* registerEvaluateCallbacks{};

    // Compute API
    RenderAPI platform = RenderAPI::eD3D12;
    chi::ICompute* compute{};

    // DEPRECATED (reflex-pcl):
    plugin_manager::PFun_slGetDataInternal* pclGetData{};
    plugin_manager::PFun_slSetDataInternal* pclSetData{};

    UIStats uiStats{};

    // Engine type (Unity, UE etc)
    EngineType engine{};

    //! Latest constants
    ReflexOptions constants{};

    //! Camera data
    ReflexCameraDataManager<ReflexCameraData> simCameraData;
    //! Predicted camera data
    ReflexCameraDataManager<ReflexPredictedCameraData> predCameraData;

    //! Matrices from the last frame
    float4x4 prevWorldToViewMatrix{};
    float4x4 prevViewToClipMatrix{};
    bool predictCamera = false;
    std::function<void(uint32_t frame, const sl::ReflexCameraData& cameraData)> cameraDataCallback;


    //! Can be overridden via sl.reflex.json config
    uint32_t frameLimitUs = UINT_MAX;
    bool useMarkersToOptimizeOverride = false;
    bool useMarkersToOptimizeOverrideValue = false;

    //! Specifies if low-latency mode is available or not
    bool lowLatencyAvailable = false;
    //! Specifies if latency report is available or not
    bool latencyReportAvailable = false;
    //! Specifies ownership of flash indicator toggle (true = driver, false = application)
    bool flashIndicatorDriverControlled = false;

    extra::AverageValueMeter sleepMeter{};

    //! Stats initialized or not
    std::atomic<bool> initialized = false;
    std::atomic<bool> enabled = false;

    PFunSetPCLStatsMarker* setStatsMarkerFunc = nullptr;
    
    sl::chi::Fence gameWaitFence{};
    uint32_t gameWaitSyncValue{};
    chi::ICommandListContext* gameWaitCmdList{};

    struct SimStartEntry
    {
        uint32_t frameId = 0;
        std::chrono::high_resolution_clock::time_point timestamp{};
        uint8_t driverFgMultiplier = 0;
        bool bDfgControl = false;
    };

    //! Circular buffer to store simulation start data (timestamps + DFG driver info)
    std::array<SimStartEntry, kSimulationTimingHistorySize> simulationStartData{};
    //! Mutex for thread-safe access to simulation timing data
    std::mutex simulationTimingMutex;
    std::mutex callbacksMutex;
};
}

//! Embedded JSON, containing information about the plugin and the hooks it requires.
static std::string JSON = std::string(reflex_json, &reflex_json[reflex_json_len]);

void updateEmbeddedJSON(json& config);

sl::Result slReflexSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame);
sl::Result slReflexGetCameraDataInternal(const sl::ViewportHandle& viewport, const uint32_t frame, sl::ReflexCameraData& outCameraData);
sl::Result slReflexSetCameraDataFenceInternal(const sl::ViewportHandle& viewport, sl::chi::Fence fence, const uint32_t syncValue, chi::ICommandListContext* cmdList);
sl::Result slReflexSetCameraDataCallbackInternal(std::function<void(uint32_t frame, const sl::ReflexCameraData& cameraData)> callback);
sl::Result slReflexGetSimulationDeltaUsInternal(uint32_t frameId, uint64_t& outDeltaTimeUs);
sl::Result slReflexGetFrameGenParamsInternal(uint32_t frameId, uint8_t& outFgMultiplier, bool& outDfgControl);

//! Define our plugin, make sure to update version numbers in versions.h
SL_PLUGIN_DEFINE("sl.reflex", Version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH), Version(0, 0, 1), JSON.c_str(), updateEmbeddedJSON, reflex, LatencyContext)

//! Figure out if we are supported on the current hardware or not
//! 
void updateEmbeddedJSON(json& config)
{
    // Latency can be GPU agnostic to some degree
    uint32_t adapterMask = ~0;

    auto& ctx = (*reflex::getContext());

    // Defaults everything to false
    ctx.lowLatencyAvailable = false;
    ctx.latencyReportAvailable = false;
    ctx.flashIndicatorDriverControlled = false;

    // Check if plugin is supported or not on this platform and set the flag accordingly
    common::SystemCaps* caps = {};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kSystemCaps, &caps);
    common::PFunUpdateCommonEmbeddedJSONConfig* updateCommonEmbeddedJSONConfig{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunUpdateCommonEmbeddedJSONConfig, &updateCommonEmbeddedJSONConfig);
    if (caps && updateCommonEmbeddedJSONConfig)
    {
        // All defaults since sl.reflex can run on any adapter
        common::PluginInfo info{};
        info.SHA = GIT_LAST_COMMIT_SHORT;
        updateCommonEmbeddedJSONConfig(&config, info);
    }

    // Figure out if we should use NVAPI or not
    // 
    // NVDA driver has to be 455+ otherwise Reflex Low Latency won't work
    sl::Version minDriver(455, 0, 0);
    if (caps && caps->driverVersionMajor > 455)
    {
        // We start with Pascal+ then later check again if GetSleepStatus returns error or not
        for (uint32_t i = 0; i < caps->gpuCount; i++)
        {
            ctx.lowLatencyAvailable |= caps->adapters[i].architecture >= NV_GPU_ARCHITECTURE_ID::NV_GPU_ARCHITECTURE_GM000;
            // Starting since 511.23 flash indicator should be controlled by GFE instead of application
            ctx.flashIndicatorDriverControlled |= (caps->driverVersionMajor * 100 + caps->driverVersionMinor) >= 51123;
        }
    }

    std::unordered_set<std::string> deviceExtensions;
    if (ctx.lowLatencyAvailable)
    {
        // Only require the Vk extension if Reflex is actually supported
        deviceExtensions.emplace("VK_NV_low_latency");
    }

    // VK_NV_low_latency2 is the modern Reflex path. A new interposer honors optional_extensions and
    // enables it when the device exposes it. An older interposer ignores that key, so under a
    // conservative heuristic (NV adapter) we append it to the strict list instead. The two paths
    // are mutually exclusive to avoid declaring the same extension in both buckets.
    //
    // The strict-list path bypasses the interposer's device-exposes-it check, so the heuristic must
    // not enable LL2 on adapters that don't support it. Pre-Turing support is dropped, so gate on Turing.
    bool ll2HeuristicSafe = false;
    if (ctx.lowLatencyAvailable && caps)
    {
        for (uint32_t i = 0; i < caps->gpuCount; i++)
        {
            if (caps->adapters[i].vendor == chi::VendorId::eNVDA &&
                caps->adapters[i].architecture >= NV_GPU_ARCHITECTURE_ID::NV_GPU_ARCHITECTURE_TU100)
            {
                ll2HeuristicSafe = true;
                break;
            }
        }
    }
    if (ll2HeuristicSafe)
    {
        deviceExtensions.insert(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    }
    else
    {
        config["external"]["vk"]["device"]["optional_extensions"] = std::unordered_set<std::string>{
            VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
        };
    }

    config["external"]["vk"]["device"]["extensions"] = deviceExtensions;
    config["external"]["reflex"]["lowLatencyAvailable"] = ctx.lowLatencyAvailable;
    config["external"]["reflex"]["flashIndicatorDriverControlled"] = ctx.flashIndicatorDriverControlled;
}

//! Update stats shown on screen
void updateStats(uint32_t presentFrameIndex)
{
#ifndef SL_PRODUCTION
    auto& ctx = (*reflex::getContext());
    const std::string mode[ReflexMode_eCount] = { "Off", "On", "On + boost" };

    std::scoped_lock lock(ctx.uiStats.mtx);
    ctx.uiStats.mode = "Mode: " + mode[ctx.constants.mode];
    ctx.uiStats.markers = extra::format("Optimize with markers: {}", (ctx.constants.useMarkersToOptimize ? "Yes" : "No"));
    ctx.uiStats.fpsCap = extra::format("FPS cap: {}us", ctx.constants.frameLimitUs);
    if(presentFrameIndex) ctx.uiStats.presentFrame = extra::format("Present marker frame: {}", presentFrameIndex);
    ctx.uiStats.sleeping = extra::format("Sleeping: {}ms", ctx.sleepMeter.getMean());
#endif
}

//! Set constants for our plugin (if any, this is optional and should be thread safe)
Result slSetData(const BaseStructure* inputs, CommandBuffer* cmdBuffer)
{
    auto& ctx = (*reflex::getContext());

    if (!ctx.compute)
    {
        SL_LOG_WARN("Reflex: no compute interface");
        return Result::eErrorInvalidIntegration;
    }

    auto marker = findStruct<ReflexHelper>(inputs);
    auto consts = findStruct<ReflexOptions>(inputs);
    auto frame = findStruct<FrameToken>(inputs);
    
    if (marker && frame)
    {
        const MarkerUnderlying evd_id = *marker;
        // Special 'marker' for low latency mode
        if (evd_id == kReflexMarkerSleep)
        {
            if (ctx.lowLatencyAvailable)
            {
#ifdef SL_PRODUCTION
                ctx.lowLatencyAvailable = ctx.compute->sleep() == chi::ComputeStatus::eOk;
#else
                ctx.sleepMeter.begin();
                ctx.lowLatencyAvailable = ctx.compute->sleep() == chi::ComputeStatus::eOk;
                if (!ctx.lowLatencyAvailable)
                {
                    SL_LOG_WARN("Reflex sleep failed");
                }
                ctx.sleepMeter.end();
#endif
            }
        }
        else
        {
            // Made sure it's not special kReflexMarkerSleep value, so should be "safe" to cast to valid PCLMarker enum
            assert(evd_id < to_underlying(PCLMarker::eMaximum));
            const PCLMarker pcl_marker = (PCLMarker)evd_id;
            if (ctx.lowLatencyAvailable && pcl_marker != PCLMarker::ePCLatencyPing
                && (pcl_marker != PCLMarker::eTriggerFlash || ctx.flashIndicatorDriverControlled))
            {
                CHI_VALIDATE(ctx.compute->setReflexMarker(pcl_marker, *frame));
            }

            if (pcl_marker == PCLMarker::ePresentStart)
            {
                // The app emits its own PRESENT markers. Latch this so SL stops emitting its own on
                // the Vulkan single-channel path and avoids double-stamping the driver (see
                // Vulkan::setAsyncFrameMarker). Apps that never send a present marker (e.g. Unity,
                // handled below via a render-marker substitute) leave this false, so SL keeps
                // supplying present markers for them.
                ctx.compute->setAppOwnsPresentMarkers(true);
            }

            if (pcl_marker == PCLMarker::ePresentStart
                // Special case for Unity, it is hard to provide present markers so using render markers
                || (ctx.engine == EngineType::eUnity && pcl_marker == PCLMarker::eRenderSubmitEnd)
                )
            {
                // This frame-id assists present-time SL features like DLSS FG and LW to detect id of the frame 
                // being currently processed on the present thread.
                api::getContext()->parameters->set(sl::param::latency::kMarkerPresentFrame, *frame);
                updateStats(*frame);

                // Mark the last frame we were active
                //
                // NOTE: We do this on present marker only to prevent
                // scenarios where simulation marker for new frame comes in
                // and advances the frame index
                if (ctx.enabled.load())
                {
                    uint32_t frame = 0;
                    CHI_VALIDATE(ctx.compute->getFinishedFrameIndex(frame));
                    api::getContext()->parameters->set(sl::param::latency::kCurrentFrame, frame + 1);
                }
            }

            // DEPRECATED (reflex-pcl):
            PCLHelper helper{pcl_marker};
            helper.next = (BaseStructure*)frame;
            auto res = ctx.pclSetData(&helper, cmdBuffer);
            if (res != Result::eOk)
            {
                SL_LOG_WARN("Reflex-PCL: PCLSetData failed %d", res);
                return res;
            }
        }
    }
    else
    {
        if (!consts)
        {
            SL_LOG_WARN("Reflex: no consts");
            return Result::eErrorMissingInputParameter;
        }
        if (!ctx.lowLatencyAvailable)
        {
            // At the moment low latency is only possible on NVDA hw
            if (consts->mode == ReflexMode::eLowLatency || consts->mode == ReflexMode::eLowLatencyWithBoost)
            {
                SL_LOG_WARN_ONCE("Low-latency modes are only supported on NVIDIA hardware through Reflex, collecting latency stats only");
            }
        }
        
        // DEPRECATED (reflex-pcl):
        {
            PCLHotKey hotkey{};
            switch (consts->virtualKey)
            {
                case 0: break;
                case VK_F13: { hotkey = PCLHotKey::eVK_F13; } break;
                case VK_F14: { hotkey = PCLHotKey::eVK_F14; } break;
                case VK_F15: { hotkey = PCLHotKey::eVK_F15; } break;
                default:
                    SL_LOG_ERROR("Latency virtual key can only be assigned to VK_F13, VK_F14 or VK_F15");
                    return Result::eErrorInvalidParameter;
            }
            PCLOptions options;
            options.virtualKey = hotkey;
            options.idThread = consts->idThread;
            auto res = ctx.pclSetData(&options, cmdBuffer);
            if (res != Result::eOk)
            {
                SL_LOG_WARN("Reflex-PCL: PCLSetData failed %d", res);
                return res;
            }
        }

        {
            ctx.constants = *consts;
            ctx.enabled.store(consts->mode != ReflexMode::eOff);
#ifndef SL_PRODUCTION
            // Override from config (if any)
            if (ctx.frameLimitUs != UINT_MAX)
            {
                ctx.constants.frameLimitUs = ctx.frameLimitUs;
            }
            if (ctx.useMarkersToOptimizeOverride)
            {
                ctx.constants.useMarkersToOptimize = ctx.useMarkersToOptimizeOverrideValue;
            }
#endif
            if (ctx.lowLatencyAvailable)
            {
                CHI_VALIDATE(ctx.compute->setSleepMode(ctx.constants));
            }
            updateStats(0);
        }
    }
    
    return Result::eOk;
}

Result slGetData(const BaseStructure* inputs, BaseStructure* outputs, CommandBuffer* cmdBuffer)
{
    SL_PLUGIN_INIT_CHECK();
    auto& ctx = (*reflex::getContext());
    
    auto settings = findStruct<ReflexState>(outputs);
    if (!settings)
    {
        return Result::eErrorMissingInputParameter;
    }
    // Based on hw and driver we assume that low latency should be available
    if (ctx.compute && ctx.lowLatencyAvailable)
    {
        // NVAPI call can still fail so adjust flags
        ctx.lowLatencyAvailable = ctx.compute->getSleepStatus(*settings) == chi::ComputeStatus::eOk;
        ctx.latencyReportAvailable = ctx.compute->getLatencyReport(*settings) == chi::ComputeStatus::eOk;
    }
    settings->lowLatencyAvailable = ctx.lowLatencyAvailable;
    settings->latencyReportAvailable = ctx.latencyReportAvailable;
    settings->flashIndicatorDriverControlled = ctx.flashIndicatorDriverControlled;

    // DEPRECATED (reflex-pcl):
    {
        PCLState state{};
        auto res = ctx.pclGetData(inputs, &state, cmdBuffer);
        if (res != Result::eOk)
        {
            SL_LOG_WARN("Reflex-PCL: PCLGetData failed %d", res);
            return res;
        }
        settings->statsWindowMessage = state.statsWindowMessage;
    }

    return Result::eOk;
}


internal::shared::Status getSharedData(BaseStructure* requestedData, const BaseStructure* requesterInfo)
{
    if (!requestedData || requestedData->structType != reflex::ReflexInternalSharedData::s_structType)
    {
        SL_LOG_ERROR("Invalid request is made for shared data");
        return internal::shared::Status::eInvalidRequestedData;
    }
    auto remote = static_cast<reflex::ReflexInternalSharedData*>(requestedData);

    // v1
    remote->slReflexSetMarker = slReflexSetMarker;

    // v2
    remote->slReflexGetCameraData = slReflexGetCameraDataInternal;

    // v3
    remote->slReflexSetCameraDataFence = slReflexSetCameraDataFenceInternal;

    remote->reservedV4 = nullptr;

    // v5
    remote->slReflexGetSimulationDeltaUs = slReflexGetSimulationDeltaUsInternal;

    // v6
    remote->slReflexSetCameraDataCallback = slReflexSetCameraDataCallbackInternal;

    // v7
    remote->slReflexGetFrameGenParams = slReflexGetFrameGenParamsInternal;

    memset(remote->reservedV7, 0, sizeof(remote->reservedV7));

    // Let newer requester know that we are older
    if (remote->structVersion > kStructVersion7)
    {
        remote->structVersion = kStructVersion7;
    }

    return internal::shared::Status::eOk;
}

sl::Result predictCameraData(const ReflexCameraData& cameraData, float4x4& prevWorldToView, float4x4& prevViewToClip, ReflexPredictedCameraData& predictedCameraData)
{
    float4x4 ViewToWorld, PrevViewToWorld;
    matrixOrthoNormalInvert(ViewToWorld, cameraData.worldToViewMatrix);
    matrixOrthoNormalInvert(PrevViewToWorld, prevWorldToView);

    // 1st order prediction (simple constant velocity for now): 
    const float4& currentTranslation = ViewToWorld.getRow(3);
    float4 prevTranslation = PrevViewToWorld.getRow(3);
    float4 predictedTranslation = float4(currentTranslation.x, currentTranslation.y, currentTranslation.z, 1);
    predictedTranslation.x += currentTranslation.x - prevTranslation.x;
    predictedTranslation.y += currentTranslation.y - prevTranslation.y;
    predictedTranslation.z += currentTranslation.z - prevTranslation.z;

    float4x4 currentRotation;
    currentRotation[0].x = ViewToWorld[0].x; currentRotation[0].y = ViewToWorld[0].y; currentRotation[0].z = ViewToWorld[0].z; currentRotation[0].w = 0.f;
    currentRotation[1].x = ViewToWorld[1].x; currentRotation[1].y = ViewToWorld[1].y; currentRotation[1].z = ViewToWorld[1].z; currentRotation[1].w = 0.f;
    currentRotation[2].x = ViewToWorld[2].x; currentRotation[2].y = ViewToWorld[2].y; currentRotation[2].z = ViewToWorld[2].z; currentRotation[2].w = 0.f;
    currentRotation[3].x = 0.f;              currentRotation[3].y = 0.f;              currentRotation[3].x = 0.f;              currentRotation[3].x = 1.f;

    float4x4 inversePrevRotation;
    inversePrevRotation[0].x = PrevViewToWorld[0].x; inversePrevRotation[0].y = PrevViewToWorld[1].x; inversePrevRotation[0].z = PrevViewToWorld[2].x; inversePrevRotation[0].w = 0.f;
    inversePrevRotation[1].x = PrevViewToWorld[0].y; inversePrevRotation[1].y = PrevViewToWorld[1].y; inversePrevRotation[1].z = PrevViewToWorld[2].y; inversePrevRotation[1].w = 0.f;
    inversePrevRotation[2].x = PrevViewToWorld[0].z; inversePrevRotation[2].y = PrevViewToWorld[1].z; inversePrevRotation[2].z = PrevViewToWorld[2].z; inversePrevRotation[2].w = 0.f;
    inversePrevRotation[3].x = 0.f;                  inversePrevRotation[3].y = 0.f;                  inversePrevRotation[3].x = 0.f;                  inversePrevRotation[3].x = 1.f;

    float4x4 deltaRotation;
    matrixMul(deltaRotation, currentRotation, inversePrevRotation);

    matrixMul(predictedCameraData.predictedWorldToViewMatrix, deltaRotation, currentRotation);
    predictedCameraData.predictedWorldToViewMatrix[3].x = predictedTranslation.x;
    predictedCameraData.predictedWorldToViewMatrix[3].y = predictedTranslation.y;
    predictedCameraData.predictedWorldToViewMatrix[3].z = predictedTranslation.z;
    predictedCameraData.predictedWorldToViewMatrix[3].w = 1.f;

    // TODO: Predict viewtoclip
    predictedCameraData.predictedViewToClipMatrix = cameraData.viewToClipMatrix;

    return sl::Result::eOk;
}

sl::Result slReflexSetCameraData(const sl::ViewportHandle& viewport, const sl::FrameToken& frame, const sl::ReflexCameraData& inCameraData)
{
    auto& ctx = (*reflex::getContext());

    ctx.compute->setReflexMarker(PCLMarker::eCameraConstructed, frame);
    ctx.setStatsMarkerFunc(PCLMarker::eCameraConstructed, frame);

    if (ctx.predictCamera && frame > 0)
    {
        ReflexPredictedCameraData predictedCameraData{};
        predictCameraData(inCameraData, ctx.prevWorldToViewMatrix, ctx.prevViewToClipMatrix, predictedCameraData);
        ctx.predCameraData.insertCameraData(frame, predictedCameraData);
    }

    ctx.simCameraData.insertCameraData(frame, inCameraData);

    ctx.prevWorldToViewMatrix = inCameraData.worldToViewMatrix;
    ctx.prevViewToClipMatrix = inCameraData.viewToClipMatrix;

    std::lock_guard<std::mutex> lock(ctx.callbacksMutex);
    if (ctx.cameraDataCallback)
    {
        ctx.cameraDataCallback(frame, inCameraData);
    }

    return sl::Result::eOk;
}

sl::Result slReflexGetCameraDataInternal(const sl::ViewportHandle& viewport, const uint32_t frame, sl::ReflexCameraData& outCameraData)
{
    auto& ctx = (*reflex::getContext());

    std::optional<ReflexCameraData> cameraData = ctx.simCameraData.getCameraData(frame);
    if (!cameraData.has_value())
    {
        SL_LOG_WARN("Could not get camera data for frame %d", frame);
        return Result::eErrorInvalidState;
    }

    outCameraData = cameraData.value();
    return sl::Result::eOk;
}

sl::Result slReflexSetCameraDataFenceInternal(const sl::ViewportHandle& viewport, sl::chi::Fence fence, const uint32_t syncValue, chi::ICommandListContext* cmdList)
{
    auto& ctx = (*reflex::getContext());
    ctx.gameWaitCmdList = cmdList;
    ctx.gameWaitFence = fence;
    ctx.gameWaitSyncValue = syncValue;
    return sl::Result::eOk;
}

sl::Result slReflexSetCameraDataCallbackInternal(std::function<void(uint32_t frame, const sl::ReflexCameraData& cameraData)> callback)
{
    auto& ctx = (*reflex::getContext());
    std::lock_guard<std::mutex> lock(ctx.callbacksMutex);
    ctx.cameraDataCallback = callback;
    return sl::Result::eOk;
}

sl::Result slReflexGetPredictedCameraData(const sl::ViewportHandle& viewport, const sl::FrameToken& frame, sl::ReflexPredictedCameraData& outCameraData)
{
    auto& ctx = (*reflex::getContext());
    ctx.predictCamera = true;

    std::optional<ReflexPredictedCameraData> cameraData = ctx.predCameraData.getCameraData(frame);
    if (!cameraData.has_value())
    {
        SL_LOG_WARN("Could not get predicted camera data for frame %d", frame);
        return Result::eErrorInvalidState;
    }

    outCameraData = cameraData.value();
    return sl::Result::eOk;
}

Result slReflexGetSimulationDeltaUsInternal(uint32_t frameId, uint64_t& outDeltaTimeUs)
{
    auto& ctx = (*reflex::getContext());

    if (frameId <= 1)
    {
        return Result::eErrorInvalidState;
    }

    std::lock_guard<std::mutex> lock(ctx.simulationTimingMutex);

    const auto& current = ctx.simulationStartData[frameId % kSimulationTimingHistorySize];
    if (current.frameId != frameId)
    {
        return Result::eErrorInvalidState;
    }

    const auto& previous = ctx.simulationStartData[(frameId - 1) % kSimulationTimingHistorySize];
    if (previous.frameId != (frameId - 1))
    {
        return Result::eErrorInvalidState;
    }

    outDeltaTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(current.timestamp - previous.timestamp).count();

    return Result::eOk;
}

Result slReflexGetFrameGenParamsInternal(uint32_t frameId, uint8_t& outFgMultiplier, bool& outDfgControl)
{
    auto& ctx = (*reflex::getContext());

    if (frameId == 0)
    {
        return Result::eErrorInvalidState;
    }

    std::lock_guard<std::mutex> lock(ctx.simulationTimingMutex);

    const auto& entry = ctx.simulationStartData[frameId % kSimulationTimingHistorySize];
    if (entry.frameId != frameId)
    {
        return Result::eErrorInvalidState;
    }

    outFgMultiplier = entry.driverFgMultiplier;
    outDfgControl = entry.bDfgControl;

    return Result::eOk;
}

//! Main entry point - starting our plugin
//! 
//! IMPORTANT: Plugins are started based on their priority.
//! sl.common always starts first since it has priority 0
//!
bool slOnPluginStartup(const char* jsonConfig, void* device)
{
    //! Common startup and setup
    //!     
    SL_PLUGIN_COMMON_STARTUP();

    auto& ctx = (*reflex::getContext());

    auto parameters = api::getContext()->parameters;

    //! Register our evaluate callbacks
    //!
    //! Note that sl.common handles evaluate calls from the host
    //! and distributes eval calls to the right plugin based on the feature id
    //! 
    if (!param::getPointerParam(parameters, param::common::kPFunRegisterEvaluateCallbacks, &ctx.registerEvaluateCallbacks))
    {
        SL_LOG_ERROR( "Cannot obtain `registerEvaluateCallbacks` interface - check that sl.common was initialized correctly");
        return false;
    }

    // DEPRECATED (reflex-pcl):
    if (!param::getPointerParam(parameters, sl::param::_deprecated_reflex_pcl::kSlGetData, &ctx.pclGetData)
        || !param::getPointerParam(parameters, sl::param::_deprecated_reflex_pcl::kSlSetData, &ctx.pclSetData))
    {
        SL_LOG_ERROR("Failed to get PCL implementation");
        return false;
    }
    
    //! Plugin manager gives us the device type and the application id
    //! 
    json& config = *(json*)api::getContext()->loaderConfig;
    uint32_t deviceType{};
    int appId{};
    config.at("appId").get_to(appId);
    config.at("deviceType").get_to(deviceType);
    if (config.contains("ngx"))
    {
        config.at("ngx").at("engineType").get_to(ctx.engine);
        if (ctx.engine == EngineType::eUnity)
        {
            SL_LOG_INFO("Detected Unity engine - using render submit markers instead of present to detect current frame");
        }
    }
    //! Now let's obtain compute interface if we need to dispatch some compute work
    //! 
    ctx.platform = (RenderAPI)deviceType;
    if (!param::getPointerParam(parameters, sl::param::common::kComputeAPI, &ctx.compute))
    {
        SL_LOG_ERROR( "Cannot obtain compute interface - check that sl.common was initialized correctly");
        return false;
    }

    json& extraConfig = *(json*)api::getContext()->extConfig;
    if (extraConfig.contains("frameLimitUs"))
    {
        extraConfig.at("frameLimitUs").get_to(ctx.frameLimitUs);
        SL_LOG_HINT("Read 'frameLimitUs' %u from JSON config", ctx.frameLimitUs);
    }
    if (extraConfig.contains("useMarkersToOptimize"))
    {
        extraConfig.at("useMarkersToOptimize").get_to(ctx.useMarkersToOptimizeOverrideValue);
        ctx.useMarkersToOptimizeOverride = true;
        SL_LOG_HINT("Read 'useMarkersToOptimize' %u from JSON config", ctx.useMarkersToOptimizeOverrideValue);
    }

    updateStats(0);
    parameters->set(internal::shared::getParameterNameForFeature(kFeatureReflex).c_str(), (void*)getSharedData);

#ifndef SL_PRODUCTION
    // Check for UI and register our callback
    imgui::ImGUI* ui{};
    param::getPointerParam(parameters, param::imgui::kInterface, &ui);
    if (ui)
    {
        // Runs async from the present thread where UI is rendered just before frame is presented
        auto renderUI = [&ctx](imgui::ImGUI* ui, bool finalFrame)->void
        {
            auto v = api::getContext()->pluginVersion;
            if (ui->collapsingHeader(extra::format("{} v{}", api::getPluginName(), (v.toStr() + "." + GIT_LAST_COMMIT_SHORT)).c_str(), imgui::kTreeNodeFlagDefaultOpen))
            {
                std::scoped_lock lock(ctx.uiStats.mtx);
                ui->text(ctx.uiStats.mode.c_str());
                ui->text(ctx.uiStats.markers.c_str());
                ui->text(ctx.uiStats.fpsCap.c_str());
                ui->text(ctx.uiStats.presentFrame.c_str());
                ui->text(ctx.uiStats.sleeping.c_str());
            }
        };
        ui->registerRenderCallbacks(renderUI, nullptr);
    }
#endif

    param::getPointerParam(parameters, param::pcl::kPFunSetPCLStatsMarker, &ctx.setStatsMarkerFunc);

    return true;
}

//! Main exit point - shutting down our plugin
//! 
//! IMPORTANT: Plugins are shutdown in the inverse order based to their priority.
//! sl.common always shuts down LAST since it has priority 0
//!
void slOnPluginShutdown()
{
    auto& ctx = (*reflex::getContext());

    // If we used 'evaluate' mechanism reset the callbacks here
    ctx.registerEvaluateCallbacks(kFeatureReflex, nullptr, nullptr);

    // Common shutdown
    plugin::onShutdown(api::getContext());
}

//! Exports from sl_reflex.h
//! 
sl::Result slReflexGetState(sl::ReflexState& state)
{
    return slGetData(nullptr, &state, nullptr);
}

sl::Result slReflexSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    auto& ctx = (*reflex::getContext());
    sl::ReflexHelper inputs(marker);
    inputs.next = (BaseStructure*)&frame;


    // Track simulation start timing and query DFG driver info
    if (marker == sl::PCLMarker::eSimulationStart)
    {
        uint8_t fgMult = 0;
        bool dfgCtrl = false;
        bool driverOk = false;
        // NVAPI query - only safe where Reflex low latency is available (NV hw)
        if (ctx.lowLatencyAvailable && ctx.platform != RenderAPI::eVulkan)
        {
            driverOk = ctx.compute->getFrameGenParams(fgMult, dfgCtrl) == chi::ComputeStatus::eOk;
        }

        std::lock_guard<std::mutex> lock(ctx.simulationTimingMutex);

        auto& entry = ctx.simulationStartData[frame % kSimulationTimingHistorySize];
        entry.frameId = frame;
        entry.timestamp = std::chrono::high_resolution_clock::now();
        entry.driverFgMultiplier = driverOk ? fgMult : 0;
        entry.bDfgControl = driverOk && dfgCtrl;
    }

    if (marker == sl::PCLMarker::eRenderSubmitStart && ctx.gameWaitCmdList && ctx.gameWaitFence && ctx.gameWaitSyncValue && ctx.compute->getCompletedValue(ctx.gameWaitFence) < ctx.gameWaitSyncValue)
    {
        ctx.compute->waitCPUFence(ctx.gameWaitFence, ctx.gameWaitSyncValue);
    }

    return slSetData(&inputs, nullptr);
}

sl::Result slReflexSleep(const sl::FrameToken& frame)
{
    sl::ReflexHelper inputs(kReflexMarkerSleep);
    inputs.next = (BaseStructure*)&frame;
    return slSetData(&inputs,nullptr);
}

sl::Result slReflexSetOptions(const sl::ReflexOptions& options)
{
    return slSetData(&options, nullptr);
}

// Storage for the VkSwapchainLatencyCreateInfoNV we may inject into the host's
// vkCreateSwapchainKHR pNext chain. Its lifetime must extend past the before-hook return so the
// interposer's underlying CreateSwapchainKHR (which runs after us, on the same thread) sees it.
// Thread-local rather than in the shared plugin context because vkCreateSwapchainKHR is not
// externally synchronized: two threads can create swapchains concurrently. Each thread runs
// before-hook -> real create -> after-hook serially, so one slot per thread is exactly the right
// lifetime and there is no cross-thread sharing to race on.
namespace { thread_local VkSwapchainLatencyCreateInfoNV t_swapchainLatencyInfo{}; }

VkResult slHookVkCreateSwapchainKHRBefore(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*, VkSwapchainKHR*, bool& skip)
{
    skip = false;

    // Only inject VkSwapchainLatencyCreateInfoNV when the LL2 device extension is actually enabled
    // on this device. Resolving via vkGetDeviceProcAddr returns null when the extension wasn't
    // enabled at vkCreateDevice time, which mirrors the prior interposer-side gate.
    interposer::VkTable* vk{};
    if (!param::getPointerParam(api::getContext()->parameters, sl::param::global::kVulkanTable, &vk) || !vk)
    {
        return VK_SUCCESS;
    }
    auto pfnLatencySleepNV = (PFN_vkLatencySleepNV)vk->getDeviceProcAddr(device, "vkLatencySleepNV");
    if (!pfnLatencySleepNV)
    {
        return VK_SUCCESS;
    }

    // Walk the existing pNext chain to see if the host already provided VkSwapchainLatencyCreateInfoNV.
    auto* nonConstInfo = const_cast<VkSwapchainCreateInfoKHR*>(info);
    auto* next = (const VkBaseInStructure*)nonConstInfo->pNext;
    while (next)
    {
        if (next->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV)
        {
            if (!reinterpret_cast<const VkSwapchainLatencyCreateInfoNV*>(next)->latencyModeEnable)
            {
                SL_LOG_WARN("vkCreateSwapchainKHR pNext chain has VkSwapchainLatencyCreateInfoNV but latency-mode not enabled");
            }
            return VK_SUCCESS;
        }
        next = next->pNext;
    }

    // Prepend our own VkSwapchainLatencyCreateInfoNV. The struct lives in thread-local storage to
    // outlive this hook return; the interposer's underlying CreateSwapchainKHR runs after us.
    t_swapchainLatencyInfo = VkSwapchainLatencyCreateInfoNV{
        VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
        nonConstInfo->pNext,
        VK_TRUE,
    };
    nonConstInfo->pNext = &t_swapchainLatencyInfo;
    return VK_SUCCESS;
}

VkResult slHookVkCreateSwapchainKHRAfter(VkDevice, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*, VkSwapchainKHR* Swapchain)
{
    auto& ctx = (*reflex::getContext());
    auto next = (const VkBaseInStructure*)info->pNext;
    bool is_latency_mode_enabled = false;
    while (next)
    {
        if (next->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV)
        {
            is_latency_mode_enabled = reinterpret_cast<const VkSwapchainLatencyCreateInfoNV*>(next)->latencyModeEnable;
            break;
        }
        next = next->pNext;
    }
    ctx.compute->notifyCreateSwapchain(*Swapchain, is_latency_mode_enabled);
    return VK_SUCCESS;
}

void slHookVkDestroySwapchainKHRBefore(VkDevice, VkSwapchainKHR Swapchain, const VkAllocationCallbacks*, bool& skip)
{
    skip = false;
    // Fires before the driver frees the swapchain, so the LL2 backend can stop using the handle
    // (see chi::VkNvLowLatency2::notifyDestroySwapchain). No-op on the v1 / D3D paths.
    auto& ctx = (*reflex::getContext());
    if (ctx.compute)
    {
        ctx.compute->notifyDestroySwapchain(Swapchain);
    }
}

//! The only exported function - gateway to all functionality
SL_EXPORT void* slGetPluginFunction(const char* functionName)
{
    //! Core API
    SL_EXPORT_FUNCTION(slOnPluginLoad);
    SL_EXPORT_FUNCTION(slOnPluginShutdown);
    SL_EXPORT_FUNCTION(slOnPluginStartup);
    SL_EXPORT_FUNCTION(slSetData);
    SL_EXPORT_FUNCTION(slGetData);
    
    SL_EXPORT_FUNCTION(slReflexGetState);
    SL_EXPORT_FUNCTION(slReflexSetMarker);
    SL_EXPORT_FUNCTION(slReflexSleep);
    SL_EXPORT_FUNCTION(slReflexSetOptions);

    SL_EXPORT_FUNCTION(slHookVkCreateSwapchainKHRBefore);
    SL_EXPORT_FUNCTION(slHookVkCreateSwapchainKHRAfter);
    SL_EXPORT_FUNCTION(slHookVkDestroySwapchainKHRBefore);

    SL_EXPORT_FUNCTION(slReflexSetCameraData);
    SL_EXPORT_FUNCTION(slReflexGetPredictedCameraData);

    return nullptr;
}

}
