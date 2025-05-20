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

#pragma once

#include <d3d11_4.h>
#include <future>

#include "source/core/sl.thread/thread.h"
#include "source/platforms/sl.chi/generic.h"
#include "source/core/sl.extra/extra.h"

namespace sl
{
namespace chi
{

constexpr uint32_t kMaxD3D11Items = 8;

struct D3D11ThreadContext : public CommonThreadContext
{
    ID3D11SamplerState* engineSamplers[kMaxD3D11Items] = {};
    ID3D11ComputeShader* engineCS = {};
    ID3D11RenderTargetView* engineRTVs[kMaxD3D11Items] = {};
    ID3D11UnorderedAccessView* engineUAVs[kMaxD3D11Items] = {};
    ID3D11ShaderResourceView* engineSRVs[kMaxD3D11Items] = {};
    ID3D11DepthStencilView* engineDSV = {};
    ID3D11Buffer* engineConstBuffers[kMaxD3D11Items] = {};
};

struct KernelDataD3D11 : public KernelDataBase
{
    ID3D11ComputeShader* shader = {};
    std::map<uint32_t, ID3D11Buffer*> constBuffers = {};
    void destroy();
};

struct ResourceDriverDataD3D11
{
    uint32_t handle = 0;
    uint64_t virtualAddress = 0;
    uint64_t size = 0;
    uint32_t descIndex = 0;
    bool bZBCSupported = false;
    ID3D11UnorderedAccessView* UAV = nullptr;
    ID3D11ShaderResourceView* SRV = nullptr;
};

struct DispatchDataD3D11
{
    KernelDataD3D11* kernel;
};

class D3D11 : public Generic
{
    static constexpr uint32_t kResourceHashTypeSrv = 0;
    static constexpr uint32_t kResourceHashTypeUav = 1;

    struct PerfData
    {
        ID3D11Query *queryBegin{};
        ID3D11Query* queryEnd{};
        ID3D11Query* queryDisjoint{};
        extra::AverageValueMeter meter{};
    };
    using MapSectionPerf = std::map<std::string, PerfData>;
    MapSectionPerf m_sectionPerfMap[MAX_NUM_NODES] = {};

    ID3D11Device *m_device = nullptr;
    ID3D11Device5* m_device5 = nullptr;

    chi::Kernel m_copyKernel{};

    bool m_dbgSupportRs2RelaxedConversionRules = false;

    ID3D11DeviceContext* m_context{};
    ID3D11DeviceContext* m_immediateContext{};
    UINT m_visibleNodeMask = 0;

    std::map<ID3D11Resource*, std::map<uint64_t,ResourceDriverDataD3D11>> m_resourceData;
    ID3D11SamplerState* m_samplers[eSamplerCount];

    thread::ThreadContext<DispatchDataD3D11> m_dispatchContext;

    virtual int destroyResourceDeferredImpl(const Resource InResource) override final;
    virtual std::wstring getDebugName(Resource res) override final;

    ComputeStatus transitionResourceImpl(CommandList InCmdList, const ResourceTransition *transisitions, uint32_t count) override final;
    ComputeStatus createTexture2DResourceSharedImpl(ResourceDescription &InOutResourceDesc, Resource &OutResource, bool UseNativeFormat, ResourceState InitialState, const char InFriendlyName[]) override final;
    ComputeStatus createBufferResourceImpl(ResourceDescription &InOutResourceDesc, Resource &OutResource, ResourceState InitialState, const char InFriendlyName[]) override final;
    ComputeStatus getLUIDFromDevice(NVSDK_NGX_LUID *OutId);
    ComputeStatus getSurfaceDriverData(Resource resource, ResourceDriverDataD3D11& data, uint32_t mipOffset = 0);
    ComputeStatus getTextureDriverData(Resource resource, ResourceDriverDataD3D11& data, uint32_t mipOffset = 0, uint32_t mipLevels = 0, Sampler sampler = Sampler::eSamplerPointClamp);

    bool isSupportedFormat(DXGI_FORMAT format, int flag1, int flag2);
    DXGI_FORMAT getCorrectFormat(DXGI_FORMAT Format);

public:

    virtual ComputeStatus init(Device InDevice, param::IParameters* params) override final;
    virtual ComputeStatus shutdown() override final;
    virtual ComputeStatus clearCache() override final;

    virtual ComputeStatus getRenderAPI(RenderAPI &OutType) override final;

    // Don't fail this on d3d11 since it is irrelevant
    virtual ComputeStatus getNativeResourceState(ResourceState state, uint32_t& nativeState) override final { return ComputeStatus::eOk; };
    virtual ComputeStatus getResourceState(uint32_t nativeState, ResourceState& state) override final { return ComputeStatus::eOk; };
    virtual ComputeStatus getResourceState(Resource resource, ResourceState& state) override final { return ComputeStatus::eOk; };

    virtual ComputeStatus createKernel(void *InCubinBlob, unsigned int InCubinBlobSize, const char* fileName, const char *EntryPoint, Kernel &OutKernel) override final;
    virtual ComputeStatus destroyKernel(Kernel& kernel) override final;

    virtual ComputeStatus createCommandListContext(CommandQueue queue, uint32_t count, ICommandListContext*& ctx, const char friendlyName[]) override final;
    virtual ComputeStatus destroyCommandListContext(ICommandListContext* ctx) override final;

