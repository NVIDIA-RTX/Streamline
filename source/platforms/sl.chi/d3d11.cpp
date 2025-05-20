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

#include <d3d11_4.h>
#include <wrl/client.h>

#include "source/core/sl.log/log.h"
#include "source/platforms/sl.chi/d3d11.h"
#include "nvapi.h"
#include "_artifacts/shaders/copy_cs.h"

namespace sl
{
namespace chi
{

D3D11 s_d3d11;
ICompute *getD3D11()
{
    return &s_d3d11;
}

struct D3D11CommandListContext : public ICommandListContext
{
    ID3D11DeviceContext4* m_cmdCtxImmediate{};
    std::wstring m_name;
    uint64_t m_syncValue = 0;
    Fence m_fence{};
    ICompute* m_compute{};

    void init(const char* debugName, ID3D11Device* device, ICompute* ci)
    {
        std::string tmp = debugName;
        m_name = extra::utf8ToUtf16(debugName);
        m_compute = ci;

        ID3D11DeviceContext* cmdCtx;
        device->GetImmediateContext(&cmdCtx);
        if (cmdCtx)
        {
            cmdCtx->Release();
            cmdCtx->QueryInterface(&m_cmdCtxImmediate);
            if (!m_cmdCtxImmediate)
            {
                SL_LOG_ERROR( "Failed to obtain ID3D11DeviceContext4");
            }
            else
            {
                m_compute->createFence(eFenceFlagsShared, m_syncValue, m_fence, "sl.dlssg.d3d11.fence");
            }
        }
    }

    void shutdown()
    {
        m_compute->destroyFence(m_fence);
        SL_SAFE_RELEASE(m_cmdCtxImmediate);
    }

    RenderAPI getType() { return RenderAPI::eD3D11; }

    bool signalGPUFenceAt(uint32_t index) override
    {
        return signalGPUFence(m_fence, ++m_syncValue);
    }

    bool signalGPUFence(Fence fence, uint64_t syncValue) override
    {
        if (FAILED(m_cmdCtxImmediate->Signal((ID3D11Fence*)fence, syncValue)))
        {
            SL_LOG_ERROR( "Failed to signal on the command queue");
            return false;
        }
        return true;
    }

    WaitStatus waitCPUFence(Fence fence, uint64_t syncValue)
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
        return WaitStatus::eError;
    }

    void waitGPUFence(Fence fence, uint64_t syncValue, const DebugInfo &debugInfo) override
    {
        if (FAILED(m_cmdCtxImmediate->Wait((ID3D11Fence*)fence, syncValue)))
        {
            SL_LOG_ERROR( "Failed to signal on the command queue");
        }
    }

    CommandList getCmdList()
    {
        return m_cmdCtxImmediate;
    }

    CommandQueue getCmdQueue()
    {
        return m_cmdCtxImmediate;
    }

    CommandAllocator getCmdAllocator()
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return nullptr;
    }

    Handle getFenceEvent()
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return nullptr;
    }

    Fence getFence(uint32_t index)
    {
        // Only one fence in d3d11 case
        return m_fence;
    }

    bool beginCommandList()
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return false;
    }

    bool executeCommandList(const sl::chi::GPUSyncInfo*)
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return false;
    }

    bool isCommandListRecording()
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return false;
    }

    WaitStatus flushAll()
    {
        m_cmdCtxImmediate->Flush();
        return WaitStatus::eNoTimeout;
    }

    uint32_t getPrevCommandListIndex() override
    {
        return 0;
    }

    uint32_t getCurrentCommandListIndex()
    {
        return 0;
    }

    uint64_t getSyncValueAtIndex(uint32_t idx)
    {
        return m_syncValue;
    }
    
    SyncPoint getSyncPointAtIndex(uint32_t idx) override
    {
        return { m_fence, m_syncValue };
    }

    Fence getNextVkAcquireFence() override final
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
        return nullptr;
    }

    int acquireNextBufferIndex(SwapChain chain, uint32_t& index, sl::chi::Fence* semaphore)
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return 0;
    }

    WaitStatus waitForCommandListToFinish(uint32_t index)
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return WaitStatus::eError;
    }

    uint64_t getCompletedValue(Fence fence)
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return false;
    }

    bool didCommandListFinish(uint32_t index)
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return false;
    }

    void syncGPU(const GPUSyncInfo* info)
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
    }

    void waitOnGPUForTheOtherQueue(const ICommandListContext* other, uint32_t clIndex,
        uint64_t syncValue, const DebugInfo &debugInfo) override
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
    }

    WaitStatus waitForCommandList(FlushType ft)
    {
        assert(false);
        SL_LOG_ERROR( "Not implemented");
        return WaitStatus::eError;
    }

    int present(SwapChain chain, uint32_t sync, uint32_t flags, void* params)
    {
        BOOL fullscreen = FALSE;
        ((IDXGISwapChain*)chain)->GetFullscreenState(&fullscreen, nullptr);
        if (fullscreen || sync)
        {
            flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }
        else if (sync == 0)
        {
            flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
        
        HRESULT res{};
        if (params)
        {
            res = ((IDXGISwapChain1*)chain)->Present1(sync, flags, (DXGI_PRESENT_PARAMETERS*)params);
        }
        else
        {
            res = ((IDXGISwapChain*)chain)->Present(sync, flags);
        }
        return res;
    }

    void getFrameStats(SwapChain chain, void* frameStats)
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
        return;
    }

    void getLastPresentID(SwapChain chain, uint32_t& id)
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
        return;
    }

    void waitForVblank(SwapChain chain)
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
        return;
    }
};

