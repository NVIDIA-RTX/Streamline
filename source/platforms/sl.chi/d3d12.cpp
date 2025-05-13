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
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <future>
#include <wrl/client.h>

#include "source/core/sl.log/log.h"
#include "source/core/sl.interposer/d3d12/d3d12.h"
#include "source/platforms/sl.chi/d3d12.h"
#include "shaders/copy_to_buffer_cs.h"
#include "nvapi.h"

#include <d3d11_4.h>

#if SL_ENABLE_PROFILING
#pragma comment( lib, "WinPixEventRuntime.lib")
#define USE_PIX
#include "external/pix/Include/WinPixEventRuntime/pix3.h"
#endif

namespace sl
{
namespace chi
{

constexpr uint64_t kMaxSemaphoreWaitMs = 500; // 500ms max wait on any semaphore;

// helper function for texture subresource calculations
// https://msdn.microsoft.com/en-us/library/windows/desktop/dn705766(v=vs.85).aspx
inline static uint32_t calcSubresource(uint32_t MipSlice, uint32_t ArraySlice, uint32_t PlaneSlice, uint32_t MipLevels, uint32_t ArraySize)
{
    return MipSlice + (ArraySlice * MipLevels) + (PlaneSlice * MipLevels * ArraySize);
}

D3D12 s_d3d12;
ICompute *getD3D12()
{
    return &s_d3d12;
}

std::wstring D3D12::getDebugName(Resource res)
{
    Microsoft::WRL::ComPtr<IUnknown> unknown = static_cast<IUnknown*>(res->native);
    Microsoft::WRL::ComPtr<ID3D12Pageable> pageable;
    Microsoft::WRL::ComPtr<IDXGIObject> dxgi;
    unknown.As(&pageable);
    unknown.As(&dxgi);
    wchar_t name[128] = {};
    std::wstring wname = L"Unknown";
    if (pageable)
    {
        UINT size = sizeof(name);
        if (FAILED(pageable->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            char sname[128] = {};
            size = sizeof(sname);
            if (SUCCEEDED(pageable->GetPrivateData(WKPDID_D3DDebugObjectName, &size, sname)))
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

const char * getDXGIFormatStr(uint32_t format)
{
    static constexpr const char *GDXGI_FORMAT_STR[] = {
        "DXGI_FORMAT_UNKNOWN",
        "DXGI_FORMAT_R32G32B32A32_TYPELESS",
        "DXGI_FORMAT_R32G32B32A32_FLOAT",
        "DXGI_FORMAT_R32G32B32A32_UINT",
        "DXGI_FORMAT_R32G32B32A32_SINT",
        "DXGI_FORMAT_R32G32B32_TYPELESS",
        "DXGI_FORMAT_R32G32B32_FLOAT",
        "DXGI_FORMAT_R32G32B32_UINT",
        "DXGI_FORMAT_R32G32B32_SINT",
        "DXGI_FORMAT_R16G16B16A16_TYPELESS",
        "DXGI_FORMAT_R16G16B16A16_FLOAT",
        "DXGI_FORMAT_R16G16B16A16_UNORM",
        "DXGI_FORMAT_R16G16B16A16_UINT",
        "DXGI_FORMAT_R16G16B16A16_SNORM",
        "DXGI_FORMAT_R16G16B16A16_SINT",
        "DXGI_FORMAT_R32G32_TYPELESS",
        "DXGI_FORMAT_R32G32_FLOAT",
        "DXGI_FORMAT_R32G32_UINT",
        "DXGI_FORMAT_R32G32_SINT",
        "DXGI_FORMAT_R32G8X24_TYPELESS",
        "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
        "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
        "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
        "DXGI_FORMAT_R10G10B10A2_TYPELESS",
        "DXGI_FORMAT_R10G10B10A2_UNORM",
        "DXGI_FORMAT_R10G10B10A2_UINT",
        "DXGI_FORMAT_R11G11B10_FLOAT",
        "DXGI_FORMAT_R8G8B8A8_TYPELESS",
        "DXGI_FORMAT_R8G8B8A8_UNORM",
        "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
        "DXGI_FORMAT_R8G8B8A8_UINT",
        "DXGI_FORMAT_R8G8B8A8_SNORM",
        "DXGI_FORMAT_R8G8B8A8_SINT",
        "DXGI_FORMAT_R16G16_TYPELESS",
        "DXGI_FORMAT_R16G16_FLOAT",
        "DXGI_FORMAT_R16G16_UNORM",
        "DXGI_FORMAT_R16G16_UINT",
        "DXGI_FORMAT_R16G16_SNORM",
        "DXGI_FORMAT_R16G16_SINT",
        "DXGI_FORMAT_R32_TYPELESS",
        "DXGI_FORMAT_D32_FLOAT",
        "DXGI_FORMAT_R32_FLOAT",
        "DXGI_FORMAT_R32_UINT",
        "DXGI_FORMAT_R32_SINT",
        "DXGI_FORMAT_R24G8_TYPELESS",
        "DXGI_FORMAT_D24_UNORM_S8_UINT",
        "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
        "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
        "DXGI_FORMAT_R8G8_TYPELESS",
        "DXGI_FORMAT_R8G8_UNORM",
        "DXGI_FORMAT_R8G8_UINT",
        "DXGI_FORMAT_R8G8_SNORM",
        "DXGI_FORMAT_R8G8_SINT",
        "DXGI_FORMAT_R16_TYPELESS",
        "DXGI_FORMAT_R16_FLOAT",
        "DXGI_FORMAT_D16_UNORM",
        "DXGI_FORMAT_R16_UNORM",
        "DXGI_FORMAT_R16_UINT",
        "DXGI_FORMAT_R16_SNORM",
        "DXGI_FORMAT_R16_SINT",
        "DXGI_FORMAT_R8_TYPELESS",
        "DXGI_FORMAT_R8_UNORM",
        "DXGI_FORMAT_R8_UINT",
        "DXGI_FORMAT_R8_SNORM",
        "DXGI_FORMAT_R8_SINT",
        "DXGI_FORMAT_A8_UNORM",
        "DXGI_FORMAT_R1_UNORM",
        "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
        "DXGI_FORMAT_R8G8_B8G8_UNORM",
        "DXGI_FORMAT_G8R8_G8B8_UNORM",
        "DXGI_FORMAT_BC1_TYPELESS",
        "DXGI_FORMAT_BC1_UNORM",
        "DXGI_FORMAT_BC1_UNORM_SRGB",
        "DXGI_FORMAT_BC2_TYPELESS",
        "DXGI_FORMAT_BC2_UNORM",
        "DXGI_FORMAT_BC2_UNORM_SRGB",
        "DXGI_FORMAT_BC3_TYPELESS",
        "DXGI_FORMAT_BC3_UNORM",
        "DXGI_FORMAT_BC3_UNORM_SRGB",
        "DXGI_FORMAT_BC4_TYPELESS",
        "DXGI_FORMAT_BC4_UNORM",
        "DXGI_FORMAT_BC4_SNORM",
        "DXGI_FORMAT_BC5_TYPELESS",
        "DXGI_FORMAT_BC5_UNORM",
        "DXGI_FORMAT_BC5_SNORM",
        "DXGI_FORMAT_B5G6R5_UNORM",
        "DXGI_FORMAT_B5G5R5A1_UNORM",
        "DXGI_FORMAT_B8G8R8A8_UNORM",
        "DXGI_FORMAT_B8G8R8X8_UNORM",
        "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
        "DXGI_FORMAT_B8G8R8A8_TYPELESS",
        "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
        "DXGI_FORMAT_B8G8R8X8_TYPELESS",
        "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
        "DXGI_FORMAT_BC6H_TYPELESS",
        "DXGI_FORMAT_BC6H_UF16",
        "DXGI_FORMAT_BC6H_SF16",
        "DXGI_FORMAT_BC7_TYPELESS",
        "DXGI_FORMAT_BC7_UNORM",
        "DXGI_FORMAT_BC7_UNORM_SRGB",
        "DXGI_FORMAT_AYUV",
        "DXGI_FORMAT_Y410",
        "DXGI_FORMAT_Y416",
        "DXGI_FORMAT_NV12",
        "DXGI_FORMAT_P010",
        "DXGI_FORMAT_P016",
        "DXGI_FORMAT_420_OPAQUE",
        "DXGI_FORMAT_YUY2",
        "DXGI_FORMAT_Y210",
        "DXGI_FORMAT_Y216",
        "DXGI_FORMAT_NV11",
        "DXGI_FORMAT_AI44",
        "DXGI_FORMAT_IA44",
        "DXGI_FORMAT_P8",
        "DXGI_FORMAT_A8P8",
        "DXGI_FORMAT_B4G4R4A4_UNORM",
        "DXGI_FORMAT_P208",
        "DXGI_FORMAT_V208",
        "DXGI_FORMAT_V408",
    };
    if (format < countof(GDXGI_FORMAT_STR))
    {
        return GDXGI_FORMAT_STR[format];
    }
        
    return "DXGI_INVALID_FORMAT";
}

struct CommandAllocators
{
    CommandAllocators() { }
    void init(ID3D12CommandQueue *pQueue, const std::wstring &sQueueName, uint64_t completedFenceValue)
    {
        // shouldn't be creating new allocators if old allocators are still in use
        assert(completedFenceValue >= m_fenceValueAfterLastSwitch);
        Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
        pQueue->GetDevice(IID_PPV_ARGS(&pDevice));

        D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();
        m_sName = sQueueName + L".Allocator";
        for (uint32_t u = 0; u < m_pAllocators.size(); ++u)
        {
            if (SUCCEEDED(pDevice->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&m_pAllocators[u]))))
            {
                m_pAllocators[u]->SetName(m_sName.c_str());
            }
            else
            {
                assert(false);
            }
        }

        // new allocators are not used yet - so safe to set this fence value to 0
        m_fenceValueAfterLastSwitch = 0;
    }
    void shutdown(uint64_t completedFenceValue)
    {
        std::fill(m_pAllocators.begin(), m_pAllocators.end(), nullptr);
    }
    ID3D12CommandAllocator& getAllocator(uint64_t signalledFenceValue, uint64_t completedFenceValue)
    {
        // can we switch to a new allocator?
        if (completedFenceValue >= m_fenceValueAfterLastSwitch)
        {
            m_uCurAllocator = (m_uCurAllocator + 1) % m_pAllocators.size();
            m_pAllocators[m_uCurAllocator]->Reset();
            // since we need the value AFTER the switch, we have to take signalledFenceValue + 1
            m_fenceValueAfterLastSwitch = signalledFenceValue + 1;

            m_timerStart = std::chrono::system_clock::now();
        }
        else
        {
            // detect the cases when allocator isn't reset for long time and report
            uint64_t uFenceDistance = std::abs((int64_t)(signalledFenceValue - m_fenceValueAfterLastSwitch));
            static const uint32_t kMaxFenceDistance = 100;
            if (uFenceDistance >= kMaxFenceDistance)
            {
                std::chrono::time_point<std::chrono::system_clock> timerEnd = std::chrono::system_clock::now();
                std::chrono::duration<double> fSecondsDistance = timerEnd - m_timerStart;
                static const uint32_t kMaxSecondsDistance = 5;
                if (fSecondsDistance.count() >= kMaxSecondsDistance)
                {
                    assert(false);
                    SL_LOG_WARN("Couldn't reset the allocator %S for %.2f seconds. signalledFenceValue=%lld, m_fenceValueAfterLastSwitch=%lld",
                        m_sName.c_str(), fSecondsDistance.count(), signalledFenceValue, m_fenceValueAfterLastSwitch);
                }
            }
        }

        return *m_pAllocators[m_uCurAllocator].Get();
    }

private:
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, 2> m_pAllocators;
    uint32_t m_uCurAllocator = 0;
    uint64_t m_fenceValueAfterLastSwitch = 0;
    std::chrono::time_point<std::chrono::system_clock> m_timerStart;
    std::wstring m_sName;
};

class CommandListContext : public ICommandListContext
{
    struct WaitingContext
    {
        WaitingContext(ID3D12Fence* pFence, uint64_t uSyncValue, const DebugInfo &debugInfo) :
            fence(pFence),
            syncValue(uSyncValue),
            m_debugInfo(debugInfo)
        {
        }
        ID3D12Fence* fence{};
        uint64_t syncValue{};
        void updateCompletedFenceValue()
        {
            uint64_t value = fence->GetCompletedValue();
            if (value == -1)
            {
                SL_LOG_ERROR("We've lost the Device.");
                return;
            }
            m_completedValue = value;
        }
        uint64_t m_completedValue = 0;
        DebugInfo m_debugInfo;
    };
    std::vector<WaitingContext> m_waitingQueue;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
    ID3D12GraphicsCommandList* m_cmdList{};

    CommandAllocators m_allocators;

    void updateCompletedFenceValue()
    {
        uint64_t value = m_fence->GetCompletedValue();
        if (value != (uint64_t)-1) // we get -1 when device is lost
        {
            m_lastCompletedFenceValue = value;
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValueGenerator = 0;
    uint64_t m_lastSignalledFenceValue = 0;
    uint64_t m_lastCompletedFenceValue = 0;

    HANDLE m_fenceEvent{};
    HANDLE m_fenceEventExternal{};
    std::vector<UINT64> m_fenceValue{};
    std::atomic<bool> m_cmdListIsRecording = false;
    uint32_t m_index = 0;
    uint32_t m_lastIndex = UINT_MAX;
    uint32_t m_bufferCount = 0;
    std::wstring m_name;
    std::mutex m_mtxQueueList;

public:
    void waitForVblank(SwapChain chain) override
    {
        IDXGIOutput* output = nullptr;
        ((IDXGISwapChain*)chain)->GetContainingOutput(&output);
        output->WaitForVBlank();
    }

    void getFrameStats(SwapChain chain, void* frameStats) override
    {
        DXGI_FRAME_STATISTICS* frameStatsPtr = (DXGI_FRAME_STATISTICS*)frameStats;
        HRESULT hr = ((IDXGISwapChain*)chain)->GetFrameStatistics(frameStatsPtr);
    }

    void getLastPresentID(SwapChain chain, uint32_t& id) override
    {
        HRESULT hr = ((IDXGISwapChain*)chain)->GetLastPresentCount(&id);
    }

    void init(const char* debugName, ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t count)
    {
        m_name = extra::utf8ToUtf16(debugName);
        m_cmdQueue = queue;
        auto cmdQueueDesc = m_cmdQueue->GetDesc();
        m_bufferCount = count;
        // To support DX11 fences have to be shared
        device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));
        m_fenceValue.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            m_fenceValue[i] = 0;
        }

        m_allocators.init(queue, m_name, m_lastCompletedFenceValue);
        ID3D12CommandAllocator &allocator = m_allocators.getAllocator(m_lastSignalledFenceValue, m_lastCompletedFenceValue);
        device->CreateCommandList(0, cmdQueueDesc.Type, &allocator, nullptr, IID_PPV_ARGS(&m_cmdList));

        m_cmdList->Close(); // Immediately close since it will be reset on first use
        m_cmdList->SetName((m_name + L" command list").c_str());

        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); 
        m_fenceEventExternal = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    void shutdown()
    {
        this->updateCompletedFenceValue();
        SL_SAFE_RELEASE(m_cmdList);
        m_fence = nullptr;
        CloseHandle(m_fenceEvent);
        CloseHandle(m_fenceEventExternal);
        m_allocators.shutdown(m_lastCompletedFenceValue);
    }

    RenderAPI getType() { return RenderAPI::eD3D12; }

    CommandList getCmdList()
    {
        return m_cmdList;
    }

    CommandQueue getCmdQueue()
    {
        return m_cmdQueue.Get();
    }

    CommandAllocator getCmdAllocator()
    {
        ID3D12CommandAllocator &allocator = m_allocators.getAllocator(m_lastSignalledFenceValue, m_lastCompletedFenceValue);
        return &allocator;
    }

    Handle getFenceEvent()
    {
        return m_fenceEvent;
    }

    Fence getFence(uint32_t index)
    {
        return m_fence.Get();
    }

    bool beginCommandList()
    {
        if (m_cmdListIsRecording)
        {
            SL_LOG_ERROR( "Command list not closed");
            return false;
        }

        this->updateCompletedFenceValue();
        ID3D12CommandAllocator& allocator = m_allocators.getAllocator(m_lastSignalledFenceValue, m_lastCompletedFenceValue);
        m_cmdListIsRecording = SUCCEEDED(m_cmdList->Reset(&allocator, nullptr));
        if (!m_cmdListIsRecording)
        {
            SL_LOG_ERROR( "%S command buffer - cannot reset command list", m_name.c_str());
        }
        return m_cmdListIsRecording;
    }

    bool executeCommandList(const GPUSyncInfo* info)
    {
        if (!m_cmdListIsRecording)
        {
            SL_LOG_ERROR( "Command list not opened");
            return false;
        }

        if (FAILED(m_cmdList->Close()))
        {
            SL_LOG_ERROR( "%S command buffer - cannot close command list", m_name.c_str());
            return false;
        }

        if (info)
        {
            if ((info->waitSemaphores.size() != info->waitValues.size()) ||
                (info->signalSemaphores.size() != info->signalValues.size()))
            {
                SL_LOG_ERROR("Mismatching semaphore array size");
                return false;
            }

            for (size_t i = 0; i < info->waitSemaphores.size(); i++)
            {
                waitGPUFence(info->waitSemaphores[i], info->waitValues[i],
                    chi::DebugInfo(__FILE__, __LINE__));
            }
        }

        ID3D12CommandList* const cmd_lists[] = { m_cmdList };
        m_cmdQueue->ExecuteCommandLists(ARRAYSIZE(cmd_lists), cmd_lists);

        if (info)
        {
            for (size_t i = 0; i < info->signalSemaphores.size(); i++)
            {
                signalGPUFence(info->signalSemaphores[i], info->signalValues[i]);
            }
        }

        if (!this->signalGPUFenceAt(m_index))
        {
            return false;
        }

        m_lastIndex = m_index;
        m_index = (m_index + 1) % m_bufferCount;
        m_cmdListIsRecording = false;

        return true;
    }

    bool isCommandListRecording()
    {
        return m_cmdListIsRecording;
    }

    WaitStatus waitWithoutDeadlock(uint32_t index, uint64_t value)
    {
        if (SUCCEEDED(m_fence->SetEventOnCompletion(value, m_fenceEvent)))
        {
            DWORD res{};
            uint32_t waitTime = 0;
            if (WaitForSingleObject(m_fenceEvent, kMaxSemaphoreWaitMs) == WAIT_TIMEOUT)
            {
                SL_LOG_WARN("Wait on gpu fence in '%S' timed out after %llums - index %u value %llu", m_name.c_str(), kMaxSemaphoreWaitMs, index, value);
                signalAllWaitingOnQueues();
                return WaitStatus::eTimeout;
            }
        }
        else
        {
            SL_LOG_ERROR("Failed to SetEventOnCompletion");
            return WaitStatus::eError;
        }
        return WaitStatus::eNoTimeout;
    }

    WaitStatus flushAll()
    {
        // signals complete in the same order they were generated. so we generate a new signal
        // and wait for it. this guarantees that all signals before it also complete
        if (signalGPUFenceAt(0))
        {
            return waitWithoutDeadlock(0, m_fenceValue[0]);
        }
        return WaitStatus::eError;
    }
    
    uint32_t getPrevCommandListIndex() override
    {
        assert(m_bufferCount > 0); // can't call this if you don't have any buffers
        return (m_index + m_bufferCount - 1) % m_bufferCount;
    }

    uint32_t getCurrentCommandListIndex()
    {
        return m_index;
    }

    uint64_t getSyncValueAtIndex(uint32_t idx)
    {
        assert(idx < m_bufferCount);
        return m_fenceValue[idx];
    }
    
    SyncPoint getSyncPointAtIndex(uint32_t idx) override
    {
        return { m_fence.Get(), m_fenceValue[idx]};
    }

    Fence getNextVkAcquireFence() override final
    {
        assert(false);
        SL_LOG_ERROR("Not implemented");
        return nullptr;
    }

    int acquireNextBufferIndex(SwapChain chain, uint32_t& index, Fence* waitSemaphore)
    {
        index = ((IDXGISwapChain4*)chain)->GetCurrentBackBufferIndex();
        return S_OK;
    }

    WaitStatus waitForCommandListToFinish(uint32_t index)
    {
        return waitWithoutDeadlock(index, m_fenceValue[index]);
    }

    bool didCommandListFinish(uint32_t index)
    {
        if (index >= m_bufferCount)
        {
            SL_LOG_ERROR( "Invalid index");
            return true;
        }
        this->updateCompletedFenceValue();
        return m_lastCompletedFenceValue >= m_fenceValue[index];
    }

    bool signalAllWaitingOnQueues()
    {
        std::lock_guard<std::mutex> lock(m_mtxQueueList);
        for (auto& other : m_waitingQueue)
        {
            // We are waiting on the GPU for these fences, signal them to get out of the deadlock
            auto syncValue = other.syncValue;
            auto completedValue = other.fence->GetCompletedValue();

            // Desperate times - desperate measures, make sure to signal new value
            if (completedValue < syncValue)
            {
                SL_LOG_WARN("The fence [%s][%d] has timed out - we're letting it go",
                    other.m_debugInfo.m_sFile, other.m_debugInfo.m_uLine);
                if (FAILED(other.fence->Signal(syncValue)))
                {
                    SL_LOG_ERROR("Failed to signal fence value %llu", other.syncValue);
                    return false;
                }
            }
            //SL_LOG_INFO("Signaled %S index %u value %llu", other.ctx->name.c_str(), other.clIndex, other.syncValue);
        }
        m_waitingQueue.clear();
        return true;
    }

    bool signalGPUFenceAt(uint32_t index) override
    {
        uint64_t value = InterlockedIncrement(&m_fenceValueGenerator);
        if (signalGPUFence(m_fence.Get(), value))
        {
            m_fenceValue[index] = value;
            m_lastSignalledFenceValue = value;
            return true;
        }
        return false;
    }

    bool signalGPUFence(Fence fence, uint64_t syncValue) override
    {
        if (FAILED(m_cmdQueue->Signal((ID3D12Fence*)fence, syncValue)))
        {
            SL_LOG_ERROR( "Failed to signal on the command queue");
            return false;
        }
        return true;
    }

    WaitStatus waitCPUFence(Fence fence, uint64_t syncValue)
    {
        // This can be called from any thread so make sure not to touch any internals
        auto d3d12Fence = ((ID3D12Fence*)fence);
        auto completedValue = d3d12Fence->GetCompletedValue();
        if (completedValue < syncValue)
        {
            if (SUCCEEDED(d3d12Fence->SetEventOnCompletion(syncValue, m_fenceEventExternal)))
            {
                if (WaitForSingleObject(m_fenceEventExternal, kMaxSemaphoreWaitMs) == WAIT_TIMEOUT)
                {
                    SL_LOG_WARN("Wait on gpu fence in '%S' timed out after 500ms value %llu", m_name.c_str(), syncValue);
                    return WaitStatus::eTimeout;
                }
            }
            else
            {
                return WaitStatus::eError;
            }
        }
        return WaitStatus::eNoTimeout;
    }

    void waitGPUFence(Fence fence, uint64_t syncValue, const DebugInfo &debugInfo) override
    {
        if (FAILED(m_cmdQueue->Wait((ID3D12Fence*)fence, syncValue)))
        {
            SL_LOG_ERROR( "Failed to wait on the command queue");
        }
        std::lock_guard<std::mutex> lock(m_mtxQueueList);
        bool found = false;
        for (auto& other : m_waitingQueue)
        {
            if (other.fence == fence)
            {
                found = true;
                other.fence = (ID3D12Fence*)fence;
                other.syncValue = syncValue;
                break;
            }
        }
        if (!found)
        {
            m_waitingQueue.push_back({ (ID3D12Fence*)fence, syncValue, debugInfo });
        }
        //SL_LOG_INFO("%S waiting for %S at i%u:%llu", name.c_str(), tmp->name.c_str(), clIndex, tmp->fenceValue[clIndex] + syncValueOffset);
    }

    void syncGPU(const GPUSyncInfo* info)
    {
        if (info)
        {
            assert(info->waitSemaphores.size() == info->waitValues.size());
            assert(info->signalSemaphores.size() == info->signalValues.size());

            if ((info->waitSemaphores.size() != info->waitValues.size()) ||
                (info->signalSemaphores.size() != info->signalValues.size()))
            {
                SL_LOG_ERROR("Mismatching semaphore array size");
                return;
            }
            for(size_t i = 0; i < info->waitSemaphores.size(); i++)
            {
                waitGPUFence(info->waitSemaphores[i], info->waitValues[i],
                    chi::DebugInfo(__FILE__, __LINE__));
            }
            for (size_t i = 0; i < info->signalSemaphores.size(); i++)
            {
                signalGPUFence(info->signalSemaphores[i], info->signalValues[i]);
            }
        }
    }

    void waitOnGPUForTheOtherQueue(const ICommandListContext* other, uint32_t clIndex,
        uint64_t syncValue, const DebugInfo &debugInfo) override
    {
        auto tmp = (const CommandListContext*)other;
        if (tmp->m_cmdQueue == m_cmdQueue)
        {
            // Can't wait on ourselves
            return;
        }

        waitGPUFence(tmp->m_fence.Get(), syncValue, debugInfo);
    }

    WaitStatus waitForCommandList(FlushType ft)
    {
        // Flush command list, to avoid it still referencing resources that may be destroyed after this call
        if (m_cmdListIsRecording)
        {
            if (!executeCommandList(nullptr))
            {
                return WaitStatus::eError;
            }
        }

        if (ft == eCurrent)
        {
            return waitWithoutDeadlock(m_lastIndex, m_fenceValue[m_lastIndex]);
        }
        else if (ft == eDefault)
        {
            return waitWithoutDeadlock(m_lastIndex, m_fenceValue[m_lastIndex] - 1);
        }

        return WaitStatus::eNoTimeout;
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

    virtual void monitoringThreadTick() override
    {
        updateCompletedFenceValue();
        std::lock_guard<std::mutex> lock(m_mtxQueueList);
        for (uint32_t u = (uint32_t)m_waitingQueue.size() - 1; u < m_waitingQueue.size(); --u)
        {
            auto& w = m_waitingQueue[u];
            w.updateCompletedFenceValue();
            // if the value has already passed - this element can be removed from the list
            if (w.m_completedValue >= w.syncValue)
            {
                w = *m_waitingQueue.rbegin();
                m_waitingQueue.pop_back();
            }
        }
    }
};

ComputeStatus D3D12::init(Device device, param::IParameters* params)
{
    // First check if this is dx11 on dx12
    ID3D11Device* deviceD3D11{};
    ((IUnknown*)device)->QueryInterface(&deviceD3D11);
    if (deviceD3D11)
    {
        SL_LOG_INFO("Detected DX11 on DX12 scenario");
        deviceD3D11->Release();
        IDXGIDevice* dxgiDevice;
        deviceD3D11->QueryInterface(&dxgiDevice);
        if (!dxgiDevice)
        {
            SL_LOG_ERROR( "Cannot obtain IDXGIDevice");
            return ComputeStatus::eError;
        }
        dxgiDevice->Release();
        IDXGIAdapter* adapter{};
        dxgiDevice->GetAdapter(&adapter);
        if (!adapter)
        {
            SL_LOG_ERROR( "Cannot obtain IDXGIAdapter");
            return ComputeStatus::eError;
        }
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device))))
        {
            SL_LOG_ERROR( "D3D12CreateDevice failed");
            return ComputeStatus::eError;
        }
        adapter->Release();