    virtual ComputeStatus createFence(FenceFlags flags, uint64_t initialValue, Fence& outFence, const char friendlyName[])  override final;

    virtual ComputeStatus createCommandQueue(CommandQueueType type, CommandQueue& queue, const char friendlyName[], uint32_t index) override final;
    virtual ComputeStatus destroyCommandQueue(CommandQueue& queue) override final;

    virtual ComputeStatus pushState(CommandList cmdList) override final;
    virtual ComputeStatus popState(CommandList cmdList) override final;

    virtual ComputeStatus setFullscreenState(SwapChain chain, bool fullscreen, Output out) override final;
    virtual ComputeStatus getRefreshRate(SwapChain chain, float& refreshRate) override final;
    virtual ComputeStatus getSwapChainBuffer(SwapChain chain, uint32_t index, Resource& buffer) override final;
    
    virtual ComputeStatus bindKernel(const Kernel InKernel) override final;
    virtual ComputeStatus bindSharedState(CommandList cmdList, UINT node) override final;
    virtual ComputeStatus bindSampler(uint32_t binding, uint32_t base, Sampler sampler) override final;
    virtual ComputeStatus bindConsts(uint32_t binding, uint32_t base, void *data, size_t dataSize, uint32_t instances) override final;
    virtual ComputeStatus bindTexture(uint32_t binding, uint32_t base, Resource resource, uint32_t mipOffset = 0, uint32_t mipLevels = 0) override final;
    virtual ComputeStatus bindRWTexture(uint32_t binding, uint32_t base, Resource resource, uint32_t mipOffset = 0) override final;
    virtual ComputeStatus bindRawBuffer(uint32_t binding, uint32_t base, Resource resource) override final;
    virtual ComputeStatus dispatch(unsigned int blockX, unsigned int blockY, unsigned int blockZ = 1) override final;

    virtual ComputeStatus clearView(CommandList cmdList, Resource InResource, const float4 Color, const RECT * pRect, unsigned int NumRects, CLEAR_TYPE &outType) override final;
    
    virtual ComputeStatus insertGPUBarrierList(CommandList cmdList, const Resource* InResources, unsigned int InResourceCount, BarrierType InBarrierType = eBarrierTypeUAV) override final;
    virtual ComputeStatus insertGPUBarrier(CommandList cmdList, Resource InResource, BarrierType InBarrierType) override final;
    virtual ComputeStatus copyResource(CommandList cmdList, Resource InDstResource, Resource InSrcResource) override final;
    virtual ComputeStatus cloneResource(Resource InResource, Resource &OutResource, const char friendlyName[], ResourceState InitialState, unsigned int InCreationMask, unsigned int InVisibilityMask) override final;
    virtual ComputeStatus copyBufferToReadbackBuffer(CommandList cmdList, Resource InResource, Resource OutResource, unsigned int InBytesToCopy) override final;
    virtual ComputeStatus getResourceDescription(Resource InResource, ResourceDescription &OutDesc) override final;

    virtual ComputeStatus mapResource(CommandList cmdList, Resource resource, void*& data, uint32_t subResource = 0, uint64_t offset = 0, uint64_t totalBytes = UINT64_MAX) override final;
    virtual ComputeStatus unmapResource(CommandList cmdList, Resource resource, uint32_t subResource) override final;

    virtual ComputeStatus copyHostToDeviceBuffer(CommandList InCmdList, uint64_t InSize, const void* InData, Resource InUploadResource, Resource InTargetResource, unsigned long long InUploadOffset, unsigned long long InDstOffset) override final;
    virtual ComputeStatus copyHostToDeviceTexture(CommandList InCmdList, uint64_t InSize, uint64_t RowPitch, const void* InData, Resource InTargetResource, Resource& InUploadResource) override final;

    virtual ComputeStatus setDebugName(Resource res, const char friendlyName[]) override final;

    virtual ComputeStatus beginPerfSection(CommandList cmdList, const char *key, unsigned int node, bool InReset = false) override final;
    virtual ComputeStatus endPerfSection(CommandList cmdList, const char *key, float &OutAvgTimeMS, unsigned int node) override final;
    virtual ComputeStatus beginProfiling(CommandList cmdList, UINT metadata, const char* marker) override final;
    virtual ComputeStatus endProfiling(CommandList cmdList) override final;

    virtual bool signalCPUFence(Fence fence, uint64_t syncValue) override final;

    virtual ComputeStatus notifyOutOfBandCommandQueue(CommandQueue queue, OutOfBandCommandQueueType type) override final;
    virtual ComputeStatus setAsyncFrameMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId) override final;
    virtual ComputeStatus setLatencyMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId) override final;

    virtual ComputeStatus prepareTranslatedResources(CommandList cmdList, const std::vector<std::pair<chi::TranslatedResource, chi::ResourceDescription>>& resourceList) override final;
    virtual ComputeStatus createSharedHandle(Resource res, Handle& handle)  override final;
    virtual ComputeStatus destroySharedHandle(Handle& handle)  override final;
    virtual ComputeStatus getResourceFromSharedHandle(ResourceType type, Handle handle, Resource& res)  override final;
};

}
}