std::wstring D3D11::getDebugName(Resource res)
{
    Microsoft::WRL::ComPtr<IUnknown> unknown = static_cast<IUnknown*>(res->native);
    Microsoft::WRL::ComPtr<ID3D11Resource> d3d11Resource;
    Microsoft::WRL::ComPtr<IDXGIObject> dxgi;
    unknown.As(&d3d11Resource);
    unknown.As(&dxgi);
    wchar_t name[128] = {};
    std::wstring wname = L"Unknown";
    if (d3d11Resource)
    {
        UINT size = sizeof(name);
        if (FAILED(d3d11Resource->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            char sname[128] = {};
            size = sizeof(sname);
            if (SUCCEEDED(d3d11Resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, sname)))
            {
                std::string tmp(sname);
                wname = std::wstring(tmp.begin(), tmp.end());
            }
        }
        else
        {
            wname = name;
        }
    }
    else if (dxgi)
    {
        UINT size = sizeof(name);
        if (FAILED(dxgi->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            char sname[128] = {};
            size = sizeof(sname);
            if (SUCCEEDED(dxgi->GetPrivateData(WKPDID_D3DDebugObjectName, &size, sname)))
            {
                std::string tmp(sname);
                wname = std::wstring(tmp.begin(), tmp.end());
            }
        }
        else
        {
            wname = name;
        }
    }
    return wname;
}

extern const char* getDXGIFormatStr(uint32_t format);

ComputeStatus D3D11::init(Device InDevice, param::IParameters* params)
{
    Generic::init(InDevice, params);

    m_device = (ID3D11Device*)InDevice;
    m_device->GetImmediateContext(&m_immediateContext);

    m_device->QueryInterface(&m_device5);
    if (!m_device5)
    {
        SL_LOG_ERROR( "Failed to obtain ID3D11Device5");
        return ComputeStatus::eError;
    }

    UINT NodeCount = 1;
    m_visibleNodeMask = (1 << NodeCount) - 1;

    if (NodeCount > MAX_NUM_NODES)
    {
        SL_LOG_ERROR( " too many GPU nodes");
        return ComputeStatus::eError;
    }

    HRESULT hr = S_OK;

    m_dbgSupportRs2RelaxedConversionRules = true;

    SL_LOG_INFO("GPU nodes %u - visible node mask %u", NodeCount, m_visibleNodeMask);
       
    {
        D3D11_SAMPLER_DESC sampDesc =
        {
            D3D11_FILTER_MIN_MAG_MIP_POINT,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            0.0f,
            1,
            D3D11_COMPARISON_NEVER,
            { 0.0f, 0.0f , 0.0f ,0.0f},
            0.0f,
            D3D11_FLOAT32_MAX,
        };
        m_device->CreateSamplerState(&sampDesc, &m_samplers[eSamplerPointClamp]);
    }

    {
        D3D11_SAMPLER_DESC sampDesc =
        {
            D3D11_FILTER_MIN_MAG_MIP_POINT,
            D3D11_TEXTURE_ADDRESS_MIRROR,
            D3D11_TEXTURE_ADDRESS_MIRROR,
            D3D11_TEXTURE_ADDRESS_MIRROR,
            0.0f,
            1,
            D3D11_COMPARISON_NEVER,
            { 0.0f, 0.0f , 0.0f ,0.0f},
            0.0f,
            D3D11_FLOAT32_MAX,
        };
        m_device->CreateSamplerState(&sampDesc, &m_samplers[eSamplerPointMirror]);
    }

    {
        D3D11_SAMPLER_DESC sampDesc =
        {
            D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            0.0f,
            1,
            D3D11_COMPARISON_NEVER,
            { 0.0f, 0.0f , 0.0f ,0.0f},
            0.0f,
            D3D11_FLOAT32_MAX,
        };
        m_device->CreateSamplerState(&sampDesc, &m_samplers[eSamplerLinearClamp]);
    }

    {
        D3D11_SAMPLER_DESC sampDesc =
        {
            D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            D3D11_TEXTURE_ADDRESS_MIRROR,
            D3D11_TEXTURE_ADDRESS_MIRROR,
            D3D11_TEXTURE_ADDRESS_MIRROR,
            0.0f,
            1,
            D3D11_COMPARISON_NEVER,
            { 0.0f, 0.0f , 0.0f ,0.0f},
            0.0f,
            D3D11_FLOAT32_MAX,
        };
        m_device->CreateSamplerState(&sampDesc, &m_samplers[eSamplerLinearMirror]);
    }

    {
        D3D11_SAMPLER_DESC sampDesc =
        {
            D3D11_FILTER_ANISOTROPIC,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            D3D11_TEXTURE_ADDRESS_CLAMP,
            0.0f,
            1,
            D3D11_COMPARISON_NEVER,
            { 0.0f, 0.0f , 0.0f ,0.0f},
            0.0f,
            D3D11_FLOAT32_MAX,
        };
        m_device->CreateSamplerState(&sampDesc, &m_samplers[eSamplerAnisoClamp]);
    }

    createKernel(copy_cs, copy_cs_len, "copy.cs", "main", m_copyKernel);

    genericPostInit();

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::shutdown()
{
    CHI_CHECK(destroyKernel(m_copyKernel));
    m_copyKernel = {};

    m_context = {};
    SL_SAFE_RELEASE(m_immediateContext);
    SL_SAFE_RELEASE(m_device5);

    for (auto i = 0; i < eSamplerCount; i++)
    {
        SL_SAFE_RELEASE(m_samplers[i]);
    }

    for (UINT node = 0; node < MAX_NUM_NODES; node++)
    {
        for (auto section : m_sectionPerfMap[node])
        {
            SL_SAFE_RELEASE(section.second.queryBegin);
            SL_SAFE_RELEASE(section.second.queryEnd);
            SL_SAFE_RELEASE(section.second.queryDisjoint);
        }
        m_sectionPerfMap[node].clear();
    }

    clearCache();

    for (auto& k : m_kernels)
    {
        auto kernel = (KernelDataD3D11*)k.second;
        SL_LOG_VERBOSE("Destroying kernel %s", kernel->name.c_str());
        kernel->destroy();
        delete kernel;
    }
    m_kernels.clear();

    return Generic::shutdown();
}

ComputeStatus D3D11::clearCache()
{
    for (auto& resources : m_resourceData)
    {
        for (auto& data : resources.second)
        {
            if (data.second.UAV)
            {
                auto refCount = data.second.UAV->Release();
                SL_LOG_VERBOSE("Clearing cached UAV 0x%llx for resource 0x%llx - ref count %u", data.second.UAV, resources.first, refCount);
            }
            if (data.second.SRV)
            {
                auto refCount = data.second.SRV->Release();
                SL_LOG_VERBOSE("Clearing cached SRV 0x%llx for resource 0x%llx - ref count %u", data.second.SRV, resources.first, refCount);
            }
        }
        resources.second.clear();
    }
    m_resourceData.clear();

    if (m_context)
    {
        m_context->ClearState();
    }

    return Generic::clearCache();
}

ComputeStatus D3D11::getRenderAPI(RenderAPI &OutType)
{
    OutType = RenderAPI::eD3D11;
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::createKernel(void *blobData, unsigned int blobSize, const char* fileName, const char *entryPoint, Kernel &kernel)
{
    if (!blobData || !fileName || !entryPoint)
    {
        return ComputeStatus::eInvalidArgument;
    }

    size_t hash = 0;
    const char* p = fileName;
    while (*p)
    {
        hash_combine(hash, *p++);
    }
    p = entryPoint;
    while (*p)
    {
        hash_combine(hash, *p++);
    }
    auto i = blobSize;
    while (i--)
    {
        hash_combine(hash, ((char*)blobData)[i]);
    }

    ComputeStatus Res = ComputeStatus::eOk;
    KernelDataD3D11 *data = {};
    bool missing = false;
    {
        std::scoped_lock lock(m_mutexKernel);
        auto it = m_kernels.find(hash);
        missing = it == m_kernels.end();
        if (missing)
        {
            data = new KernelDataD3D11;
            data->hash = hash;
            m_kernels[hash] = data;
        }
        else
        {
            data = (KernelDataD3D11*)(*it).second;
        }
    }
    if (missing)
    {
        data->name = fileName;
        data->entryPoint = entryPoint;
        const char* blob = (const char*)blobData;
        if (blob[0] == 'D' && blob[1] == 'X' && blob[2] == 'B' && blob[3] == 'C')
        {
            data->kernelBlob.resize(blobSize);
            memcpy(data->kernelBlob.data(), blob, blobSize);
            if (FAILED(m_device->CreateComputeShader(data->kernelBlob.data(), data->kernelBlob.size(), nullptr, &data->shader)))
            {
                SL_LOG_ERROR( "Failed to create shader %s:%s", fileName, entryPoint);
                return ComputeStatus::eError;
            }
            SL_LOG_VERBOSE("Creating DXBC kernel %s:%s hash %llu", fileName, entryPoint, hash);
        }
        else
        {
            SL_LOG_ERROR( "Unsupported kernel blob");
            return ComputeStatus::eInvalidArgument;
        }
    }
    else
    {
        if (data->entryPoint != entryPoint || data->name != fileName)
        {
            SL_LOG_ERROR( "Shader %s:%s has overlapping hash with shader %s:%s", data->name.c_str(), data->entryPoint.c_str(), fileName, entryPoint);
            return ComputeStatus::eError;
        }
        SL_LOG_WARN("Kernel %s:%s with hash 0x%llx already created!", fileName, entryPoint, hash);
    }
    kernel = hash;
    return Res;
}

ComputeStatus D3D11::destroyKernel(Kernel& InKernel)
{
    if (!InKernel) return ComputeStatus::eOk; // fine to destroy null kernels
    std::scoped_lock lock(m_mutexKernel);
    auto it = m_kernels.find(InKernel);
    if (it == m_kernels.end())
    {
        return ComputeStatus::eInvalidCall;
    }
    KernelDataD3D11* kernel = (KernelDataD3D11*)(it->second);
    SL_LOG_VERBOSE("Destroying kernel %s", kernel->name.c_str());
    kernel->destroy();
    delete it->second;
    m_kernels.erase(it);
    InKernel = {};
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::pushState(CommandList cmdList)
{
    if (!cmdList) return ComputeStatus::eOk;

    auto& threadD3D11 = *(chi::D3D11ThreadContext*)m_getThreadContext();
    auto context = (ID3D11DeviceContext*)cmdList;

    context->CSGetShader(&threadD3D11.engineCS, 0, 0);
    context->CSGetSamplers(0, chi::kMaxD3D11Items, threadD3D11.engineSamplers);
    context->OMGetRenderTargets(chi::kMaxD3D11Items, threadD3D11.engineRTVs, &threadD3D11.engineDSV);
    context->CSGetShaderResources(0, chi::kMaxD3D11Items, &threadD3D11.engineSRVs[0]);
    context->CSGetUnorderedAccessViews(0, chi::kMaxD3D11Items, &threadD3D11.engineUAVs[0]);
    context->CSGetConstantBuffers(0, chi::kMaxD3D11Items, threadD3D11.engineConstBuffers);

    // Must do this to ensure RTV/SRV/UAV is not bound as previous input/output otherwise 
    // our CS passes which rely on the resources from the engine might get null input
    static ID3D11RenderTargetView* nullRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    static ID3D11UnorderedAccessView* nullUAVs[chi::kMaxD3D11Items] = {};
    static ID3D11ShaderResourceView* nullSRVs[chi::kMaxD3D11Items] = {};
    context->OMSetRenderTargets(chi::kMaxD3D11Items, nullRTVs, nullptr);
    context->CSSetUnorderedAccessViews(0, chi::kMaxD3D11Items, nullUAVs, nullptr);
    context->CSSetShaderResources(0, chi::kMaxD3D11Items, nullSRVs);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::popState(CommandList cmdList)
{
    if (!cmdList) return ComputeStatus::eOk;

    auto& threadD3D11 = *(chi::D3D11ThreadContext*)m_getThreadContext();
    auto context = (ID3D11DeviceContext*)cmdList;

    context->CSSetShader(threadD3D11.engineCS, 0, 0);
    context->CSSetSamplers(0, chi::kMaxD3D11Items, threadD3D11.engineSamplers);
    context->CSSetUnorderedAccessViews(0, chi::kMaxD3D11Items, threadD3D11.engineUAVs, nullptr);
    context->OMSetRenderTargets(chi::kMaxD3D11Items, threadD3D11.engineRTVs, threadD3D11.engineDSV);
    context->CSSetShaderResources(0, chi::kMaxD3D11Items, threadD3D11.engineSRVs);
    context->CSSetConstantBuffers(0, chi::kMaxD3D11Items, threadD3D11.engineConstBuffers);

    SL_SAFE_RELEASE(threadD3D11.engineCS);
    SL_SAFE_RELEASE(threadD3D11.engineDSV);

    int n = chi::kMaxD3D11Items;
    while (n--)
    {
        SL_SAFE_RELEASE(threadD3D11.engineSamplers[n]);
        SL_SAFE_RELEASE(threadD3D11.engineConstBuffers[n]);
        SL_SAFE_RELEASE(threadD3D11.engineUAVs[n]);
        SL_SAFE_RELEASE(threadD3D11.engineSRVs[n]);
        SL_SAFE_RELEASE(threadD3D11.engineRTVs[n]);
    }

    threadD3D11 = {};

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::createCommandListContext(CommandQueue queue, uint32_t count, ICommandListContext*& ctx, const char friendlyName[])
{
    auto ctx1 = new D3D11CommandListContext();
    ctx1->init(friendlyName, m_device, this);
    ctx = ctx1;
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::destroyCommandListContext(ICommandListContext* ctx)
{
    if (ctx)
    {
        ((D3D11CommandListContext*)ctx)->shutdown();
        delete ctx;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::createCommandQueue(CommandQueueType type, CommandQueue& queue, const char friendlyName[], uint32_t index)
{    
    queue = m_immediateContext;
    m_immediateContext->AddRef();
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::destroyCommandQueue(CommandQueue& queue)
{
    if (queue)
    {
        auto tmp = (IUnknown*)queue;
        SL_SAFE_RELEASE(tmp);
    }    
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::createFence(FenceFlags flags, uint64_t initialValue, Fence& outFence, const char friendlyName[])
{
    ID3D11Fence* fence{};
    D3D11_FENCE_FLAG d3d11Flags = D3D11_FENCE_FLAG_NONE;
    if (flags & eFenceFlagsShared)
    {
        d3d11Flags |= D3D11_FENCE_FLAG_SHARED;
    }
    if (FAILED(m_device5->CreateFence(initialValue, d3d11Flags, IID_PPV_ARGS(&fence))))
    {
        SL_LOG_ERROR( "Failed to create ID3D11Fence");
    }
    else
    {
        outFence = fence;
        sl::Resource r(ResourceType::eFence, fence);
        setDebugName(&r, friendlyName);
    }
    return fence ? ComputeStatus::eOk : ComputeStatus::eError;
}

ComputeStatus D3D11::setFullscreenState(SwapChain chain, bool fullscreen, Output out)
{
    if (!chain) return ComputeStatus::eInvalidArgument;
    IDXGISwapChain* swapChain = (IDXGISwapChain*)chain;
    if (FAILED(swapChain->SetFullscreenState(fullscreen, (IDXGIOutput*)out)))
    {
        SL_LOG_ERROR( "Failed to set fullscreen state");
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::getRefreshRate(SwapChain chain, float& refreshRate)
{
    if (!chain) return ComputeStatus::eInvalidArgument;
    IDXGISwapChain* swapChain = (IDXGISwapChain*)chain;
    IDXGIOutput* dxgiOutput;
    HRESULT hr = swapChain->GetContainingOutput(&dxgiOutput);
    // if swap chain get failed to get DXGIoutput then follow the below link get the details from remarks section
    //https://docs.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getcontainingoutput
    if (SUCCEEDED(hr))
    {
        // get the descriptor for current output
        // from which associated mornitor will be fetched
        DXGI_OUTPUT_DESC outputDes{};
        hr = dxgiOutput->GetDesc(&outputDes);
        dxgiOutput->Release();
        if (SUCCEEDED(hr))
        {
            MONITORINFOEXW info;
            info.cbSize = sizeof(info);
            // get the associated monitor info
            if (GetMonitorInfoW(outputDes.Monitor, &info) != 0)
            {
                // using the CCD get the associated path and display configuration
                UINT32 requiredPaths, requiredModes;
                if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS)
                {
                    std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
                    std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
                    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS)
                    {
                        // iterate through all the paths until find the exact source to match
                        for (auto& p : paths) {
                            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
                            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                            sourceName.header.size = sizeof(sourceName);
                            sourceName.header.adapterId = p.sourceInfo.adapterId;
                            sourceName.header.id = p.sourceInfo.id;
                            if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS)
                            {
                                // find the matched device which is associated with current device 
                                // there may be the possibility that display may be duplicated and windows may be one of them in such scenario
                                // there may be two callback because source is same target will be different
                                // as window is on both the display so either selecting either one is ok
                                if (wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
                                    // get the refresh rate
                                    UINT numerator = p.targetInfo.refreshRate.Numerator;
                                    UINT denominator = p.targetInfo.refreshRate.Denominator;
                                    double refrate = (double)numerator / (double)denominator;
                                    refreshRate = (float)refrate;
                                    return ComputeStatus::eOk;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    SL_LOG_ERROR( "Failed to retreive refresh rate from swapchain 0x%llx", chain);
    return ComputeStatus::eError;
}

ComputeStatus D3D11::getSwapChainBuffer(SwapChain chain, uint32_t index, Resource& buffer)
{
    ID3D11Resource* tmp;
    if (FAILED(((IDXGISwapChain*)chain)->GetBuffer(index, IID_PPV_ARGS(&tmp))))
    {
        SL_LOG_ERROR( "Failed to get buffer from swapchain");
        return ComputeStatus::eError;
    }
    buffer = new sl::Resource(ResourceType::eTex2d, tmp);
    // We free these buffers but never allocate them so account for the VRAM
    manageVRAM(buffer, VRAMOperation::eAlloc);
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::bindSharedState(CommandList cmdList, UINT node)
{
    if (!cmdList) return ComputeStatus::eInvalidArgument;

    m_context = (ID3D11DeviceContext*)cmdList;
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::bindKernel(const Kernel kernelToBind)
{
    if (!m_context) return ComputeStatus::eInvalidArgument;

    auto& ctx = m_dispatchContext.getContext();
    
    {
        std::scoped_lock lock(m_mutexKernel);
        auto it = m_kernels.find(kernelToBind);
        if (it == m_kernels.end())
        {
            SL_LOG_ERROR( "Trying to bind kernel which has not been created");
            return ComputeStatus::eInvalidCall;
        }
        ctx.kernel = (KernelDataD3D11*)(*it).second;
    }

    m_context->CSSetShader(ctx.kernel->shader, nullptr, 0);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::bindSampler(uint32_t pos, uint32_t base, Sampler sampler)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!m_context || !ctx.kernel || base >= 8) return ComputeStatus::eInvalidArgument;

    m_context->CSSetSamplers(base, 1, &m_samplers[sampler]);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::bindConsts(uint32_t pos, uint32_t base, void *data, size_t dataSize, uint32_t instances)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!m_context || !ctx.kernel) return ComputeStatus::eInvalidArgument;

    auto it = ctx.kernel->constBuffers.find(base);
    if (it == ctx.kernel->constBuffers.end())
    {
        Resource buffer;
        chi::ResourceDescription desc = {};
        desc.width = extra::align((uint32_t)dataSize, 16);
        desc.height = 1;
        desc.heapType = eHeapTypeUpload;
        desc.state = ResourceState::eConstantBuffer;
        createBuffer(desc, buffer, "sl.d3d11.const_buffer");
        ctx.kernel->constBuffers[base] = (ID3D11Buffer*)(buffer->native);
    }
    auto buffer = ctx.kernel->constBuffers[base];
    D3D11_MAPPED_SUBRESOURCE bufferData = {};
    m_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &bufferData);
    if (!bufferData.pData)
    {
        SL_LOG_ERROR( "Failed to map constant buffer");
        return ComputeStatus::eError;
    }
    memcpy(bufferData.pData, data, dataSize);
    m_context->Unmap(buffer, 0);

    m_context->CSSetConstantBuffers(base, 1, &buffer);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::bindTexture(uint32_t pos, uint32_t base, Resource resource, uint32_t mipOffset, uint32_t mipLevels)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!m_context || !ctx.kernel) return ComputeStatus::eInvalidArgument;

    // Allow null resource
    auto res = ComputeStatus::eOk;
    ResourceDriverDataD3D11 data{};
    if (resource)
    {
        res = getTextureDriverData(resource, data, mipOffset, mipLevels);
    }

    m_context->CSSetShaderResources(base, 1, &data.SRV);

    return res;
}

ComputeStatus D3D11::bindRWTexture(uint32_t pos, uint32_t base, Resource resource, uint32_t mipOffset)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!m_context || !ctx.kernel) return ComputeStatus::eInvalidArgument;

    // Allow null resource
    auto res = ComputeStatus::eOk;
    ResourceDriverDataD3D11 data{};
    if (resource)
    {
        res = getSurfaceDriverData(resource, data, mipOffset);
    }

    m_context->CSSetUnorderedAccessViews(base, 1, &data.UAV, nullptr);

    return res;
}

ComputeStatus D3D11::bindRawBuffer(uint32_t pos, uint32_t base, Resource resource)
{
    // This is still just a UAV for D3D11 so reuse the other method
    // Note that UAV creation checks for buffers and modifies view accordingly (D3D12_BUFFER_UAV_FLAG_RAW etc.)
    return bindRWTexture(pos, base, resource);
}

ComputeStatus D3D11::dispatch(unsigned int blocksX, unsigned int blocksY, unsigned int blocksZ)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!m_context || !ctx.kernel) return ComputeStatus::eInvalidArgument;
     
    m_context->Dispatch(blocksX, blocksY, blocksZ);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::getTextureDriverData(Resource res, ResourceDriverDataD3D11&data, uint32_t mipOffset, uint32_t mipLevels, Sampler sampler)
{
    if (!res || !res->native) return ComputeStatus::eInvalidArgument;

    std::scoped_lock lock(m_mutexResource);

    ID3D11Resource* resource = (ID3D11Resource*)(res->native);

    uint64_t hash = (((mipOffset << 16) | mipLevels) << 32 | kResourceHashTypeSrv);

    auto it = m_resourceData.find(resource);
    if (it == m_resourceData.end() || (*it).second.find(hash) == (*it).second.end())
    {
        
        ResourceDescription Desc;
        getResourceDescription(res, Desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
        SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Format = getCorrectFormat((DXGI_FORMAT)Desc.nativeFormat);
        SRVDesc.Texture2D.MipLevels = Desc.mips;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        auto status = m_device->CreateShaderResourceView(resource, &SRVDesc, &data.SRV);
        if (FAILED(status))
        { 
            SL_LOG_ERROR( "CreateShaderResourceView failed - status %d", status);
            return ComputeStatus::eError;
        }
        constexpr char SRVFriendlyName[] = "sl.compute.textureCachedSRV";
        data.SRV->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(SRVFriendlyName), SRVFriendlyName); // Narrow character type debug object name

        SL_LOG_VERBOSE("Cached SRV resource 0x%llx node %u fmt %s size (%u,%u)", res, 0, getDXGIFormatStr(Desc.nativeFormat), (UINT)Desc.width, (UINT)Desc.height);

        m_resourceData[resource][hash] = data;
    }
    else
    {
        data = (*it).second[hash];
    }

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::getSurfaceDriverData(Resource res, ResourceDriverDataD3D11&data, uint32_t mipOffset)
{
    if (!res || !res->native) return ComputeStatus::eInvalidArgument;

    std::scoped_lock lock(m_mutexResource);

    ID3D11Resource* resource = (ID3D11Resource*)(res->native);

    uint32_t hash = ((mipOffset << 48) | kResourceHashTypeUav);

    auto it = m_resourceData.find(resource);
    if (it == m_resourceData.end() || (*it).second.find(hash) == (*it).second.end())
    {
        auto node = 0; // FIX THIS
        ResourceDescription Desc;
        getResourceDescription(res, Desc);

        D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc;
        if (Desc.flags & (ResourceFlags::eRawOrStructuredBuffer | ResourceFlags::eConstantBuffer))
        {
            UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            UAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            UAVDesc.Buffer.FirstElement = 0;
            UAVDesc.Buffer.NumElements = Desc.width / sizeof(uint32_t);
            UAVDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        }
        else
        {
            if (!isSupportedFormat(getCorrectFormat((DXGI_FORMAT)Desc.nativeFormat), 0, D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            {
                return ComputeStatus::eError;
            }

            UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            UAVDesc.Texture2D.MipSlice = mipOffset;
            UAVDesc.Format = getCorrectFormat((DXGI_FORMAT)Desc.nativeFormat);
        }

        auto status = m_device->CreateUnorderedAccessView(resource, &UAVDesc, &data.UAV);
        if (FAILED(status))
        { 
            SL_LOG_ERROR( "CreateShaderResourceView failed - status %d", status);
            return ComputeStatus::eError;
        }

        constexpr char UAVFriendlyName[] = "sl.compute.surfaceCachedUAV";
        data.UAV->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(UAVFriendlyName), UAVFriendlyName); // Narrow character type debug object name

        SL_LOG_VERBOSE("Cached UAV resource 0x%llx node %u fmt %s size (%u,%u)", res, 0, getDXGIFormatStr(Desc.nativeFormat), (UINT)Desc.width, (UINT)Desc.height);

        m_resourceData[resource][hash] = data;
    }
    else
    {
        data = (*it).second[hash];
    }

    return ComputeStatus::eOk;
}

bool D3D11::isSupportedFormat(DXGI_FORMAT format, int flag1, int flag2)
{
    HRESULT hr = {};
    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT FormatSupport = { format, 0 };
        hr = m_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &FormatSupport, sizeof(FormatSupport));
        if (SUCCEEDED(hr) && (FormatSupport.OutFormatSupport & flag1) != 0)
        {
            return true;
        }
    }
    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 FormatSupport = { format, 0 };
        hr = m_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &FormatSupport, sizeof(FormatSupport));
        if (SUCCEEDED(hr) && (FormatSupport.OutFormatSupport2 & flag2) != 0)
        {
            return true;
        }
    }
    SL_LOG_ERROR( "Format %s is unsupported - hres %lu flags %d %d", getDXGIFormatStr(format), hr, flag1, flag2);
    return false;
}

ComputeStatus D3D11::createTexture2DResourceSharedImpl(ResourceDescription &InOutResourceDesc, Resource &OutResource, bool UseNativeFormat, ResourceState InitialState, const char InFriendlyName[])
{
    ID3D11Texture2D* pResource = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = InOutResourceDesc.width;
    desc.Height = InOutResourceDesc.height;
    desc.MipLevels = InOutResourceDesc.mips;
    desc.ArraySize = 1;
    if (UseNativeFormat)
    {
        desc.Format = (DXGI_FORMAT)InOutResourceDesc.nativeFormat;
    }
    else
    {
        NativeFormat native;
        getNativeFormat(InOutResourceDesc.format, native);
        desc.Format = getCorrectFormat((DXGI_FORMAT)native);
    }
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    if (InOutResourceDesc.flags & ResourceFlags::eSharedResource)
    {
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        // Special case, some depth buffers cannot be shared as NT handle so change format
        if (InOutResourceDesc.format == eFormatD24S8 || InOutResourceDesc.format == eFormatD32S32)
        {
            desc.Format = DXGI_FORMAT_R32_FLOAT;
        }
    }

    switch (InOutResourceDesc.heapType)
    {
        default: assert(0); // Fall through
        case eHeapTypeDefault:
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            desc.BindFlags = 0;
            break;
        case eHeapTypeUpload:
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = 0;
            break;
        case eHeapTypeReadback:
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.BindFlags = 0;
            break;
    }

    UINT formatSupport{};
    m_device->CheckFormatSupport(desc.Format, &formatSupport);
    if (formatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
    {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        InOutResourceDesc.flags |= ResourceFlags::eColorAttachment;
    }
    else
    {
        InOutResourceDesc.flags &= ~ResourceFlags::eColorAttachment;
    }
    if (formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
    {
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        InOutResourceDesc.flags |= ResourceFlags::eShaderResourceStorage;
    }
    else
    {
        InOutResourceDesc.flags &= ~ResourceFlags::eShaderResourceStorage;
    }
    if (formatSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)
    {
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    else
    {
        InOutResourceDesc.flags &= ~ResourceFlags::eShaderResource;
    }

    if (m_allocateCallback)
    {
        ResourceAllocationDesc rd = { ResourceType::eTex2d, &desc, 0, nullptr };
        auto result = m_allocateCallback(&rd, m_device);
        pResource = (ID3D11Texture2D*)result.native;
    }
    else
    {
        m_device->CreateTexture2D(&desc, NULL, &pResource);
    }
    
    OutResource = new sl::Resource(ResourceType::eTex2d, pResource);
    if (!pResource)
    {
        SL_LOG_ERROR( "Failed to create Tex2d");
        return ComputeStatus::eError;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::createBufferResourceImpl(ResourceDescription &InOutResourceDesc, Resource &OutResource, ResourceState InitialState, const char InFriendlyName[])
{
    ID3D11Buffer* pResource = nullptr;

    D3D11_BUFFER_DESC bufdesc;
    bufdesc.ByteWidth = InOutResourceDesc.width;
    assert(InOutResourceDesc.height == 1);
    bufdesc.StructureByteStride = 0;

    switch (InOutResourceDesc.heapType)
    {
        default: assert(0); // Fall through
        case eHeapTypeDefault:
            bufdesc.Usage = D3D11_USAGE_DEFAULT;
            bufdesc.CPUAccessFlags = 0;
            bufdesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            bufdesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            break;
        case eHeapTypeUpload:
            bufdesc.MiscFlags = 0;
            if (InOutResourceDesc.state == ResourceState::eConstantBuffer)
            {
                bufdesc.Usage = D3D11_USAGE_DYNAMIC;
                bufdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                bufdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            }
            else
            {
                bufdesc.Usage = D3D11_USAGE_STAGING;
                bufdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
                bufdesc.BindFlags = 0;
            }
            break;
        case eHeapTypeReadback:
            bufdesc.Usage = D3D11_USAGE_STAGING;
            bufdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bufdesc.MiscFlags = 0;
            bufdesc.BindFlags = 0;
            break;
    }

    if (m_allocateCallback)
    {
        ResourceAllocationDesc desc = { ResourceType::eBuffer, &bufdesc, 0, nullptr };
        auto result = m_allocateCallback(&desc, m_device);
        pResource = (ID3D11Buffer*)result.native;
    }
    else
    {
        m_device->CreateBuffer(&bufdesc, NULL, &pResource);
    }

    OutResource = new sl::Resource(ResourceType::eBuffer, pResource);
    if (!pResource)
    {
        SL_LOG_ERROR( "Failed to create buffer");
        return ComputeStatus::eError;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::setDebugName(Resource res, const char name[])
{
#ifdef SL_DEBUG
    auto unknown = (IUnknown*)(res->native);
    ID3D11DeviceChild* deviceChild{};
    unknown->QueryInterface(&deviceChild);
    if (deviceChild)
    {
        deviceChild->Release();
        deviceChild->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
    }
#endif
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::copyHostToDeviceBuffer(CommandList InCmdList, uint64_t InSize, const void *InData, Resource InUploadResource, Resource InTargetResource, unsigned long long InUploadOffset, unsigned long long InDstOffset)
{
    UINT8* StagingPtr = nullptr;

    ID3D11Resource* Resource = (ID3D11Resource*)(InTargetResource->native);
    ID3D11Resource* Scratch = (ID3D11Resource*)(InUploadResource->native);

    auto context = ((ID3D11DeviceContext*)InCmdList);
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        // Deferred Dx11 contexts seem to require dynamic resource for the Map() to work. Changing
        // the resources to dynamic is an intrusive change that would affect all Dx11 apps - let's
        // grab the immediate Dx11 context instead.
        context = m_immediateContext;
    }

    D3D11_MAPPED_SUBRESOURCE sub;
    HRESULT hr = context->Map(Scratch, 0, D3D11_MAP_WRITE, 0, &sub);
    if (hr != S_OK)
    {
        SL_LOG_ERROR( "Failed to map buffer - error %lu", hr);
        return ComputeStatus::eError;
    }

    char* target = (char*)sub.pData + InUploadOffset;
    memcpy(target, InData, InSize);
    context->Unmap(Scratch, 0);
    D3D11_BOX Box = { (UINT)InUploadOffset, 0, 0, (UINT)InUploadOffset + (UINT)InSize, 1, 1 };
    context->CopySubresourceRegion(Resource, 0, (UINT)InDstOffset, 0, 0, Scratch, 0, &Box);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::copyHostToDeviceTexture(CommandList InCmdList, uint64_t InSize, uint64_t RowPitch, const void* InData, Resource InTargetResource, Resource& InUploadResource)
{
    ((ID3D11DeviceContext*)InCmdList)->UpdateSubresource((ID3D11Resource*)(InTargetResource->native), 0, nullptr, InData, UINT(RowPitch), UINT(InSize));
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::clearView(CommandList InCmdList, Resource InResource, const float4 Color, const RECT * pRects, unsigned int NumRects, CLEAR_TYPE &outType)
{
    outType = CLEAR_UNDEFINED;
    
    ID3D11DeviceContext1* context1;
    ((ID3D11DeviceContext*)InCmdList)->QueryInterface(IID_PPV_ARGS(&context1));
    if (context1)
    {
        ResourceDriverDataD3D11 data;
        ComputeStatus status = getSurfaceDriverData(InResource, data);
        if (status == ComputeStatus::eOk)
        {
            if (!data.bZBCSupported)
            {
                return ComputeStatus::eNotSupported;
            }
            // dx11 driver may skip the clear (ClearSkip perfstrat) if it decides that this clear is redundant. I didn't yet figure out why
            // it decides that, but calling DiscardView() prior to ClearView() disables this behaviour and works around the bug 200666776
            context1->DiscardView((ID3D11UnorderedAccessView*)data.UAV);
            context1->ClearView((ID3D11UnorderedAccessView*)data.UAV, (const FLOAT*)&Color, (const RECT*)pRects, NumRects);
        }
        return status;
    }
    return ComputeStatus::eError;
}

ComputeStatus D3D11::insertGPUBarrierList(CommandList InCmdList, const Resource* InResources, unsigned int InResourceCount, BarrierType InBarrierType)
{
    // Nothing to do here in d3d11
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::insertGPUBarrier(CommandList InCmdList, Resource InResource, BarrierType InBarrierType)
{
    // Nothing to do here in d3d11
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::transitionResourceImpl(CommandList cmdList, const ResourceTransition *transitions, uint32_t count)
{
    if (!cmdList || !transitions)
    {
        return ComputeStatus::eInvalidArgument;
    }
    // Nothing to do here in d3d11
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::copyResource(CommandList cmdList, Resource dstResource, Resource srcResource)
{
    if (!cmdList || !dstResource || !srcResource) return ComputeStatus::eInvalidArgument;
    auto context = (ID3D11DeviceContext*)cmdList;
    context->CopyResource((ID3D11Resource*)(dstResource->native), (ID3D11Resource*)(srcResource->native));
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::cloneResource(Resource resource, Resource &clone, const char friendlyName[], ResourceState initialState, unsigned int creationMask, unsigned int visibilityMask)
{
    if (!resource) return ComputeStatus::eInvalidArgument;

    ID3D11Resource* res = nullptr;
    HRESULT hr = S_OK;
    ResourceDescription desc;
    if (getResourceDescription(resource, desc) != ComputeStatus::eOk)
    {
        return ComputeStatus::eError;
    }
    auto type = desc.flags & (ResourceFlags::eRawOrStructuredBuffer | ResourceFlags::eConstantBuffer) ? ResourceType::eBuffer : ResourceType::eTex2d;
    if (type == ResourceType::eBuffer)
    {
        auto buffer = (ID3D11Buffer*)resource->native;
        D3D11_BUFFER_DESC desc1;
        buffer->GetDesc(&desc1);
        if (m_allocateCallback)
        {
            ResourceAllocationDesc desc = { ResourceType::eBuffer, &desc1, (uint32_t)initialState, nullptr };
            auto result = m_allocateCallback(&desc, m_device);
            res = (ID3D11Resource*)result.native;
        }
        else
        {
            hr = m_device->CreateBuffer(&desc1, nullptr, &buffer);
            res = buffer;
        }
    }
    else
    {
        auto tex2d = (ID3D11Texture2D*)resource->native;
        D3D11_TEXTURE2D_DESC desc1;
        tex2d->GetDesc(&desc1);

        UINT formatSupport{};
        m_device->CheckFormatSupport(desc1.Format, &formatSupport);
        if (formatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
        {
            desc1.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        if (formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
        {
            desc1.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }

        if (m_allocateCallback)
        {
            ResourceAllocationDesc desc = { ResourceType::eTex2d, &desc1, (uint32_t)initialState, nullptr };
            auto result = m_allocateCallback(&desc, m_device);
            res = (ID3D11Resource*)result.native;
        }
        else
        {
            hr = m_device->CreateTexture2D(&desc1, nullptr, &tex2d);
            res = tex2d;
        }
    }
    
    if (hr != S_OK || !res)
    {
        SL_LOG_ERROR( "Unable to clone resource");
        return ComputeStatus::eError;
    }

    clone = new sl::Resource(type, res);

    // D3D12/Vulkan sets native flags which doesn't map to D3D11 which has multiple flag fields. 
    // d3d11 ignores this field
    clone->flags = 0; 
    clone->mipLevels = desc.mips;
    clone->arrayLayers = desc.depth;
    clone->nativeFormat = desc.nativeFormat;
    clone->width = desc.width;
    clone->height = desc.height;

    manageVRAM(clone, VRAMOperation::eAlloc);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::copyBufferToReadbackBuffer(CommandList InCmdList, Resource InResource, Resource OutResource, unsigned int InBytesToCopy) 
{
    ID3D11Resource *InD3dResource = (ID3D11Resource*)(InResource->native);
    ID3D11Resource *OutD3dResource = (ID3D11Resource*)(OutResource->native);
    
    ID3D11DeviceContext* DeviceContext = reinterpret_cast<ID3D11DeviceContext*>(InCmdList);
    ID3D11Resource* ReadbackResource = reinterpret_cast<ID3D11Resource*>(OutResource->native);

    D3D11_BOX SrcBox = { 0, 0, 0, InBytesToCopy, 1, 1 };
    DeviceContext->CopySubresourceRegion(ReadbackResource, 0, 0, 0, 0, (ID3D11Resource*)InResource, 0, &SrcBox);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::mapResource(CommandList cmdList, Resource resource, void*& data, uint32_t subResource, uint64_t offset, uint64_t totalBytes)
{
    auto src = (ID3D11Resource*)(resource->native);
    if (!src) return ComputeStatus::eInvalidPointer;

    ID3D11DeviceContext* dc = reinterpret_cast<ID3D11DeviceContext*>(cmdList);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(dc->Map(src, subResource, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        SL_LOG_ERROR( "Failed to map buffer");
        return ComputeStatus::eError;
    }
    data = ((uint8_t*)mapped.pData) + offset;
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::unmapResource(CommandList cmdList, Resource resource, uint32_t subResource)
{
    auto src = (ID3D11Resource*)(resource->native);
    if (!src) return ComputeStatus::eInvalidPointer;

    ID3D11DeviceContext* dc = reinterpret_cast<ID3D11DeviceContext*>(cmdList);
    dc->Unmap(src,subResource);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::getResourceDescription(Resource resource, ResourceDescription &outDesc)
{
    if (!resource || !resource->native) return ComputeStatus::eInvalidArgument;

    outDesc = {};

    if (resource->type == ResourceType::eFence)
    {
        // Fences are always shared with d3d12 so report back
        outDesc.flags |= ResourceFlags::eSharedResource;
        return ComputeStatus::eOk;
    }

    // First make sure this is not an DXGI or some other resource
    Microsoft::WRL::ComPtr<IUnknown> unknown = (IUnknown*)(resource->native);
    Microsoft::WRL::ComPtr<ID3D11Resource> d3d11Resource;

    unknown.As(&d3d11Resource);
    if (!d3d11Resource)
    {
        return ComputeStatus::eError;
    }

    D3D11_RESOURCE_DIMENSION dim;
    d3d11Resource->GetType(&dim);

    outDesc = {};
    outDesc.format = eFormatINVALID;

    ID3D11Texture2D* tex2d = nullptr;
    ID3D11Buffer* buffer = nullptr;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        tex2d = (ID3D11Texture2D*)(resource->native);
        D3D11_TEXTURE2D_DESC desc;
        tex2d->GetDesc(&desc);

        if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
        {
            outDesc.flags |= ResourceFlags::eShaderResourceStorage;
        }
        if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)
        {
            outDesc.flags |= ResourceFlags::eDepthStencilAttachment;
        }
        if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
        {
            outDesc.flags |= ResourceFlags::eColorAttachment;
        }
        outDesc.width = (UINT)desc.Width;
        outDesc.height = desc.Height;
        outDesc.nativeFormat = desc.Format;
        outDesc.mips = desc.MipLevels;
        outDesc.depth = desc.ArraySize;
        outDesc.flags |= ResourceFlags::eShaderResource;
        if (desc.MiscFlags & D3D11_RESOURCE_MISC_FLAG::D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
        {
            outDesc.flags |= ResourceFlags::eSharedResource;
        }
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        buffer = (ID3D11Buffer*)(resource->native);
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);

        if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
        {
            outDesc.flags |= ResourceFlags::eShaderResourceStorage;
        }
        
        outDesc.width = (UINT)desc.ByteWidth;
        outDesc.height = 1;
        outDesc.nativeFormat = DXGI_FORMAT_UNKNOWN;
        outDesc.flags |= ResourceFlags::eRawOrStructuredBuffer | ResourceFlags::eConstantBuffer;
    }
    else
    {
        SL_LOG_ERROR( "Unknown resource");
    }

    getFormat(outDesc.nativeFormat, outDesc.format);

    return ComputeStatus::eOk;
}

ComputeStatus D3D11::getLUIDFromDevice(NVSDK_NGX_LUID *OutId)
{
    return ComputeStatus::eError;
}

ComputeStatus D3D11::beginPerfSection(CommandList cmdList, const char *key, unsigned int node, bool reset)
{
    PerfData* data = {};
    {
        std::scoped_lock lock(m_mutexProfiler);
        auto section = m_sectionPerfMap[node].find(key);
        if (section == m_sectionPerfMap[node].end())
        {
            m_sectionPerfMap[node][key] = {};
            section = m_sectionPerfMap[node].find(key);
        }
        data = &(*section).second;
    }
    
    if (reset)
    {
        data->meter.reset();
    }
    
    if (!data->queryBegin)
    {
        D3D11_QUERY_DESC timestamp_query_desc;
        timestamp_query_desc.Query = D3D11_QUERY_TIMESTAMP;
        timestamp_query_desc.MiscFlags = 0;

        D3D11_QUERY_DESC timestamp_disjoint_query_desc;
        timestamp_disjoint_query_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        timestamp_disjoint_query_desc.MiscFlags = 0;

        m_device->CreateQuery(&timestamp_query_desc, &data->queryBegin);
        m_device->CreateQuery(&timestamp_query_desc, &data->queryEnd);
        m_device->CreateQuery(&timestamp_disjoint_query_desc, &data->queryDisjoint);
    }
    
    auto context = (ID3D11DeviceContext*)cmdList;
    context->Begin(data->queryDisjoint);
    context->End(data->queryBegin);
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::endPerfSection(CommandList cmdList, const char* key, float &avgTimeMS, unsigned int node)
{
    PerfData* data = {};
    {
        std::scoped_lock lock(m_mutexProfiler);
        auto section = m_sectionPerfMap[node].find(key);
        if (section == m_sectionPerfMap[node].end())
        {
            return ComputeStatus::eError;
        }
        data = &(*section).second;
    }
    auto context = (ID3D11DeviceContext*)cmdList;
    context->End(data->queryEnd);
    context->End(data->queryDisjoint);

    UINT64 beginTimeStamp = 0, endTimeStamp = 0;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timestampData = { 0 };

    HRESULT hres = S_OK;

    // Prevent deadlocks 
    {
        int i = 0;
        while (i++ < 100 && (hres = context->GetData(data->queryDisjoint, &timestampData, sizeof(timestampData), 0)) == S_FALSE)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    if (hres == S_OK)
    {
        int i = 0;
        while (i++ < 100 && (hres = context->GetData(data->queryBegin, &beginTimeStamp, sizeof(beginTimeStamp), 0)) == S_FALSE)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    if (hres == S_OK)
    {
        int i = 0;
        while (i++ < 100 && (hres = context->GetData(data->queryEnd, &endTimeStamp, sizeof(endTimeStamp), 0)) == S_FALSE)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    if (hres == S_OK)
    {
        if (!timestampData.Disjoint)
        {
            double delta = (double)((endTimeStamp - beginTimeStamp) / (double)timestampData.Frequency * 1000);
            data->meter.add(delta);
        }
        avgTimeMS = (float)data->meter.getMean();
    }
    else
    {
        SL_LOG_WARN("D3D11 time-stamp timed out");
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::beginProfiling(CommandList cmdList, unsigned int Metadata, const char* marker)
{
#if SL_ENABLE_PROFILING
#endif
    return ComputeStatus::eError;
}

ComputeStatus D3D11::endProfiling(CommandList cmdList)
{
#if SL_ENABLE_PROFILING
#endif
    return ComputeStatus::eError;
}

 bool D3D11::signalCPUFence(Fence fence, uint64_t syncValue)
{
    assert(false);
    SL_LOG_ERROR("Not implemented");
    return false;
}

int D3D11::destroyResourceDeferredImpl(const Resource resource)
{
    auto unknown = (IUnknown*)(resource->native);
    return unknown->Release();
}

DXGI_FORMAT D3D11::getCorrectFormat(DXGI_FORMAT Format)
{
    switch (Format)
    {
        case DXGI_FORMAT_D16_UNORM: // casting from non typeless is supported from RS2+
            assert(m_dbgSupportRs2RelaxedConversionRules);
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D32_FLOAT: // casting from non typeless is supported from RS2+
            assert(m_dbgSupportRs2RelaxedConversionRules); // fall through
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: // casting from non typeless is supported from RS2+
            assert(m_dbgSupportRs2RelaxedConversionRules); // fall through
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: // casting from non typeless is supported from RS2+
            assert(m_dbgSupportRs2RelaxedConversionRules); // fall through
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            return Format;
    };
}
ComputeStatus D3D11::notifyOutOfBandCommandQueue(CommandQueue queue, OutOfBandCommandQueueType type)
{
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::setAsyncFrameMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId)
{
    SL_LOG_WARN_ONCE("D3D11 setAsyncFrameMarker is not implemented!");
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::setLatencyMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId)
{
    SL_LOG_WARN_ONCE("D3D11 setLatencyMarker is not implemented!");
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::createSharedHandle(Resource resource, Handle& outHandle)
{
    if (!resource || !resource->native) return ComputeStatus::eInvalidArgument;

    auto unknown = (IUnknown*)(resource->native);

    IDXGIResource1* res1{};
    unknown->QueryInterface(&res1);
    if (res1)
    {
        res1->Release();
        HANDLE handle;
        if (HRESULT hr;FAILED(hr = res1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle)))
        {
            SL_LOG_ERROR( "Failed to create shared handle %s", std::system_category().message(hr).c_str());
            assert(false);
            return ComputeStatus::eError;
        }
        outHandle = handle;
    }
    else
    {
        ID3D11Fence* fence{};
        unknown->QueryInterface(&fence);
        if (fence)
        {
            fence->Release();
            HANDLE handle;
            if (FAILED(fence->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle)))
            {
                SL_LOG_ERROR( "Failed to create shared handle");
                assert(false);
                return ComputeStatus::eError;
            }
            outHandle = handle;
        }
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::destroySharedHandle(Handle& handle)
{
    if (!CloseHandle(handle))
    {
        SL_LOG_ERROR( "Failed to close shared handle");
        return ComputeStatus::eError;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::getResourceFromSharedHandle(ResourceType type, Handle handle, Resource& resource)
{
    if (type == ResourceType::eTex2d)
    {
        ID3D11Texture2D* tex{};
        if (FAILED(m_device->OpenSharedResource((HANDLE)handle, __uuidof(ID3D11Texture2D), (void**)&tex)))
        {
            SL_LOG_ERROR( "Failed to open shared handle");
            assert(false);
            return ComputeStatus::eError;
        }
        resource = new sl::Resource(ResourceType::eTex2d, tex);
        setDebugName(resource, "sl.shared.from.d3d12");
        // We free these buffers but never allocate them so account for the VRAM
        manageVRAM(resource, VRAMOperation::eAlloc);
    }
    else if (type == ResourceType::eFence)
    {
        ID3D11Fence* fence{};
        if (FAILED(m_device5->OpenSharedFence((HANDLE)handle, __uuidof(ID3D11Fence), (void**)&fence)))
        {
            SL_LOG_ERROR( "Failed to open shared handle");
            assert(false);
            return ComputeStatus::eError;
        }
        resource = new sl::Resource(ResourceType::eFence, fence);
    }
    else
    {
        SL_LOG_ERROR( "Unsupported resource type");
        return ComputeStatus::eError;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D11::prepareTranslatedResources(CommandList cmdList, const std::vector<std::pair<chi::TranslatedResource, chi::ResourceDescription>>& resourceList)
{
    // Running on D3D11 immediate context and using D3D11 resources
    CHI_CHECK(pushState(cmdList));
    CHI_CHECK(bindSharedState(cmdList, 0));
    CHI_CHECK(bindKernel(m_copyKernel));
    for (auto& [resource, desc] : resourceList)
    {
        // If shared directly nothing to do here!
        if (!resource.clone)
        {
            continue;
        }

        // Why use copy kernel? 
        // 
        // Some formats cannot be used in combination with NT shared handle hence
        // direct copy is not always possible due to format difference. For example,
        // any depth/stencil format cannot be shared directly, needs to be cloned as R32F
        // and then we copy R24S8 to R32F using the below code.

        struct CopyCB
        {
            sl::float4 texSize;
        };
        CopyCB cb;
        cb.texSize.x = (float)desc.width;
        cb.texSize.y = (float)desc.height;
        cb.texSize.z = 1.0f / cb.texSize.x;
        cb.texSize.w = 1.0f / cb.texSize.y;
        CHI_CHECK(bindConsts(0, 0, &cb, sizeof(CopyCB), 1)); // unlike vk/d3d12 on d3d11 there is just one buffer, driver takes care of updates
        CHI_CHECK(bindTexture(1, 0, resource.source));
        CHI_CHECK(bindRWTexture(2, 0, resource.clone)); // this is shared as d3d12 resource
        uint32_t grid[] = { ((uint32_t)cb.texSize.x + 16 - 1) / 16, ((uint32_t)cb.texSize.y + 16 - 1) / 16, 1 };
        CHI_CHECK(dispatch(grid[0], grid[1], grid[2]));
    }
    CHI_CHECK(popState(cmdList));
    return ComputeStatus::eOk;
}

void KernelDataD3D11::destroy()
{
    for(auto& cb : constBuffers)
    { 
        SL_SAFE_RELEASE(cb.second);
    }
    constBuffers.clear();
    SL_SAFE_RELEASE(shader);
}
}
}