        dx11On12 = true;

        // From this point use new D3D12 device
        device = m_device;
    }

    Generic::init(device, params);

    m_device = (ID3D12Device*)device;

    UINT NodeCount = m_device->GetNodeCount();
    m_visibleNodeMask = (1 << NodeCount) - 1;

    if (NodeCount > MAX_NUM_NODES)
    {
        SL_LOG_ERROR( " too many GPU nodes");
        return ComputeStatus::eError;
    }

    HRESULT hr = S_OK;

    // The ability to cast one fully typed resource to a compatible fully typed cast resource
    // (instead of creating the resource typeless)
    // should be supported by all our GPUs with drivers going back as far as RS2 which is way before the introduction of DLSS
    // There are some restrictions though see :
    // https://microsoft.github.io/DirectX-Specs/d3d/RelaxedCasting.html
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 FeatureOptions3;
    hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &FeatureOptions3, sizeof(FeatureOptions3));
    if (FAILED(hr) || !FeatureOptions3.CastingFullyTypedFormatSupported)
    {
        SL_LOG_ERROR( " CheckFeatureSupport() call did not succeed or the driver did not report CastingFullyTypedFormatSupported. Windows 10 RS2 or higher was expected. %s", std::system_category().message(hr).c_str());
        m_dbgSupportRs2RelaxedConversionRules = false;
    }
    else
    {
        m_dbgSupportRs2RelaxedConversionRules = true;
    }

    m_heap = new HeapInfo;

    for(UINT Node = 0; Node < NodeCount; Node++)
    {
        // create desc heaps for SRV/UAV/CBV
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            // Each wraparound we target a different part of the heap to prevent direct reuse
            heapDesc.NumDescriptors = SL_MAX_D3D12_DESCRIPTORS * SL_DESCRIPTOR_WRAPAROUND_CAPACITY;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            heapDesc.NodeMask = (1 << Node);
            hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap->descriptorHeap[Node]));
            if (FAILED(hr)) { SL_LOG_ERROR( " failed to create descriptor heap, hr=%d", hr); return ComputeStatus::eError; }
            m_heap->descriptorHeap[Node]->SetName(L"sl.chi.heapGPU");
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap->descriptorHeapCPU[Node]));
            if (FAILED(hr)) { SL_LOG_ERROR( " failed to create descriptor heap, hr=%d", hr); return ComputeStatus::eError; }
            m_heap->descriptorHeapCPU[Node]->SetName(L"sl.chi.heapCPU");
        }
        
        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    m_bFastUAVClearSupported = true;

    genericPostInit();

    CHI_CHECK(createKernel((void*)copy_to_buffer_cs, copy_to_buffer_cs_len, "copy_to_buffer.cs", "main", m_copyKernel));

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::shutdown()
{
    CHI_CHECK(destroyKernel(m_copyKernel));
    m_copyKernel = {};

    for (UINT node = 0; node < MAX_NUM_NODES; node++)
    {
        for (auto section : m_sectionPerfMap[node])
        {
            for (int i = 0; i < SL_READBACK_QUEUE_SIZE; i++)
            {
                SL_SAFE_RELEASE(section.second.queryHeap[i]);
                SL_SAFE_RELEASE(section.second.queryBufferReadback[i]);
            }
        }
        m_sectionPerfMap[node].clear();
        SL_SAFE_RELEASE(m_heap->descriptorHeap[node]);
        SL_SAFE_RELEASE(m_heap->descriptorHeapCPU[node]);
    }

    delete m_heap;
    m_heap = {};

    for (auto& v : m_psoMap)
    {
        SL_LOG_VERBOSE("Destroying pipeline state 0x%llx", v.second);
        SL_SAFE_RELEASE(v.second);
    }
    for (auto& v : m_rootSignatureMap)
    {
        SL_LOG_VERBOSE("Destroying root signature 0x%llx", v.second);
        SL_SAFE_RELEASE(v.second);
    }
    m_rootSignatureMap.clear();
    m_psoMap.clear();

    ComputeStatus Res = ComputeStatus::eOk;
    for (auto& k : m_kernels)
    {
        auto kernel = (KernelDataBase*)k.second;
        SL_LOG_VERBOSE("Destroying kernel %s", kernel->name.c_str());
        delete kernel;
    }
    m_kernels.clear();

    m_dispatchContext.clear();

    auto res = Generic::shutdown();

    if (dx11On12)
    {
        // We created this device so release it
        SL_SAFE_RELEASE(m_device);
    }

    return res;
}

ComputeStatus D3D12::clearCache()
{
    for (auto& resources : m_resourceData)
    {
        resources.second.clear();
    }
    m_resourceData.clear();

    return Generic::clearCache();
}

ComputeStatus D3D12::getRenderAPI(RenderAPI &OutType)
{
    OutType = RenderAPI::eD3D12;
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::restorePipeline(CommandList cmdBuffer)
{
    if (!cmdBuffer) return ComputeStatus::eOk;

    D3D12ThreadContext* thread = (D3D12ThreadContext*)m_getThreadContext();
    auto cmdList = ((ID3D12GraphicsCommandList*)cmdBuffer);
    
    assert(thread->cmdList->m_base == cmdBuffer);

    if (thread->cmdList->m_numHeaps > 0)
    {
        cmdList->SetDescriptorHeaps(thread->cmdList->m_numHeaps, thread->cmdList->m_heaps);
    }
    if (thread->cmdList->m_rootSignature)
    {
        cmdList->SetComputeRootSignature(thread->cmdList->m_rootSignature);
        for (auto& pair : thread->cmdList->m_mapHandles)
        {
            cmdList->SetComputeRootDescriptorTable(pair.first, pair.second);
        }
        for (auto& pair : thread->cmdList->m_mapCBV)
        {
            cmdList->SetComputeRootConstantBufferView(pair.first, pair.second);
        }
        for (auto& pair : thread->cmdList->m_mapSRV)
        {
            cmdList->SetComputeRootShaderResourceView(pair.first, pair.second);
        }
        for (auto& pair : thread->cmdList->m_mapUAV)
        {
            cmdList->SetComputeRootUnorderedAccessView(pair.first, pair.second);
        }
        for (auto& pair : thread->cmdList->m_mapConstants)
        {
            cmdList->SetComputeRoot32BitConstants(pair.first, pair.second.Num32BitValuesToSet, pair.second.SrcData, pair.second.DestOffsetIn32BitValues);
        }
    }
    if (thread->cmdList->m_pso)
    {
        cmdList->SetPipelineState(thread->cmdList->m_pso);
    }
    if (thread->cmdList->m_so)
    {
        static_cast<ID3D12GraphicsCommandList4*>(cmdList)->SetPipelineState1(thread->cmdList->m_so);
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getBarrierResourceState(uint32_t barrierType, ResourceState& resourceStates)
{
    resourceStates = ResourceState::ePresent;

    if (barrierType & (D3D12_BARRIER_LAYOUT_SHADER_RESOURCE | D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE | D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE))
        resourceStates = resourceStates | (ResourceState::eTextureRead);
    if (barrierType & (D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS | D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS | D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS))
        resourceStates = resourceStates | ResourceState::eStorageRW;
    if (barrierType & D3D12_BARRIER_LAYOUT_RENDER_TARGET)
        resourceStates = resourceStates | ResourceState::eColorAttachmentWrite;
    if (barrierType & D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ)
        resourceStates = resourceStates | ResourceState::eDepthStencilAttachmentRead;
    if (barrierType & D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE)
        resourceStates = resourceStates | ResourceState::eDepthStencilAttachmentWrite;
    if (barrierType & (D3D12_BARRIER_LAYOUT_COPY_SOURCE | D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE | D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE))
        resourceStates = resourceStates | ResourceState::eCopySource;
    if (barrierType & (D3D12_BARRIER_LAYOUT_COPY_DEST | D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST | D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST))
        resourceStates = resourceStates | ResourceState::eCopyDestination;
    if (barrierType & D3D12_BARRIER_LAYOUT_RESOLVE_DEST)
        resourceStates = resourceStates | ResourceState::eResolveDestination;
    if (barrierType & D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE)
        resourceStates = resourceStates | ResourceState::eResolveSource;

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getResourceState(uint32_t states, ResourceState& resourceStates)
{
    resourceStates = ResourceState::ePresent;

    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
        resourceStates = resourceStates | (ResourceState::eConstantBuffer | ResourceState::eVertexBuffer);
    if (states & (D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
        resourceStates = resourceStates | (ResourceState::eTextureRead);
    if (states & (D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        resourceStates = resourceStates | (ResourceState::eStorageRead);
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER)
        resourceStates = resourceStates | ResourceState::eIndexBuffer;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
        resourceStates = resourceStates | ResourceState::eArgumentBuffer;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        resourceStates = resourceStates | ResourceState::eStorageRW;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET)
        resourceStates = resourceStates | ResourceState::eColorAttachmentWrite;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_READ)
        resourceStates = resourceStates | ResourceState::eDepthStencilAttachmentRead;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_WRITE)
        resourceStates = resourceStates | ResourceState::eDepthStencilAttachmentWrite;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_SOURCE)
        resourceStates = resourceStates | ResourceState::eCopySource;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST)
        resourceStates = resourceStates | ResourceState::eCopyDestination;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        resourceStates = resourceStates | (ResourceState::eAccelStructRead | ResourceState::eAccelStructWrite);
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RESOLVE_DEST)
        resourceStates = resourceStates | ResourceState::eResolveDestination;
    if (states & D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RESOLVE_SOURCE)
        resourceStates = resourceStates | ResourceState::eResolveSource;

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getNativeResourceState(ResourceState states, uint32_t& resourceStates)
{
    resourceStates = D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON;

    if (states & (ResourceState::eConstantBuffer | ResourceState::eVertexBuffer))
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (states & (ResourceState::eTextureRead))
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (states & (ResourceState::eGenericRead))
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ;
    if (states & (ResourceState::eStorageRead))
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (states & ResourceState::eIndexBuffer)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (states & ResourceState::eArgumentBuffer)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if ((states & ResourceState::eStorageWrite) && (states & ResourceState::eStorageRead))
    {
        // Clear out incompatible state if we want read/write access
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        resourceStates &= ~(D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (states & ResourceState::eColorAttachmentWrite)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (states & ResourceState::eDepthStencilAttachmentRead)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_READ;
    if (states & ResourceState::eDepthStencilAttachmentWrite)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (states & ResourceState::eCopySource)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (states & ResourceState::eCopyDestination)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST;
    if (states & (ResourceState::eAccelStructRead | ResourceState::eAccelStructWrite))
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if (states & ResourceState::eResolveDestination)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RESOLVE_DEST;
    if (states & ResourceState::eResolveSource)
        resourceStates |= D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::createKernel(void *blobData, uint32_t blobSize, const char* fileName, const char *entryPoint, Kernel &kernel)
{
    if (!blobData || !fileName || !entryPoint)
    {
        if (fileName && entryPoint)
        {
            SL_LOG_ERROR( "Missing blobData for %s, (Entry: %s)", fileName, entryPoint);
        }
        else
        {
            SL_LOG_ERROR( "Unable to create kernel (bad fileName and/or entryPoint)");
        }
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
    KernelDataBase*data = {};
    bool missing = false;
    {
        std::scoped_lock lock(m_mutexKernel);
        auto it = m_kernels.find(hash);
        missing = it == m_kernels.end();
        if (missing)
        {
            data = new KernelDataBase;
            data->hash = hash;
            m_kernels[hash] = data;
        }
        else
        {
            data = (KernelDataBase*)(*it).second;
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

ComputeStatus D3D12::destroyKernel(Kernel& InKernel)
{
    if (!InKernel) return ComputeStatus::eOk; // fine to destroy null kernels
    std::scoped_lock lock(m_mutexKernel);
    auto it = m_kernels.find(InKernel);
    if (it == m_kernels.end())
    {
        SL_LOG_WARN("Kernel %llu missing in cache, most likely destroyed already", InKernel);
    }
    else
    {
        auto data = (KernelDataBase*)(it->second);
        SL_LOG_VERBOSE("Destroying kernel %s", data->name.c_str());
        delete it->second;
        m_kernels.erase(it);
        InKernel = {};
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::createCommandListContext(CommandQueue queue, uint32_t count, ICommandListContext*& ctx, const char friendlyName[])
{
    auto tmp = new CommandListContext();
    tmp->init(friendlyName, m_device, (ID3D12CommandQueue*)queue, count);
    ctx = tmp;
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::destroyCommandListContext(ICommandListContext* ctx)
{
    if (ctx)
    {
        auto tmp = (CommandListContext*)ctx;
        tmp->shutdown();
        delete tmp;
    }
    return ComputeStatus::eOk;
}

uint64_t D3D12::getCompletedValue(Fence fence)
{
    return ((ID3D12Fence*)fence)->GetCompletedValue();
}

ComputeStatus D3D12::createCommandQueue(CommandQueueType type, CommandQueue& queue, const char friendlyName[], uint32_t index)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type == CommandQueueType::eGraphics ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    ID3D12CommandQueue* tmp;
    if (FAILED(m_device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void**)&tmp)))
    {
        SL_LOG_ERROR( "Failed to create command queue %s", friendlyName);
        return ComputeStatus::eError;
    }
    queue = tmp;
    sl::Resource r(ResourceType::eCommandQueue, queue);
    setDebugName(&r, friendlyName);
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::destroyCommandQueue(CommandQueue& queue)
{
    if (queue)
    {
        auto tmp = (ID3D12CommandQueue*)queue;
        SL_SAFE_RELEASE(tmp);
    }    
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::createFence(FenceFlags flags, uint64_t initialValue, Fence& outFence, const char friendlyName[])
{
    ID3D12Fence* fence{};
    D3D12_FENCE_FLAGS d3d12Flags = D3D12_FENCE_FLAG_NONE;
    if (flags & eFenceFlagsShared)
    {
        d3d12Flags |= D3D12_FENCE_FLAG_SHARED;
    }
    if (FAILED(m_device->CreateFence(initialValue, d3d12Flags, IID_PPV_ARGS(&fence))))
    {
        SL_LOG_ERROR( "Failed to create ID3D12Fence");
    }
    else
    {
        outFence = fence;
        sl::Resource r(ResourceType::eFence, fence);
        setDebugName(&r, friendlyName);
    }
    return fence ? ComputeStatus::eOk : ComputeStatus::eError;
}

ComputeStatus D3D12::getFullscreenState(SwapChain chain, bool& fullscreen)
{
    if (!chain) return ComputeStatus::eInvalidArgument;
    IDXGISwapChain* swapChain = (IDXGISwapChain*)chain;

    BOOL fs = false;
    if (FAILED(swapChain->GetFullscreenState(&fs, NULL)))
    {
        SL_LOG_ERROR("Failed to get fullscreen state");
    }

    fullscreen = (bool)fs;
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::setFullscreenState(SwapChain chain, bool fullscreen, Output out)
{
    if (!chain) return ComputeStatus::eInvalidArgument;
    IDXGISwapChain* swapChain = (IDXGISwapChain*)chain;
    if (FAILED(swapChain->SetFullscreenState(fullscreen, (IDXGIOutput*)out)))
    {
        SL_LOG_ERROR( "Failed to set fullscreen state");
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getRefreshRate(SwapChain chain, float& refreshRate)
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
    SL_LOG_ERROR( "Failed to retrieve refresh rate from swapchain 0x%llx", chain);
    return ComputeStatus::eError;
}

ComputeStatus D3D12::getSwapChainBuffer(SwapChain chain, uint32_t index, Resource& buffer)
{
    ID3D12Resource* tmp;
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

ComputeStatus D3D12::bindSharedState(CommandList InCmdList, UINT node)
{
    auto& ctx = m_dispatchContext.getContext();
    ctx.node = node;
    ctx.cmdList = (ID3D12GraphicsCommandList*)InCmdList;

    ID3D12DescriptorHeap *Heaps[] = { m_heap->descriptorHeap[ctx.node] };
    ctx.cmdList->SetDescriptorHeaps(1, Heaps);

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::bindKernel(const Kernel kernelToBind)
{
    auto& ctx = m_dispatchContext.getContext();
    
    {
        std::scoped_lock lock(m_mutexKernel);
        auto it = m_kernels.find(kernelToBind);
        if (it == m_kernels.end())
        {
            SL_LOG_ERROR( "Trying to bind kernel which has not been created");
            return ComputeStatus::eInvalidCall;
        }
        ctx.kernel = (KernelDataBase*)(*it).second;
    }

    if (!ctx.kddMap)
    {
        ctx.kddMap = new KernelDispatchDataMap();
    }
    auto it = ctx.kddMap->find(ctx.kernel->hash);
    if (it == ctx.kddMap->end())
    {
        (*ctx.kddMap)[ctx.kernel->hash] = {};
    }
    else
    {
        (*it).second.numSamplers = 0;
        (*it).second.slot = 0;
    }
    //SL_LOG_INFO("Binding kernel %s:%s", ctx.kernel->name.c_str(), ctx.kernel->entryPoint.c_str());
    
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::bindSampler(uint32_t pos, uint32_t base, Sampler sampler)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!ctx.kernel || base >= 8) return ComputeStatus::eInvalidArgument;

    auto &kdd = (*ctx.kddMap)[ctx.kernel->hash];
    if (sampler == Sampler::eSamplerPointClamp)
    {
        kdd.samplers[base] = CD3DX12_STATIC_SAMPLER_DESC(base, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    }
    else if (sampler == Sampler::eSamplerPointMirror)
    {
        kdd.samplers[base] = CD3DX12_STATIC_SAMPLER_DESC(base, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
    }
    else if (sampler == Sampler::eSamplerLinearClamp)
    {
        kdd.samplers[base] = CD3DX12_STATIC_SAMPLER_DESC(base, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    }
    else if (sampler == Sampler::eSamplerLinearMirror)
    {
        kdd.samplers[base] = CD3DX12_STATIC_SAMPLER_DESC(base, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
    }
    else
    {
        kdd.samplers[base] = CD3DX12_STATIC_SAMPLER_DESC(base, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    }
    kdd.numSamplers = std::max(base + 1, kdd.numSamplers);

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::bindConsts(uint32_t pos, uint32_t base, void *data, size_t dataSize, uint32_t instances)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!ctx.kernel) return ComputeStatus::eInvalidArgument;

    if (instances < 3)
    {
        SL_LOG_WARN("Detected too low instance count for circular constant buffer - please use num_viewports * 3 formula");
    }

    auto &kdd = (*ctx.kddMap)[ctx.kernel->hash];
    kdd.slot = pos;
    if (kdd.addSlot(kdd.slot))
    {
        kdd.rootRanges[kdd.slot].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, base);
        kdd.rootParameters[kdd.slot].InitAsConstantBufferView(base);
    }
    if (!kdd.cb[kdd.slot])
    {
        kdd.cb[kdd.slot] = new ConstantBuffer();
        kdd.cb[kdd.slot]->create(m_device, (uint32_t)dataSize, instances);
    }

    if (data)
    {
        auto idx = kdd.cb[kdd.slot]->getIndex();
        kdd.cb[kdd.slot]->copyStagingToGpu(data, idx);
        kdd.handles[kdd.slot] = kdd.cb[kdd.slot]->getGpuVirtualAddress(idx);
        kdd.cb[kdd.slot]->advanceIndex();
    }

#ifndef SL_PRODUCTION
    kdd.validate(kdd.slot, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, base);
    //SL_LOG_INFO("binding CBV 0x%llx", kdd.handles[slot]);
#endif
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::bindTexture(uint32_t pos, uint32_t base, Resource resource, uint32_t mipOffset, uint32_t mipLevels)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!ctx.kernel) return ComputeStatus::eInvalidArgument;

    auto &kdd = (*ctx.kddMap)[ctx.kernel->hash];
    kdd.slot = pos;
    if (kdd.addSlot(kdd.slot))
    {
        kdd.rootRanges[kdd.slot].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, base);
        kdd.rootParameters[kdd.slot].InitAsDescriptorTable(1, &kdd.rootRanges[kdd.slot]);
    }

    // Resource can be null if shader is not using this slot
    if (resource && resource->native)
    {
        ResourceDriverData data = {};
        CHI_CHECK(getTextureDriverData(resource, data, mipOffset, mipLevels));
        auto handle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_heap->descriptorHeap[ctx.node]->GetGPUDescriptorHandleForHeapStart(), data.descIndex, m_descriptorSize);
        kdd.handles[kdd.slot] = handle.ptr;

#ifndef SL_PRODUCTION
        kdd.validate(kdd.slot, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, base);
        //std::wstring name = getDebugName(resource);
        //chi::ResourceDescription desc;
        //getResourceDescription(resource, desc);
        //SL_LOG_INFO("binding SRV 0x%llx(%S) mip:%u mips:%u (%u,%u:%u)", kdd.handles[slot], name.c_str(), mipOffset, mipLevels, desc.width, desc.height, desc.mips);
#endif
    }
    else
    {
        kdd.handles[kdd.slot] = 0;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::bindRWTexture(uint32_t pos, uint32_t base, Resource resource, uint32_t mipOffset)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!ctx.kernel) return ComputeStatus::eInvalidArgument;

    auto &kdd = (*ctx.kddMap)[ctx.kernel->hash];
    kdd.slot = pos;
    if (kdd.addSlot(kdd.slot))
    {
        kdd.rootRanges[kdd.slot].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, base);
        kdd.rootParameters[kdd.slot].InitAsDescriptorTable(1, &kdd.rootRanges[kdd.slot]);
    }

    // Resource can be null if shader is not using this slot
    if (resource && resource->native)
    {
        ResourceDriverData data = {};
        CHI_CHECK(getSurfaceDriverData(resource, data, mipOffset));
        auto handle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_heap->descriptorHeap[ctx.node]->GetGPUDescriptorHandleForHeapStart(), data.descIndex, m_descriptorSize);
        kdd.handles[kdd.slot] = handle.ptr;
#ifndef SL_PRODUCTION
        kdd.validate(kdd.slot, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, base);
        //std::wstring name = getDebugName(resource);
        //chi::ResourceDescription desc;
        //getResourceDescription(resource, desc);
        //SL_LOG_INFO("binding UAV 0x%llx(%S) mip:%u (%u,%u:%u)", kdd.handles[slot], name.c_str(), mipOffset, desc.width, desc.height, desc.mips);
#endif
    }
    else
    {
        kdd.handles[kdd.slot] = 0;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::bindRawBuffer(uint32_t pos, uint32_t base, Resource resource)
{
    // This is still just a UAV for D3D12 so reuse the other method
    // Note that UAV creation checks for buffers and modifies view accordingly (D3D12_BUFFER_UAV_FLAG_RAW etc.)
    return bindRWTexture(pos, base, resource);
}

ComputeStatus D3D12::dispatch(uint32_t blocksX, uint32_t blocksY, uint32_t blocksZ)
{
    auto& ctx = m_dispatchContext.getContext();
    if (!ctx.kernel) return ComputeStatus::eInvalidArgument;

    auto &kdd = (*ctx.kddMap)[ctx.kernel->hash];
    ComputeStatus Res = ComputeStatus::eOk;
    
    {
        if (!kdd.rootSignature)
        {
            //! Debug driver complains if we leave empty slot for the sampler so find and remove any.
            //! 
            //! We use static samplers always.
            auto rootParameters = kdd.rootParameters;
            auto it = rootParameters.begin();
            while (it != rootParameters.end())
            {
                auto& param = *it;
                if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && param.DescriptorTable.NumDescriptorRanges == 0)
                {
                    it = rootParameters.erase(it);
                    continue;
                }
                it++;
            }

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
            rootSignatureDesc.Init((UINT)rootParameters.size(), rootParameters.data(), kdd.numSamplers, kdd.samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
            auto hash = hashRootSignature(rootSignatureDesc);
            auto node = ctx.node << 1;

            {
                std::scoped_lock lock(m_mutexKernel);
                auto it = m_rootSignatureMap.find(hash);
                if (it == m_rootSignatureMap.end())
                {
                    ID3DBlob *signature;
                    ID3DBlob *error;
                    D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
                    if (error)
                    {
                        SL_LOG_ERROR( "D3D12SerializeRootSignature failed %s", (const char*)error->GetBufferPointer());
                        error->Release();
                        return ComputeStatus::eError;
                    }
                    if (FAILED(m_device->CreateRootSignature(node, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&kdd.rootSignature))))
                    {
                        SL_LOG_ERROR( "Failed to create root signature");
                        return ComputeStatus::eError;
                    }
                    SL_LOG_VERBOSE("Created root signature 0x%llx with hash %llu", kdd.rootSignature, hash);
                    m_rootSignatureMap[hash] = kdd.rootSignature;
                }
                else
                {
                    kdd.rootSignature = (*it).second;
                }
            }

            {
                hash_combine(hash, ctx.kernel->hash);
                std::scoped_lock lock(m_mutexKernel);
                auto it = m_psoMap.find(hash);
                if (it == m_psoMap.end())
                {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
                    psoDesc.pRootSignature = kdd.rootSignature;
                    psoDesc.CS = { ctx.kernel->kernelBlob.data(), ctx.kernel->kernelBlob.size() };
                    psoDesc.NodeMask = node;
                    if (FAILED(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&kdd.pso))))
                    {
                        SL_LOG_ERROR( "Failed to create CS pipeline state");
                        return ComputeStatus::eError;
                    }
                    SL_LOG_VERBOSE("Created pipeline state 0x%llx with hash %llu", kdd.pso, hash);
                    m_psoMap[hash] = kdd.pso;
                }
                else
                {
                    kdd.pso = (*it).second;
                }
            }
        }

        if (!kdd.rootSignature || !kdd.pso)
        {
            SL_LOG_ERROR( "Failed to create root signature or pso for kernel %s:%s", ctx.kernel->name.c_str(), ctx.kernel->entryPoint.c_str());
            return ComputeStatus::eError;
        }

        ctx.cmdList->SetComputeRootSignature(kdd.rootSignature);
        ctx.cmdList->SetPipelineState(kdd.pso);

        //! Set root parameters by accounting for the empty sampler slot(s) (if any)
        //! 
        uint32_t slot = 0;
        auto itp = kdd.rootParameters.begin();
        auto ith = kdd.handles.begin();
        while (itp != kdd.rootParameters.end() && ith != kdd.handles.end())
        {
            const auto& param = *itp;
            const auto& handle = *ith;
            if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV)
            {
                ctx.cmdList->SetComputeRootConstantBufferView(slot, { handle });
            }
            else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            {
                if (param.DescriptorTable.NumDescriptorRanges == 0)
                {
                    // Empty slot, just skip
                    itp++;
                    ith++;
                    continue;
                }
                // To avoid triggering debug layer error, nullptr is not allowed
                if (handle)
                {
                    ctx.cmdList->SetComputeRootDescriptorTable(slot, { handle });
                }
            }
            itp++;
            ith++;
            slot++;
        }
        ctx.cmdList->Dispatch(blocksX, blocksY, blocksZ);
    }

    return Res;
}

size_t D3D12::hashRootSignature(const CD3DX12_ROOT_SIGNATURE_DESC& desc)
{
    size_t h = 0;
    hash_combine(h, desc.Flags);
    hash_combine(h, desc.NumParameters);
    hash_combine(h, desc.NumStaticSamplers);
    for (uint32_t i = 0; i < desc.NumStaticSamplers; i++)
    {
        hash_combine(h, desc.pStaticSamplers[i].Filter);
        hash_combine(h, desc.pStaticSamplers[i].ShaderRegister);
        hash_combine(h, desc.pStaticSamplers[i].AddressU);
        hash_combine(h, desc.pStaticSamplers[i].AddressV);
        hash_combine(h, desc.pStaticSamplers[i].AddressW);
        hash_combine(h, desc.pStaticSamplers[i].MipLODBias);
        hash_combine(h, desc.pStaticSamplers[i].ShaderVisibility);
    }
    for (uint32_t i = 0; i < desc.NumParameters; i++)
    {
        hash_combine(h, desc.pParameters[i].ParameterType);
        hash_combine(h, desc.pParameters[i].ShaderVisibility);
        if (desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            hash_combine(h, desc.pParameters[i].DescriptorTable.NumDescriptorRanges);
            for (uint32_t j = 0; j < desc.pParameters[i].DescriptorTable.NumDescriptorRanges; j++)
            {
                hash_combine(h, desc.pParameters[i].DescriptorTable.pDescriptorRanges[j].RangeType);
            }
        }
        else if (desc.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV)
        {
            hash_combine(h, desc.pParameters[i].Descriptor.RegisterSpace);
        }
        else
        {
            SL_LOG_ERROR( "Unsupported parameter type in root signature");
        }
    }
    return h;
}

UINT D3D12::getNewAndIncreaseDescIndex()
{
    // This method is thread safe since it is just a helper for cache texture or surface

    auto node = 0; // FIX THIS
    if ((m_heap->descIndex[node] + 1) >= SL_MAX_D3D12_DESCRIPTORS)
    {
        // We've looped around our Descriptor heap
        // There's no way we can keep the old cached Descriptors as valid..
        // Invalidate all caches and force the SetInput() calls to set up new ones
        // It will be a slow burn for a while, but hopefully that isn't too frequent ??
        SL_LOG_WARN("D3D12 Descriptor heap wrap around. Clearing all cache and reallocating from scratch again. This is impacting performance - please do NOT change the tagged resources every frame");
        memset(m_heap->descIndex, 0, MAX_NUM_NODES * sizeof(UINT));
        assert(m_heap->descIndex[node] == 0);
        m_heap->wrapAroundCount = (m_heap->wrapAroundCount + 1) % SL_DESCRIPTOR_WRAPAROUND_CAPACITY;
        m_resourceData.clear();
    }
    UINT ReturnedIndex = m_heap->descIndex[node] + SL_MAX_D3D12_DESCRIPTORS * m_heap->wrapAroundCount;
    m_heap->descIndex[node] = (m_heap->descIndex[node] + 1) % SL_MAX_D3D12_DESCRIPTORS;

    return ReturnedIndex;
}

ComputeStatus D3D12::getTextureDriverData(Resource res, ResourceDriverData &data, uint32_t mipOffset, uint32_t mipLevels, Sampler sampler)
{
    if (!res || !res->native) return ComputeStatus::eInvalidArgument;

    ID3D12Resource* resource = (ID3D12Resource*)(res->native);

    std::scoped_lock lock(m_mutexResource);

    uint32_t hash = (mipOffset << 16) | mipLevels;

    auto it = m_resourceData.find(resource);
    if (it == m_resourceData.end() || (*it).second.find(hash) == (*it).second.end())
    {
        auto node = 0; // FIX THIS
        data.descIndex = getNewAndIncreaseDescIndex();
        auto currentCPUHandle =  CD3DX12_CPU_DESCRIPTOR_HANDLE(m_heap->descriptorHeap[node]->GetCPUDescriptorHandleForHeapStart(), data.descIndex, m_descriptorSize);

        D3D12_RESOURCE_DESC desc = resource->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Format = getCorrectFormat(desc.Format);
        SRVDesc.Texture2D.MipLevels = mipLevels ? mipLevels : desc.MipLevels;
        SRVDesc.Texture2D.MostDetailedMip = mipOffset;

        auto name = getDebugName(res);
        SL_LOG_VERBOSE("Caching texture 0x%llx(%S) node %u fmt %s size (%u,%u) mip %u mips %u sampler[%d]", resource, name.c_str(), node, getDXGIFormatStr(desc.Format), (UINT)desc.Width, (UINT)desc.Height, SRVDesc.Texture2D.MostDetailedMip, SRVDesc.Texture2D.MipLevels, sampler);

        m_device->CreateShaderResourceView(resource, &SRVDesc, currentCPUHandle);

        data.heap = m_heap;

        m_resourceData[resource][hash] = data;
    }
    else
    {
        data = (*it).second[hash];
    }
    assert(data.heap == m_heap);
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getSurfaceDriverData(Resource res, ResourceDriverData &data, uint32_t mipOffset)
{
    if (!res || !res->native) return ComputeStatus::eInvalidArgument;

    ID3D12Resource* resource = (ID3D12Resource*)(res->native);

    std::scoped_lock lock(m_mutexResource);

    uint32_t hash = mipOffset << 16;

    auto it = m_resourceData.find(resource);
    if (it == m_resourceData.end() || (*it).second.find(hash) == (*it).second.end())
    {
        auto node = 0; // FIX THIS
        data.descIndex = getNewAndIncreaseDescIndex();

        D3D12_RESOURCE_DESC desc = resource->GetDesc();

        auto name = getDebugName(res);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            UAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            UAVDesc.Buffer.CounterOffsetInBytes = 0;
            UAVDesc.Buffer.FirstElement = 0;
            UAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            UAVDesc.Buffer.NumElements = (UINT)desc.Width / 4;
            UAVDesc.Buffer.StructureByteStride = 0;

            SL_LOG_VERBOSE("Caching raw buffer 0x%llx(%S) node %u fmt %s size (%u,%u)", resource, name.c_str(), node, getDXGIFormatStr(desc.Format), (UINT)desc.Width, (UINT)desc.Height);
        }
        else
        {
            UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            UAVDesc.Format = getCorrectFormat(desc.Format);
            UAVDesc.Texture2D.MipSlice = mipOffset;

            if (!isSupportedFormat(UAVDesc.Format, 0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            {
                SL_LOG_ERROR( "Format %s cannot be used as UAV", getDXGIFormatStr(UAVDesc.Format));
                return ComputeStatus::eError;
            }

            SL_LOG_VERBOSE("Caching rwtexture 0x%llx(%S) node %u fmt %s size (%u,%u) mip %u", resource, name.c_str(), node, getDXGIFormatStr(desc.Format), (UINT)desc.Width, (UINT)desc.Height, UAVDesc.Texture2D.MipSlice);
        }
        
        {
            auto cpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_heap->descriptorHeap[node]->GetCPUDescriptorHandleForHeapStart(), data.descIndex, m_descriptorSize);
            m_device->CreateUnorderedAccessView(resource, nullptr, &UAVDesc, cpuHandle);
        }

        data.heap = m_heap;

        m_resourceData[resource][hash] = data;
    }
    else
    {
        data = (*it).second[hash];
    }
    assert(data.heap == m_heap);

    return ComputeStatus::eOk;
}

bool D3D12::isSupportedFormat(DXGI_FORMAT format, int flag1, int flag2)
{
    // Make sure all typeless formats are converted before the check is done
    D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport = { getCorrectFormat(format), D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
    HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport, sizeof(FormatSupport));
    if (SUCCEEDED(hr))
    {
        return (FormatSupport.Support1 & flag1) != 0 || (FormatSupport.Support2 & flag2) != 0;
    }
    SL_LOG_ERROR( "Format %s is unsupported - hres %lu flags %d %d", getDXGIFormatStr(format), hr, flag1, flag2);
    return false;
}

ComputeStatus D3D12::createTexture2DResourceSharedImpl(ResourceDescription &resourceDesc, Resource &outResource, bool useNativeFormat, ResourceState initialState, const char InFriendlyName[])
{
    ID3D12Resource *res = nullptr;
    D3D12_HEAP_TYPE NativeHeapType;
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Alignment = 65536;
    texDesc.MipLevels = (UINT16)resourceDesc.mips;
    if (useNativeFormat)
    {
        assert(resourceDesc.nativeFormat != NativeFormatUnknown);
        texDesc.Format = (DXGI_FORMAT)resourceDesc.nativeFormat;
    }
    else
    {
        assert(resourceDesc.format != eFormatINVALID);
        NativeFormat native;
        getNativeFormat(resourceDesc.format, native);
        resourceDesc.nativeFormat = native;
        texDesc.Format = (DXGI_FORMAT)native;
    }
    texDesc.Width = resourceDesc.width;
    texDesc.Height = resourceDesc.height;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    switch(resourceDesc.heapType)
    {
        case eHeapTypeReadback:
            NativeHeapType = D3D12_HEAP_TYPE_READBACK;
            break;
        case eHeapTypeUpload:
            NativeHeapType = D3D12_HEAP_TYPE_UPLOAD;
            break;
        case eHeapTypeDefault:
            NativeHeapType = D3D12_HEAP_TYPE_DEFAULT;
            break;
    }
    texDesc.DepthOrArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (isSupportedFormat(texDesc.Format, 0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
    {
        texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    else
    {
        initialState &= ~ResourceState::eStorageRW;
    }
    if (isSupportedFormat(texDesc.Format, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, 0))
    {
        texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    else
    {
        initialState &= ~(ResourceState::eColorAttachmentRead | ResourceState::eColorAttachmentWrite);
    }
    if (isSupportedFormat(texDesc.Format, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL, 0))
    {
        texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    else
    {
        initialState &= ~(ResourceState::eDepthStencilAttachmentRead | ResourceState::eDepthStencilAttachmentWrite);
    }

    if (resourceDesc.flags & ResourceFlags::eSharedResource)
    {
        texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    }

    D3D12_RESOURCE_STATES nativeInitialState = toD3D12States(initialState);

    CD3DX12_HEAP_PROPERTIES heapProp(NativeHeapType, resourceDesc.creationMask, resourceDesc.visibilityMask ? resourceDesc.visibilityMask : m_visibleNodeMask);

    if (m_allocateCallback)
    {
        ResourceAllocationDesc desc = { ResourceType::eTex2d, &texDesc, (uint32_t)nativeInitialState, &heapProp };
        auto result = m_allocateCallback(&desc, m_device);
        res = (ID3D12Resource*)result.native;
    }
    else
    {
        auto hr = m_device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &texDesc, nativeInitialState, nullptr, IID_PPV_ARGS(&res));
        if (FAILED(hr))
        {
            SL_LOG_ERROR( "CreateCommittedResource failed %s", std::system_category().message(hr).c_str());
        }
    }

    if (!res)
    {
        SL_LOG_ERROR( " CreateCommittedResource failed");
        return ComputeStatus::eError;
    }
    
    outResource = new sl::Resource(ResourceType::eTex2d, res);

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::createBufferResourceImpl(ResourceDescription &resourceDesc, Resource &outResource, ResourceState initialState, const char InFriendlyName[])
{
    ID3D12Resource *res = nullptr;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.Width = resourceDesc.width;
    assert(resourceDesc.height == 1);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.SampleDesc.Quality = 0;
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    switch (resourceDesc.heapType)
    {
        default: assert(0); // Fall through
        case eHeapTypeDefault:
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            break;
        case eHeapTypeUpload:
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
            break;
        case eHeapTypeReadback:
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
            break;
    }

    D3D12_HEAP_TYPE NativeHeapType = (D3D12_HEAP_TYPE) resourceDesc.heapType; // TODO : proper conversion !

    // Buffers must always be created in Common state
    D3D12_RESOURCE_STATES NativeInitialState = D3D12_RESOURCE_STATE_COMMON;

    CD3DX12_HEAP_PROPERTIES heapProp(NativeHeapType, resourceDesc.creationMask, resourceDesc.visibilityMask ? resourceDesc.visibilityMask : m_visibleNodeMask);

    if (m_allocateCallback)
    {
        ResourceAllocationDesc desc = { ResourceType::eBuffer, &bufferDesc, (uint32_t)NativeInitialState, &heapProp };
        auto result = m_allocateCallback(&desc, m_device);
        res = (ID3D12Resource*)result.native;
    }
    else
    {
        m_device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufferDesc, NativeInitialState, nullptr, IID_PPV_ARGS(&res));
    }

    outResource = new sl::Resource(ResourceType::eBuffer, res, (uint32_t)NativeInitialState);
    if (!outResource)
    {
        SL_LOG_ERROR( " CreateCommittedResource failed");
        return ComputeStatus::eError;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::setDebugName(Resource res, const char name[])
{
#ifdef SL_DEBUG
    ID3D12Pageable *resource = (ID3D12Pageable*)(res->native);
    resource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
#endif
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::copyHostToDeviceBuffer(CommandList InCmdList, uint64_t InSize, const void *InData, Resource InUploadResource, Resource InTargetResource, unsigned long long InUploadOffset, unsigned long long InDstOffset)
{
    UINT8 *StagingPtr = nullptr;

    ID3D12Resource *Resource = (ID3D12Resource*)(InTargetResource->native);
    ID3D12Resource *Scratch = (ID3D12Resource*)(InUploadResource->native);

    //SL_LOG_INFO("Scratch size %llu", Scratch->GetDesc().Width * Scratch->GetDesc().Height);

    HRESULT hr = Scratch->Map(0, NULL, reinterpret_cast<void**>(&StagingPtr));
    if (hr != S_OK)
    {
        SL_LOG_ERROR( " failed to map buffer - error %s", std::system_category().message(hr).c_str());
        return ComputeStatus::eError;
    }

    memcpy(StagingPtr + InUploadOffset, InData, InSize);

    Scratch->Unmap(0, nullptr);

    ((ID3D12GraphicsCommandList*)InCmdList)->CopyBufferRegion(Resource, InDstOffset, Scratch, InUploadOffset, InSize);

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::copyHostToDeviceTexture(CommandList cmdList, uint64_t InSize, uint64_t RowPitch, const void* InData, Resource InTargetResource, Resource& InUploadResource)
{
    if (!cmdList || !InData || !InTargetResource)
    {
        return ComputeStatus::eInvalidArgument;
    }

    uint64_t depthPitch = 1;
    uint64_t mipLevel = 0;
    uint64_t arraySlice = 0;
    ID3D12Resource* dest = (ID3D12Resource*)(InTargetResource->native);
    D3D12_RESOURCE_DESC resourceDesc = dest->GetDesc();

    uint32_t subresource = calcSubresource((uint32_t)mipLevel, (uint32_t)arraySlice, 0, (uint32_t)resourceDesc.MipLevels, resourceDesc.DepthOrArraySize);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    uint32_t numRows;
    uint64_t rowSizeInBytes;
    uint64_t totalBytes;

    m_device->GetCopyableFootprints(&resourceDesc, subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
    assert(numRows <= footprint.Footprint.Height);

    ID3D12Resource* uploadBuffer = (ID3D12Resource*)(InUploadResource->native);
    void* cpuVA = nullptr;
    uploadBuffer->Map(0, nullptr, &cpuVA);
    memcpy(cpuVA, InData, InSize);
    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION destCopyLocation = {};
    destCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destCopyLocation.SubresourceIndex = subresource;
    destCopyLocation.pResource = dest;

    D3D12_TEXTURE_COPY_LOCATION srcCopyLocation = {};
    srcCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcCopyLocation.PlacedFootprint = footprint;
    srcCopyLocation.pResource = uploadBuffer;

    ((ID3D12GraphicsCommandList*)cmdList)->CopyTextureRegion(&destCopyLocation, 0, 0, 0, &srcCopyLocation, nullptr);
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::copyDeviceTextureToDeviceBuffer(CommandList cmdList, Resource srcTexture, Resource dstBuffer)
{
    if (!cmdList || !srcTexture || !dstBuffer)
    {
        return ComputeStatus::eInvalidArgument;
    }
    ID3D12Resource* tex = (ID3D12Resource*)(srcTexture->native);
    D3D12_RESOURCE_DESC resourceDesc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    m_device->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);

    D3D12_TEXTURE_COPY_LOCATION srcCopyLocation = {};
    srcCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcCopyLocation.SubresourceIndex = 0;
    srcCopyLocation.pResource = (ID3D12Resource*)(srcTexture->native);

    D3D12_TEXTURE_COPY_LOCATION destCopyLocation = {};
    destCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destCopyLocation.PlacedFootprint = footprint;
    destCopyLocation.pResource = (ID3D12Resource*)(dstBuffer->native);

    ((ID3D12GraphicsCommandList*)cmdList)->CopyTextureRegion(&destCopyLocation, 0, 0, 0, &srcCopyLocation, nullptr);
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::clearView(CommandList InCmdList, Resource resource, const float4 Color, const RECT * pRects, uint32_t NumRects, CLEAR_TYPE &outType)
{
    outType = CLEAR_UNDEFINED;
    
    ResourceDriverData Data = {};
    if (getSurfaceDriverData(resource, Data) == ComputeStatus::eOk)
    {
        auto node = 0; // FIX THIS
        CD3DX12_CPU_DESCRIPTOR_HANDLE CPUVisibleCPUHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_heap->descriptorHeapCPU[node]->GetCPUDescriptorHandleForHeapStart(), Data.descIndex, m_descriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE GPUHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_heap->descriptorHeap[node]->GetGPUDescriptorHandleForHeapStart(), Data.descIndex, m_descriptorSize);

        ((ID3D12GraphicsCommandList*)InCmdList)->ClearUnorderedAccessViewFloat(GPUHandle, CPUVisibleCPUHandle, (ID3D12Resource*)(resource->native), (const FLOAT*)&Color, NumRects, pRects);

        outType = Data.bZBCSupported ? CLEAR_ZBC_WITH_PADDING : CLEAR_NON_ZBC;
        return ComputeStatus::eOk;
    }
    return ComputeStatus::eError;
}

ComputeStatus D3D12::insertGPUBarrierList(CommandList InCmdList, const Resource* resources, uint32_t resourceCount, BarrierType barrierType)
{
    if (barrierType == BarrierType::eBarrierTypeUAV)
    {
        std::vector< D3D12_RESOURCE_BARRIER> Barriers;
        for (uint32_t i = 0; i < resourceCount; i++)
        {
            const Resource& res = resources[i];
            Barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV((ID3D12Resource*)(res->native)));
        }
        ((ID3D12GraphicsCommandList*)InCmdList)->ResourceBarrier((UINT)Barriers.size(), Barriers.data());
    }
    else
    {
        assert(false);
        return ComputeStatus::eNotSupported;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::insertGPUBarrier(CommandList InCmdList, Resource InResource, BarrierType InBarrierType)
{
    if (InBarrierType == BarrierType::eBarrierTypeUAV)
    {
        D3D12_RESOURCE_BARRIER UAV = CD3DX12_RESOURCE_BARRIER::UAV((ID3D12Resource*)(InResource->native));
        ((ID3D12GraphicsCommandList*)InCmdList)->ResourceBarrier(1, &UAV);
    }
    else
    {
        assert(false);
        return ComputeStatus::eNotSupported;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::transitionResourceImpl(CommandList cmdList, const ResourceTransition *transitions, uint32_t count)
{
    if (!cmdList || !transitions)
    {
        return ComputeStatus::eInvalidArgument;
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (uint32_t i = 0; i < count; i++)
    {
        if (transitions[i].from != transitions[i].to)
        {
            auto from = toD3D12States(transitions[i].from);
            auto to = toD3D12States(transitions[i].to);
            
            // Separate streamline resource states can map to the same D3D12 state which causes an error
            // eGeneral and ePresent both map to common for D3D12.
            if (from != to)
            {
                //SL_LOG_HINT("transitioning %S %u->%u", getDebugName(transitions[i].resource).c_str(), from, to);
                transitions[i].resource->state = to;
                barriers.push_back({ CD3DX12_RESOURCE_BARRIER::Transition((ID3D12Resource*)(transitions[i].resource->native), from, to, transitions[i].subresource) });
            }
        }
    }
    ((ID3D12GraphicsCommandList*)cmdList)->ResourceBarrier((uint32_t)barriers.size(), barriers.data());
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::copyResource(CommandList InCmdList, Resource InDstResource, Resource InSrcResource)
{
    if (!InCmdList || !InDstResource || !InSrcResource) return ComputeStatus::eInvalidArgument;
    ((ID3D12GraphicsCommandList*)InCmdList)->CopyResource((ID3D12Resource*)(InDstResource->native), (ID3D12Resource*)(InSrcResource->native));
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::cloneResource(Resource resource, Resource &clone, const char friendlyName[], ResourceState initialState, uint32_t creationMask, uint32_t visibilityMask)
{
    if (!resource || !resource->native) return ComputeStatus::eInvalidArgument;

    D3D12_RESOURCE_DESC desc1 = ((ID3D12Resource*)(resource->native))->GetDesc();
    ID3D12Resource *res = nullptr;
        
    CD3DX12_HEAP_PROPERTIES heapProp(D3D12_HEAP_TYPE_DEFAULT, creationMask, visibilityMask ? visibilityMask : m_visibleNodeMask);

    if (isSupportedFormat(desc1.Format, 0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))    
    {
        desc1.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    else
    {
        desc1.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        initialState &= ~ResourceState::eStorageRW;
    }
    if (isSupportedFormat(desc1.Format, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, 0))
    {
        desc1.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    else
    {
        desc1.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        initialState &= ~(ResourceState::eColorAttachmentRead | ResourceState::eColorAttachmentWrite);
    }

    // Depth-stencil is only allowed if resource is not already UAV or RTV
    bool depthStencilAllowed = (desc1.Flags & (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) == 0;

    if (depthStencilAllowed && isSupportedFormat(desc1.Format, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL, 0))
    {
        desc1.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    else
    {
        desc1.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        initialState &= ~(ResourceState::eDepthStencilAttachmentRead | ResourceState::eDepthStencilAttachmentWrite);
    }

    auto type = desc1.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? ResourceType::eBuffer : ResourceType::eTex2d;

    uint32_t nativeState;
    getNativeResourceState(initialState, nativeState);

    if (m_allocateCallback)
    {
        ResourceAllocationDesc desc = { type, &desc1, (uint32_t)nativeState, &heapProp };
        auto result = m_allocateCallback(&desc, m_device);
        res = (ID3D12Resource*)result.native;
    }
    else
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT, creationMask, visibilityMask);
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProp,
            D3D12_HEAP_FLAG_NONE,
            &desc1,
            (D3D12_RESOURCE_STATES)nativeState,
            nullptr,
            IID_PPV_ARGS(&res));

        if (FAILED(hr))
        {
            SL_LOG_ERROR( "Unable to clone resource (%s:%u:%u:%s) - %s", friendlyName, desc1.Width, desc1.Height, getDXGIFormatStr(desc1.Format), std::system_category().message(hr).c_str());
            return ComputeStatus::eError;
        }
    }

    if (!res)
    {
        SL_LOG_ERROR("Unable to clone resource (%s:%u:%u:%s)", friendlyName, desc1.Width, desc1.Height, getDXGIFormatStr(desc1.Format));
        return ComputeStatus::eError;
    }

    clone = new sl::Resource(type, res, nativeState);
    clone->flags = desc1.Flags;
    clone->mipLevels = desc1.MipLevels;
    clone->arrayLayers = desc1.DepthOrArraySize;
    clone->nativeFormat = desc1.Format;
    clone->width = (uint32_t)desc1.Width;
    clone->height = desc1.Height;

    setDebugName(clone, friendlyName);

    manageVRAM(clone, VRAMOperation::eAlloc);
    
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::copyBufferToReadbackBuffer(CommandList InCmdList, Resource InResource, Resource OutResource, uint32_t InBytesToCopy) 
{
    ID3D12Resource *InD3dResource = (ID3D12Resource*)(InResource->native);
    ID3D12Resource *OutD3dResource = (ID3D12Resource*)(OutResource->native);
    ID3D12GraphicsCommandList* CmdList = (ID3D12GraphicsCommandList*)InCmdList;

#if 0
    ResourceDescription ResourceDesc;
    GetResourceDescription(OutResource, ResourceDesc);
    const size_t readbackBufferSize = ResourceDesc.Width;

    D3D12_RANGE readbackBufferRange{ 0, readbackBufferSize };
    FLOAT * pReadbackBufferData{};
    OutD3dResource->Map(0, &readbackBufferRange, reinterpret_cast<void**>(&pReadbackBufferData));

    memset(pReadbackBufferData, 0xAA, readbackBufferSize);

    D3D12_RANGE emptyRange{ 0, 0 };
    OutD3dResource->Unmap(0, &emptyRange);
#endif

    CmdList->CopyBufferRegion(OutD3dResource, 0, InD3dResource, 0, InBytesToCopy);

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::mapResource(CommandList cmdList, Resource resource, void*& data, uint32_t subResource, uint64_t offset, uint64_t totalBytes)
{
    auto src = (ID3D12Resource*)(resource->native);
    if (!src) return ComputeStatus::eInvalidPointer;

    void* mapped{};
    D3D12_RANGE range = { offset, offset + totalBytes };
    if (FAILED(src->Map(subResource, &range, &mapped)))
    {
        SL_LOG_ERROR( "Failed to map buffer");
        return ComputeStatus::eError;
    }
    data = mapped;
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::unmapResource(CommandList cmdList, Resource resource, uint32_t subResource)
{
    auto src = (ID3D12Resource*)(resource->native);
    if (!src) return ComputeStatus::eInvalidPointer;

    src->Unmap(0, nullptr);
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getLUIDFromDevice(NVSDK_NGX_LUID *OutId)
{
    auto id = m_device->GetAdapterLuid();
    memcpy(OutId, &id, sizeof(LUID));
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::beginPerfSection(CommandList cmdList, const char *key, uint32_t node, bool reset)
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
        for (int i = 0; i < SL_READBACK_QUEUE_SIZE; i++)
        {
            data->reset[i] = true;
        }
        data->meter.reset();
    }
    
    if (!data->queryHeap[data->queryIdx])
    {
        D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
        queryHeapDesc.Count = 2;
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.NodeMask = (1 << node);
        m_device->CreateQueryHeap(&queryHeapDesc, __uuidof(ID3D12QueryHeap), (void **)&data->queryHeap[data->queryIdx]);

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.Width = 2 * sizeof(uint64_t);
        bufferDesc.Height = 1;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK, queryHeapDesc.NodeMask, queryHeapDesc.NodeMask);
        m_device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufferDesc, initialState, nullptr, IID_PPV_ARGS(&data->queryBufferReadback[data->queryIdx]));
        D3D12_RANGE mapRange = { 0, sizeof(uint64_t) * 2 };
        //! Map in advance to increase performance, no need to map/unmap every frame
        data->queryBufferReadback[data->queryIdx]->Map(0, &mapRange, reinterpret_cast<void**>(&data->pStagingPtr));
    }
    else
    {
        ((ID3D12GraphicsCommandList*)cmdList)->ResolveQueryData(data->queryHeap[data->queryIdx], D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, data->queryBufferReadback[data->queryIdx], 0);
        uint64_t dxNanoSecondTS[2];
        if (data->pStagingPtr)
        {
            dxNanoSecondTS[0] = ((uint64_t*)data->pStagingPtr)[0];
            dxNanoSecondTS[1] = ((uint64_t*)data->pStagingPtr)[1];
            double delta = (dxNanoSecondTS[1] - dxNanoSecondTS[0]) / 1e06;
            if (!data->reset[data->queryIdx])
            {
                if (delta > 0)
                {
                    data->meter.add(delta);
                }
            }
            else
            {
                data->meter.reset();
            }
        }
        else
        {
            data->reset[data->queryIdx] = false;
        }
    }

    ((ID3D12GraphicsCommandList*)cmdList)->EndQuery(data->queryHeap[data->queryIdx], D3D12_QUERY_TYPE_TIMESTAMP, 0);
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::endPerfSection(CommandList cmdList, const char* key, float &avgTimeMS, uint32_t node)
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
    ((ID3D12GraphicsCommandList*)cmdList)->EndQuery(data->queryHeap[data->queryIdx], D3D12_QUERY_TYPE_TIMESTAMP, 1);
    data->queryIdx = (data->queryIdx + 1) % SL_READBACK_QUEUE_SIZE;

    avgTimeMS = (float)data->meter.getMean();
    return ComputeStatus::eOk;
}


ComputeStatus D3D12::beginProfiling(CommandList cmdList, uint32_t metadata, const char* marker)
{
#if SL_ENABLE_PROFILING
    PIXBeginEvent(((ID3D12GraphicsCommandList*)cmdList), metadata, marker);
#endif    
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::endProfiling(CommandList cmdList)
{
#if SL_ENABLE_PROFILING
    PIXEndEvent(((ID3D12GraphicsCommandList*)cmdList));
#endif
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::beginProfilingQueue(CommandQueue cmdQueue, uint32_t metadata, const char* marker)
{
#if SL_ENABLE_PROFILING
    PIXBeginEvent(((ID3D12CommandQueue*)cmdQueue), metadata, marker);
#endif    
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::endProfilingQueue(CommandQueue cmdQueue)
{
#if SL_ENABLE_PROFILING
    PIXEndEvent(((ID3D12CommandQueue*)cmdQueue));
#endif
    return ComputeStatus::eOk;
}

bool D3D12::signalCPUFence(Fence fence, uint64_t syncValue)
{
    auto d3d12Fence = ((ID3D12Fence*)fence);
    if (FAILED(d3d12Fence->Signal(syncValue)))
    {
        SL_LOG_ERROR("Failed to signal fence from CPU");
        return false;
    }
    return true;
}

int D3D12::destroyResourceDeferredImpl(const Resource resource)
{   
    auto it = m_resourceData.find(resource->native);
    if (it != m_resourceData.end())
    {
        m_resourceData.erase(it);
    }
    auto unknown = (IUnknown*)(resource->native);
    return unknown->Release();
}

DXGI_FORMAT D3D12::getCorrectFormat(DXGI_FORMAT Format)
{
    switch (Format)
    {
    case DXGI_FORMAT_D16_UNORM: // casting from non typeless is supported from RS2+
        assert(m_dbgSupportRs2RelaxedConversionRules);
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT: // casting from non typeless is supported from RS2+
        assert(m_dbgSupportRs2RelaxedConversionRules); // fallthrough
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
        assert(m_dbgSupportRs2RelaxedConversionRules); // fallthrough
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: // casting from non typeless is supported from RS2+
        assert(m_dbgSupportRs2RelaxedConversionRules); // fallthrough
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return Format;
    };
}

// {694B3E1C-0E33-416F-BA83-FE248DA1E85D}
static const GUID sResourceStateGUID = { 0x694b3e1c, 0xe33, 0x416f, { 0xba, 0x83, 0xfe, 0x24, 0x8d, 0xa1, 0xe8, 0x5d } };

ComputeStatus D3D12::getResourceState(Resource resource, ResourceState& state)
{
    state = ResourceState::eUnknown;
    if(!resource) return ComputeStatus::eOk;
    return getResourceState(resource->state, state);
}

ComputeStatus D3D12::getResourceFootprint(Resource resource, ResourceFootprint& footprint)
{
    if (!resource || !resource->native) return ComputeStatus::eInvalidArgument;

    ID3D12Resource* res = (ID3D12Resource*)(resource->native);
    D3D12_RESOURCE_DESC resourceDesc = res->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpnt = {};
    uint32_t numRows;
    uint64_t rowSizeInBytes;
    uint64_t totalBytes;

    m_device->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &fpnt, &numRows, &rowSizeInBytes, &totalBytes);
    
    footprint.depth = fpnt.Footprint.Depth;
    footprint.width = fpnt.Footprint.Width;
    footprint.height = fpnt.Footprint.Height;
    footprint.offset = fpnt.Offset;
    footprint.rowPitch = fpnt.Footprint.RowPitch;
    footprint.numRows = numRows;
    footprint.rowSizeInBytes = rowSizeInBytes;
    footprint.totalBytes = totalBytes;
    getFormat(fpnt.Footprint.Format, footprint.format);

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getResourceDescription(Resource resource, ResourceDescription& outDesc)
{
    if (!resource || !resource->native) return ComputeStatus::eInvalidArgument;

    if (resource->type == ResourceType::eFence)
    {
        // Fences are always shared with d3d12 so report back
        outDesc.flags |= ResourceFlags::eSharedResource;
        return ComputeStatus::eOk;
    }

    // First make sure this is not an DXGI or some other resource
    Microsoft::WRL::ComPtr<IUnknown> unknown = (IUnknown*)(resource->native);
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12Resource;
    unknown.As(&d3d12Resource);
    if (!d3d12Resource)
    {
        return ComputeStatus::eError;
    }

    D3D12_RESOURCE_DESC desc = d3d12Resource->GetDesc();
    getFormat(desc.Format, outDesc.format);
    outDesc.width = (UINT)desc.Width;
    outDesc.height = desc.Height;
    outDesc.nativeFormat = desc.Format;
    outDesc.mips = desc.MipLevels;
    outDesc.depth = desc.DepthOrArraySize;

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        outDesc.gpuVirtualAddress = d3d12Resource->GetGPUVirtualAddress();
        outDesc.flags |= ResourceFlags::eRawOrStructuredBuffer | ResourceFlags::eConstantBuffer;
    }
    else
    {
        outDesc.flags |= ResourceFlags::eShaderResource;
    }
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        outDesc.flags |= ResourceFlags::eShaderResourceStorage;
    }
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
    {
        outDesc.flags |= ResourceFlags::eSharedResource;
    }
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    {
        outDesc.flags |= ResourceFlags::eDepthStencilAttachment;
    }
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    {
        outDesc.flags |= ResourceFlags::eColorAttachment;
    }

    return ComputeStatus::eOk;
}

ComputeStatus D3D12::notifyOutOfBandCommandQueue(CommandQueue queue, OutOfBandCommandQueueType type)
{
    NVAPI_CHECK(NvAPI_D3D12_NotifyOutOfBandCommandQueue((ID3D12CommandQueue*)queue, (NV_OUT_OF_BAND_CQ_TYPE) type));
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::setAsyncFrameMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId)
{
    NV_ASYNC_FRAME_MARKER_PARAMS_V1 params = { 0 };
    params.version = NV_ASYNC_FRAME_MARKER_PARAMS_VER1;
    params.frameID = frameId;
    params.markerType = (NV_LATENCY_MARKER_TYPE)marker;

    NVAPI_CHECK(NvAPI_D3D12_SetAsyncFrameMarker((ID3D12CommandQueue*)queue, &params));
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::setLatencyMarker(CommandQueue queue, PCLMarker marker, uint64_t frameId)
{
    NV_LATENCY_MARKER_PARAMS_V1 params = { 0 };
    params.version = NV_LATENCY_MARKER_PARAMS_VER1;
    params.frameID = frameId;
    params.markerType = (NV_LATENCY_MARKER_TYPE)marker;

    NVAPI_CHECK(NvAPI_D3D_SetLatencyMarker((ID3D12CommandQueue*)queue, &params));
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::createSharedHandle(Resource res, Handle& outHandle)
{
    auto resource = (ID3D12Resource*)(res->native);
    HANDLE handle;
    if (FAILED(m_device->CreateSharedHandle(resource, NULL, GENERIC_ALL,nullptr, &handle)))
    {
        SL_LOG_ERROR( "Failed to create shared handle");
        assert(false);
        return ComputeStatus::eError;
    }
    outHandle = handle;
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::destroySharedHandle(Handle& handle)
{
    if (!CloseHandle(handle))
    {
        SL_LOG_ERROR( "Failed to close shared handle");
        return ComputeStatus::eError;
    }
    return ComputeStatus::eOk;
}

ComputeStatus D3D12::getResourceFromSharedHandle(ResourceType type, Handle handle, Resource& resource)
{
    if (type == ResourceType::eTex2d)
    {
        ID3D12Resource* tex{};
        if (FAILED(m_device->OpenSharedHandle((HANDLE)handle, __uuidof(ID3D12Resource), (void**)&tex)))
        {
            SL_LOG_ERROR( "Failed to open shared handle");
            assert(false);
            return ComputeStatus::eError;
        }
        resource = new sl::Resource(ResourceType::eTex2d, tex);
        setDebugName(resource, "sl.shared.from.d3d11");
        // We free these buffers but never allocate them so account for the VRAM
        manageVRAM(resource, VRAMOperation::eAlloc);
    }
    else if (type == ResourceType::eFence)
    {
        ID3D12Fence* fence{};
        if (FAILED(m_device->OpenSharedHandle((HANDLE)handle, __uuidof(ID3D12Fence), (void**)&fence)))
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

}
}
