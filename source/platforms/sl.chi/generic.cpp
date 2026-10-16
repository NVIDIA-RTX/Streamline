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

#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#define __STDC_FORMAT_MACROS 1
#include <cinttypes>
#include <utility>
#include <string.h>
#include <fstream>
#include <map>
#include <unordered_set>
#include <vector>

struct IDXGIAdapter;
struct IDXGISwapChain;

#include "include/sl_helpers.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.extra/extra.h"
#include "source/core/sl.param/parameters.h"
#include "source/platforms/sl.chi/generic.h"
#include "source/core/sl.security/secureLoadLibrary.h"
#include "source/platforms/sl.chi/nvapiCompat.h"
#include "external/nsight-sdk/SystemsGraphics/include/Impl/NGFX_Core.h"
#include "nvapi.h"

// {B5504F36-CB88-4B2D-AE64-9CAE29E23CA9}
static const GUID sResourceTrackGUID = { 0xb5504f36, 0xcb88, 0x4b2d, { 0xae, 0x64, 0x9c, 0xae, 0x29, 0xe2, 0x3c, 0xa9 } };

const char *GFORMAT_STR[] = {
    "eFormatINVALID",
    "eFormatRGBA32F",
    "eFormatRGBA16F",
    "eFormatRGB32F", // Pseudo format (for typeless buffers), not supported natively by d3d/vulkan
    "eFormatRGB16F", // Pseudo format (for typeless buffers), not supported natively by d3d/vulkan
    "eFormatRG16F",
    "eFormatR16F",
    "eFormatRG32F",
    "eFormatR32F",
    "eFormatR8UN",
    "eFormatRG8UN",
    "eFormatRGB11F",
    "eFormatRGBA8UN",
    "eFormatSRGBA8UN",
    "eFormatBGRA8UN",
    "eFormatSBGRA8UN",
    "eFormatRG16UI",
    "eFormatRG16SI",
    "eFormatE5M3",
    "eFormatRGB10A2UN",
    "eFormatR8UI",
    "eFormatR16UI",
    "eFormatRG16UN",
    "eFormatR32UI",
    "eFormatRG32UI",
    "eFormatD32S32",
    "eFormatD24S8",
    "eFormatD32S8U",
    "eFormatRGBA8SN",
}; static_assert(countof(GFORMAT_STR) == sl::chi::eFormatCOUNT, "Not enough strings for eFormatCOUNT");

#define SL_TEXT_BUFFER_SIZE 16384

namespace sl
{

namespace chi
{

namespace
{
void* nsightSecureLoadLibraryCallback(const wchar_t* libName)
{
    auto handle{ static_cast<void*>(sl::security::loadLibrary(libName)) };
    if (!handle)
    {
        SL_LOG_VERBOSE("NSight library load failed: %S", libName);
    }
    return handle;
}
}

void Generic::initNsightActivity()
{
    NGFX_SetLibraryLoadFn(nsightSecureLoadLibraryCallback);

    NGFX_ActivityType activity{};
    const NGFX_Result detected{ NGFX_GetInjectedActivity(&activity) };
    if (detected != NGFX_Result_Success)
    {
        SL_LOG_VERBOSE("NSight SDK activity not detected (%d)!", detected);
        return;
    }

    if (!initNsightActivityImpl(activity))
    {
        SL_LOG_WARN("NSight SDK activity initialization failed (%d)!", activity);
        return;
    }

    m_nsightInitialized = true;
    SL_LOG_INFO("NSight SDK activity initialized successfully!");
}

ScopedProfilingSection::ScopedProfilingSection(ICompute* compute, CommandList cmdList, const char* function, sl::Feature feature)
    : ScopedProfilingSection(compute, cmdList)
{
#if SL_ENABLE_PROFILING
    std::stringstream str;
    str << function << " " << getFeatureAsStr(feature);
    m_compute->beginProfiling(m_cmdList, str.str().c_str());
#endif
}

 ScopedProfilingSection::ScopedProfilingSection(ICompute* compute, CommandList cmdList, const char* function, const sl::ResourceTag* resources, uint32_t numResources) : ScopedProfilingSection(compute, cmdList)
{
#if SL_ENABLE_PROFILING
    std::string marker;

    std::stringstream str;
    str << function << " ";

    for (uint32_t i = 0; i < numResources; ++i)
    {
        str << getBufferTypeAsStr(resources[i].type);
        
        if (i + 1 != numResources)
        {
            str << ", ";
        }
    }
    m_compute->beginProfiling(m_cmdList, str.str().c_str());
    #endif
}


#define SL_DEBUG_RESOURCE_POOL 0

struct ResourcePool : IResourcePool
{
    using TimestampedResource = std::pair<std::chrono::system_clock::time_point, HashedResource>;

    ResourcePool(ICompute* compute, const char* vramSegment) : m_compute(compute), m_vramSegment(vramSegment) {};

    virtual void setMaxQueueSize(size_t) override final
    {
    }

    virtual HashedResource allocate(Resource source, const char* debugName, ResourceState initialState) override final
    {
        ResourceDescription desc;
        m_compute->getResourceDescription(source, desc);
        desc.state = initialState;
        auto hash = getHash(desc);
        std::unique_lock<std::mutex> lock(m_mtx);
        // Look for a free one to recycle
        HashedResource resource{};
        auto& freeItems = m_free[hash];
        if (!freeItems.empty())
        {
            resource = freeItems.back().second;
            freeItems.pop_back();
            m_compute->getResourceState((Resource)resource, resource.accessState());
            m_allocated[hash].push_back({ std::chrono::system_clock::now(), resource });
            return resource;
        }
        // No free items available, allocate a new resource.
        // This is safe because all GPU work using previously recycled resources
        // was submitted on the same command queue earlier, so it is guaranteed
        // to complete before any new work referencing the newly allocated resource.
        // collectGarbage() will reclaim unused pool entries to keep VRAM in check.
        m_compute->beginVRAMSegment(m_vramSegment.c_str());
        Resource res{};
        auto status = m_compute->cloneResource(source, res, debugName, initialState);
        m_compute->endVRAMSegment();
        if (status != ComputeStatus::eOk || !res)
        {
            SL_LOG_ERROR("Failed to clone resource '%s' for hash %llu", debugName, hash);
            return HashedResource{};
        }
        m_compute->getResourceState(res->state, initialState);
        resource = HashedResource(hash, initialState, res, m_compute, true);
#if SL_DEBUG_RESOURCE_POOL
        for (auto& [timestamp, cached] : m_allocated[hash])
        {
            assert(res != cached.resource);
        }
        SL_LOG_VERBOSE("alloc - hash %llu 0x%p '%s' [%llu,%llu]\n", hash, ((Resource)resource)->native, debugName, m_allocated[hash].size(), m_free[hash].size());
#endif
        m_allocated[hash].push_back({ std::chrono::system_clock::now(), resource });
        return resource;
    }

    virtual void recycle(HashedResource res) override final
    {
        if (!res) return;
        std::scoped_lock lock(m_mtx);
        assert(!res.dbgIsCorrupted());
        auto& list = m_allocated[res.accessHash()];
        auto it = list.begin();
#if SL_DEBUG_RESOURCE_POOL
        int count = 0;
#endif
        while(it != list.end())
        {
            if ((*it).second == res)
            {
                it = list.erase(it);
#if SL_DEBUG_RESOURCE_POOL
                count++;
                continue;
#else
                break;
#endif
            }
            it++;
        }
#if SL_DEBUG_RESOURCE_POOL
        assert(count == 1);
        for (auto& [timestamp, cached] : m_free[res.hash])
        {
            assert(res.resource != cached.resource);
        }
#endif
        m_free[res.accessHash()].push_back({ std::chrono::system_clock::now(), res });
    }

    virtual void clear() override final
    {
        m_compute->beginVRAMSegment(m_vramSegment.c_str());
        std::scoped_lock lock(m_mtx);
        m_free.clear();
        m_allocated.clear();
        m_compute->endVRAMSegment();
    }

    virtual void collectGarbage(float deltaMs = 1000.0f) override final
    {
        std::scoped_lock lock(m_mtx);
        m_compute->beginVRAMSegment(m_vramSegment.c_str());
        auto it = m_free.begin();
        while (it != m_free.end())
        {
            auto it1 = (*it).second.begin();
            while (it1 != (*it).second.end())
            {
                auto& [timestamp, resource] = (*it1);
                {
                    std::chrono::duration<float, std::milli> deltaSinceLastUsed = std::chrono::system_clock::now() - timestamp;
                    if (deltaSinceLastUsed.count() > deltaMs)
                    {
                        it1 = (*it).second.erase(it1);
                        continue;
                    }
                }
                it1++;
            }
#if SL_DEBUG_RESOURCE_POOL
            for (auto& [hash, list] : m_allocated)
            {
                if (hash == (*it).first)
                {
                    SL_LOG_VERBOSE("hash %llu [alloc %llu free %llu]", hash, list.size(), (*it).second.size());
                    break;
                }
            }
#endif
            it++;
        }
        m_compute->endVRAMSegment();
    }

    uint64_t getHash(const ResourceDescription& desc) const
    {
        uint64_t hash = 0;
        hash_combine(hash, desc.width);
        hash_combine(hash, desc.height);
        hash_combine(hash, desc.format);
        hash_combine(hash, desc.mips);
        hash_combine(hash, desc.depth);
        hash_combine(hash, desc.flags);
        hash_combine(hash, desc.state);
        return hash;
    };

    std::mutex m_mtx{};
    ICompute* m_compute{};
    std::string m_vramSegment{};
    std::map<uint64_t, std::vector<TimestampedResource>> m_free{};
    std::map<uint64_t, std::vector<TimestampedResource>> m_allocated{};
};

ResourceState HashedResource::s_invalidState{};
uint64_t HashedResource::s_invalidHash{};

ComputeStatus Generic::genericPostInit()
{
    getRenderAPI(m_platform);
    return ComputeStatus::eOk;
}

ComputeStatus Generic::init(Device device, param::IParameters* params)
{
    m_parameters = params;
    m_typelessDevice = device;
    params->get(sl::param::global::kPreferenceFlags, (uint64_t*)&m_preferenceFlags);
    m_cachedReflexSyncParams.version = NV_SET_REFLEX_SYNC_PARAMS_VER1;
    return ComputeStatus::eOk;
}

ComputeStatus Generic::shutdown()
{
    Generic::clearCache();

    // Release any tracked resources
    {
        std::scoped_lock lock(m_mutexResourceTrack);
        for (auto cachedResource : m_resourceTrackMap)
        {
            ((IUnknown*)cachedResource.second)->Release();
        }
        m_resourceTrackMap.clear();

        for (auto& frameResourceMap : m_frameResourceTrackingMap)
        {
            for (auto& cachedResource : frameResourceMap.second)
            {
                ((IUnknown*)cachedResource.second)->Release();
                cachedResource.second = nullptr;
            }
            frameResourceMap.second.clear();
        }
        m_frameResourceTrackingMap.clear();
    }

    CHI_CHECK(collectGarbage(UINT_MAX));
    SL_LOG_INFO("Delayed destroy resource list count %llu", m_resourcesToDestroy.size());
    m_vramSegments.clear();

    return ComputeStatus::eOk;
}

ComputeStatus Generic::clearCache()
{
    // Release shared resources
    for (auto& [original, shared] : m_sharedResourceMap)
    {
        if (shared.source != shared.translated)
        {
            destroySharedHandle(shared.handle);
            destroyResource(shared.translated);
            destroyResource(shared.clone);
        }
    }
    m_sharedResourceMap.clear();
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getVendorId(VendorId& id)
{
    IDXGIDevice* dxgiDevice{};
    if (SUCCEEDED(((IUnknown*)m_typelessDevice)->QueryInterface(&dxgiDevice)))
    {
        dxgiDevice->Release();
        IDXGIAdapter* adapter{};
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        {
            adapter->Release();
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                id = (VendorId)desc.VendorId;
                return ComputeStatus::eOk;
            }
        }
    }
    return ComputeStatus::eError;
}

ComputeStatus Generic::startTrackingResource(uint64_t uid, Resource resource)
{
    // Make sure we are thread safe
    std::scoped_lock lock(m_mutexResourceTrack);

    // NOTE: This covers d3d11/d3d12, VK currently does NOP here
    auto it = m_resourceTrackMap.find(uid);
    IUnknown* cachedResource = (it == m_resourceTrackMap.end()) ? nullptr : it->second;
    if (cachedResource)
    {
        if (cachedResource != resource->native)
        {
            // Note that here we could easily hold last reference and that is fine, host destroys tag and calls setTag(newTag)
            cachedResource->Release();
            cachedResource = nullptr;
        }
    }
    if (!cachedResource)
    {
        cachedResource = (IUnknown*)(resource->native);
        auto refCount = cachedResource->AddRef();
        m_resourceTrackMap[uid] = cachedResource;
        //std::wstring name = getDebugName(cachedResource);
        //SL_LOG_VERBOSE("Start tracking 0x%llx '%S' ref count %d", cachedResource, name.c_str(), refCount);
    }
    return ComputeStatus::eOk;
}

ComputeStatus Generic::startTrackingResource(uint32_t frameId, uint64_t uid, Resource resource)
{
    if (resource != nullptr && resource->native != nullptr)
    {
        // Make sure we are thread safe
        std::scoped_lock lock(m_mutexResourceTrack);
        
        // NOTE: This covers d3d11/d3d12, VK currently does NOP here
        auto cachedResource = (IUnknown*)(resource->native);
        auto refCount = cachedResource->AddRef();
        m_frameResourceTrackingMap[frameId][uid] = cachedResource;

        // frames must be deleted from this map, otherwise it would grow infinitely
        constexpr uint32_t InsaneNumberOfFramesInFlight = 30;
        assert(m_frameResourceTrackingMap.size() < InsaneNumberOfFramesInFlight);
        //std::wstring name = getDebugName(cachedResource);
        //SL_LOG_VERBOSE("Start tracking 0x%llx '%S' ref count %d", cachedResource, name.c_str(), refCount);
    }

    return ComputeStatus::eOk;
}

ComputeStatus Generic::stopTrackingResource(uint64_t id, Resource dbgResource)
{
    // Make sure we are thread safe
    std::scoped_lock lock(m_mutexResourceTrack);

    // NOTE: This covers d3d11/d3d12, VK currently does NOP here
    ResourceTrackingMap::const_iterator it = m_resourceTrackMap.find(id);
    IUnknown* cachedResource = (it == m_resourceTrackMap.end()) ? nullptr : it->second;
    if (cachedResource)
    {
        assert(cachedResource == dbgResource->native ||
            dbgResource->native == nullptr); // startTracking() and stopTracking() is called for different resources?
        // Note that here we could easily hold last reference and that is fine, host destroys tag and calls setTag(null)
        cachedResource->Release();
        m_resourceTrackMap.erase(it);
    }
    return ComputeStatus::eOk;
}

ComputeStatus Generic::stopTrackingResource(uint32_t frameId, uint64_t id, Resource dbgResource)
{
    // Make sure we are thread safe
    std::scoped_lock lock(m_mutexResourceTrack);

    IUnknown* cachedResource{};
    std::map<uint32_t, ResourceTrackingMap>::const_iterator frameMapIt{};
    ResourceTrackingMap::const_iterator resourceMapIt{};
    if (frameMapIt = m_frameResourceTrackingMap.find(frameId); frameMapIt != m_frameResourceTrackingMap.end())
    {
        if (resourceMapIt = m_frameResourceTrackingMap[frameId].find(id); resourceMapIt != m_frameResourceTrackingMap[frameId].end())
        {
            cachedResource = m_frameResourceTrackingMap[frameId][id];
        }
    }

    if (cachedResource != nullptr)
    {
        assert(cachedResource == dbgResource->native ||
            dbgResource->native == nullptr); // startTracking() and stopTracking() is called for different resources?
        // Note that here we could easily hold last reference and that is fine, host destroys tag and calls setTag(null)
        // NOTE: This covers d3d11/d3d12, VK currently does NOP here
        cachedResource->Release();
        cachedResource = m_frameResourceTrackingMap[frameId][id] = nullptr;
        m_frameResourceTrackingMap[frameId].erase(resourceMapIt);
        if (m_frameResourceTrackingMap[frameId].empty())
        {
            m_frameResourceTrackingMap.erase(frameMapIt);
        }
    }

    return ComputeStatus::eOk;
}

void Generic::setResourceTracked(chi::Resource resource, uint64_t tracked)
{
    assert(m_platform != RenderAPI::eVulkan);
    if (m_platform != RenderAPI::eVulkan)
    {
        auto unknown = ((IUnknown*)(resource->native));
        ID3D12Pageable* pageable = {};
        unknown->QueryInterface(&pageable);
        if (pageable)
        {
            if (FAILED(pageable->SetPrivateData(sResourceTrackGUID, sizeof(tracked), &tracked)))
            {
                SL_LOG_ERROR("Failed to set tracked for resource 0x%llx", resource);
            }
            pageable->Release();
        }
        else
        {
            ID3D11Resource* d3d11Resource = {};
            unknown->QueryInterface(&d3d11Resource);
            if (d3d11Resource)
            {
                if (FAILED(d3d11Resource->SetPrivateData(sResourceTrackGUID, sizeof(tracked), &tracked)))
                {
                    SL_LOG_ERROR("Failed to set tracked for resource 0x%llx", resource);
                }
                d3d11Resource->Release();
            }
        }
    }
}

bool Generic::isResourceTracked(chi::Resource resource)
{
    uint64_t tracked = 0;
    assert(m_platform != RenderAPI::eVulkan);
    if (m_platform != RenderAPI::eVulkan)
    {
        auto unknown = ((IUnknown*)(resource->native));
        ID3D12Pageable* pageable = {};
        unknown->QueryInterface(&pageable);
        if (pageable)
        {
            UINT size = sizeof(tracked);
            if (FAILED(pageable->GetPrivateData(sResourceTrackGUID, &size, &tracked)))
            {
                SL_LOG_ERROR("Failed to get tracked for resource 0x%llx", resource);
            }
            pageable->Release();
        }
        else
        {
            ID3D11Resource* d3d11Resource = {};
            unknown->QueryInterface(&d3d11Resource);
            if (d3d11Resource)
            {
                UINT size = sizeof(tracked);
                if (FAILED(d3d11Resource->GetPrivateData(sResourceTrackGUID, &size, &tracked)))
                {
                    SL_LOG_ERROR("Failed to get tracked for resource 0x%llx", resource);
                }
                d3d11Resource->Release();
            }
        }
    }
    return tracked == 1;
}

ComputeStatus Generic::getResourceState(Resource resource, ResourceState& state)
{
    if (!resource)
    {
        state = ResourceState::eUnknown;
        return ComputeStatus::eOk;
    }

    state = ResourceState::eGeneral;
    if (m_platform == RenderAPI::eD3D11)
    {
        return ComputeStatus::eOk;
    }
    return getResourceState(resource->state, state);
}

ComputeStatus Generic::transitionResources(CommandList cmdList, const ResourceTransition* transitions, uint32_t count, extra::ScopedTasks* scopedTasks)
{
    if (!cmdList)
    {
        return ComputeStatus::eInvalidArgument;
    }

    // All these cases are OK, nothing to do here
    if (!transitions || count == 0 || m_platform == RenderAPI::eD3D11)
    {
        return ComputeStatus::eOk;
    }

    std::vector<ResourceTransition> transitionList;
    for (uint32_t i = 0; i < count; i++)
    {
        auto tr = transitions[i];
        if (tr.from == ResourceState::eUnknown)
        {
            getResourceState(tr.fromNativeState, tr.from);
        }
        if (!tr.resource || !tr.resource->native || (tr.from & tr.to) != 0)
        {
            continue;
        }

        if (tr.from != ResourceState::eUnknown)
        {
            if (std::find(transitionList.begin(), transitionList.end(), tr) == transitionList.end())
            {
                transitionList.push_back(tr);
            }
        }
        else
        {
            SL_LOG_ERROR("From/to states must be provided");
            return ComputeStatus::eNotSupported;
        }
    }

    if (transitionList.empty()) return ComputeStatus::eOk;

    if (scopedTasks)
    {
        auto lambda = [this, cmdList, transitionList](void) -> void
        {
            std::vector<ResourceTransition> revTransitionList;
            for (auto& tr : transitionList)
            {
                if (chi::ResourceState(tr.from & tr.to) & chi::ResourceState::eStorageRW)
                {
                    // to and from states are UAV which means we need to insert barrier on scope exit to make sure writes are done
                    insertGPUBarrier(cmdList, tr.resource);
                }
                revTransitionList.push_back(ResourceTransition(tr.resource, tr.from, tr.to));
            }
            transitionResources(cmdList, revTransitionList.data(), (uint32_t)revTransitionList.size());
        };
        scopedTasks->tasks.push_back(lambda);
    }

    return transitionResourceImpl(cmdList, transitionList.data(), (uint32_t)transitionList.size());
}

ComputeStatus Generic::beginVRAMSegment(const char* name)
{
    if (!name) return ComputeStatus::eInvalidArgument;
    std::scoped_lock lock(m_mutexVRAM);
    auto& id = m_currentVRAMSegment[std::this_thread::get_id()];
    assert(id.empty() || id == kGlobalVRAMSegment);
    id = name;
    return ComputeStatus::eOk;
}

ComputeStatus Generic::endVRAMSegment()
{
    std::scoped_lock lock(m_mutexVRAM);
    auto& id = m_currentVRAMSegment[std::this_thread::get_id()];
    assert(!id.empty() && id != kGlobalVRAMSegment);
    id = kGlobalVRAMSegment;
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getAllocatedBytes(uint64_t& bytes, const char* name)
{ 
    bytes = {};
    std::scoped_lock lock(m_mutexVRAM);
    auto it = m_vramSegments.find(name);
    if(it == m_vramSegments.end()) return ComputeStatus::eInvalidArgument;
    auto& seg = (*it).second;
    bytes = seg.totalAllocatedSize;
    return ComputeStatus::eOk; 
}

Generic::VRAMSegment Generic::manageVRAM(Resource res, VRAMOperation op)
{
    ResourceDescription desc;
    getResourceDescription(res, desc);
    auto sizeInBytes = getResourceSize(res);
    auto name = getDebugName(res);

    std::scoped_lock lock(m_mutexVRAM);

    auto& id = m_currentVRAMSegment[std::this_thread::get_id()];
    if (id != kGlobalVRAMSegment)
    {
        auto& seg = m_vramSegments[id];
        if (op == VRAMOperation::eFree)
        {
            if (seg.allocCount == 0 || seg.totalAllocatedSize < sizeInBytes)
            {
                seg = {};
            }
            else
            {
                seg.allocCount--;
                seg.totalAllocatedSize -= sizeInBytes;
            }
        }
        else
        {
            seg.allocCount++;
            seg.totalAllocatedSize += sizeInBytes;
        }
        
        SL_LOG_VERBOSE("vram %s [%s %u %.1fMB usage:%.2fGB budget:%.2fGB] resource 0x%llx [%u:%u:%s] - '%S'", op == VRAMOperation::eFree ? "free" : "alloc", id.c_str(), seg.allocCount, 
            double(seg.totalAllocatedSize / (1024 * 1024)), double(m_vramUsageBytes.load() / (1024 * 1024 * 1024)), double(m_vramBudgetBytes.load() / (1024 * 1024 * 1024)),
            res->native, desc.width, desc.height, GFORMAT_STR[desc.format], name.c_str());
    }

    auto& seg = m_vramSegments[kGlobalVRAMSegment];
    if (op == VRAMOperation::eFree)
    {
        if (seg.allocCount == 0 || seg.totalAllocatedSize < sizeInBytes)
        {
            seg = {};
        }
        else
        {
            seg.allocCount--;
            seg.totalAllocatedSize -= sizeInBytes;
        }
    }
    else
    {
        seg.allocCount++;
        seg.totalAllocatedSize += sizeInBytes;
    }
    
    // Warn if global allocations are over the budget
    auto budgetedBytes = m_vramBudgetBytes.load();
    auto usedBytes = m_vramUsageBytes.load();
    if (usedBytes > budgetedBytes)
    {
        SL_LOG_WARN("Allocated %.2fMB which is more than allowed by the VRAM budget %.2fMB", usedBytes / (1024.0 * 1024.0), budgetedBytes / (1024.0 * 1024.0));
    }

    if (id == kGlobalVRAMSegment)
    {
        SL_LOG_VERBOSE("vram %s [%s %u %.1fMB usage:%.2fGB budget:%.2fGB] resource 0x%llx [%u:%u:%s] - '%S'", op == VRAMOperation::eFree ? "free" : "alloc", id.c_str(), seg.allocCount,
            double(seg.totalAllocatedSize / (1024 * 1024)), double(m_vramUsageBytes.load() / (1024 * 1024 * 1024)), double(m_vramBudgetBytes.load() / (1024 * 1024 * 1024)),
            res->native, desc.width, desc.height, GFORMAT_STR[desc.format], name.c_str());
    }
    return seg;
}

ComputeStatus Generic::createBuffer(const ResourceDescription& CreateResourceDesc, Resource& OutResource, const char InFriendlyName[])
{
    ResourceDescription ResourceDesc = CreateResourceDesc;
    ResourceDesc.flags |= ResourceFlags::eRawOrStructuredBuffer | ResourceFlags::eConstantBuffer;

    // if we don't have any name at all - grab at least this one
    if (ResourceDesc.sName.size() == 0)
    {
        ResourceDesc.sName = InFriendlyName;
    }
    
    // Determine default buffer state if not explicitly provided
    ResourceState state = ResourceDesc.state;
    if (state == ResourceState::eUnknown)
    {
        switch (ResourceDesc.heapType)
        {
            case eHeapTypeUpload:
                state = ResourceState::eGenericRead;
                break;
            case eHeapTypeReadback:
                state = ResourceState::eCopyDestination;
                break;
            case eHeapTypeDefault:
                // Buffers in default heap should start in Common state (D3D12) or General (Vulkan)
                state = ResourceState::eGeneral;
                break;
        }
    }
    
    CHI_CHECK(createBufferResourceImpl(ResourceDesc, OutResource, state, InFriendlyName));

    manageVRAM(OutResource, VRAMOperation::eAlloc);

    setDebugName(OutResource, InFriendlyName);

    return ComputeStatus::eOk;
}

ComputeStatus Generic::createTexture2D(const ResourceDescription& CreateResourceDesc, Resource& OutResource, const char InFriendlyName[])
{
    return createTexture2DResourceShared(CreateResourceDesc, OutResource, CreateResourceDesc.format == eFormatINVALID, InFriendlyName);
}

ComputeStatus Generic::createTexture2DResourceShared(const ResourceDescription& CreateResourceDesc, Resource& OutResource, bool UseNativeFormat, const char InFriendlyName[])
{
    ResourceDescription resourceDesc = CreateResourceDesc;
    if (resourceDesc.flags & (ResourceFlags::eRawOrStructuredBuffer | ResourceFlags::eConstantBuffer))
    {
        SL_LOG_ERROR("Creating tex2d with buffer flags");
        return ComputeStatus::eError;
    }

    if (!(resourceDesc.state & ResourceState::ePresent))
    {
        resourceDesc.flags |= ResourceFlags::eShaderResourceStorage;
    }
    if (resourceDesc.format == eFormatINVALID && resourceDesc.nativeFormat != NativeFormatUnknown)
    {
        getFormat(resourceDesc.nativeFormat, resourceDesc.format);
    }

    // if we don't have any name at all - grab at least this one
    if (resourceDesc.sName.size() == 0)
    {
        resourceDesc.sName = InFriendlyName;
    }
    
    // Determine default texture state if not explicitly provided
    ResourceState state = resourceDesc.state;
    if (state == ResourceState::eUnknown)
    {
        switch (resourceDesc.heapType)
        {
            case eHeapTypeUpload:
                state = ResourceState::eGenericRead;
                break;
            case eHeapTypeReadback:
                state = ResourceState::eCopyDestination;
                break;
            case eHeapTypeDefault:
                state = ResourceState::eCopyDestination;
                break;
        }
    }
    
    CHI_CHECK(createTexture2DResourceSharedImpl(resourceDesc, OutResource, UseNativeFormat, state, InFriendlyName));

    manageVRAM(OutResource, VRAMOperation::eAlloc);

    setDebugName(OutResource, InFriendlyName);
    return ComputeStatus::eOk;
}

ComputeStatus Generic::destroy(std::function<void(void)> task, uint32_t frameDelay)
{
    // Delayed destroy for safety
    {
        std::lock_guard<std::mutex> lock(m_mutexResource);
        m_destroyWithLambdas.push_back({ task, m_finishedFrame, frameDelay });
    }
    SL_LOG_VERBOSE("Scheduled to destroy lambda task - frame %u", m_finishedFrame.load());
    return ComputeStatus::eOk;
}

ComputeStatus Generic::destroyResource(Resource resource, uint32_t frameDelay)
{
    if (!resource || !resource->native) return ComputeStatus::eOk; // OK to release null resource

    auto bufferOrTex2d = resource->type == ResourceType::eBuffer || resource->type == ResourceType::eTex2d;
    if (bufferOrTex2d)
    {
        manageVRAM(resource, VRAMOperation::eFree);
    }

    // Skip the host release callback for Vulkan swapchain images: they were never allocated by the
    // host callback, and they need destroyResourceDeferredImpl to clean up the internally-created VkImageView.
    if (m_releaseCallback && bufferOrTex2d && !(resource->internalFlags & sl::Resource::eVulkanSwapChainImage))
    {
        m_releaseCallback(resource, m_typelessDevice);
        delete resource;
    }
    else
    {
        if (frameDelay == 0)
        {
            std::lock_guard<std::mutex> lock(m_mutexResource);
            destroyResourceDeferredImpl(resource);
            delete resource;
        }
        else
        {
            // Delayed destroy for safety
            std::lock_guard<std::mutex> lock(m_mutexResource);
            TimestampedResource rest = { resource, m_finishedFrame, frameDelay };
            if (std::find(m_resourcesToDestroy.begin(), m_resourcesToDestroy.end(), rest) == m_resourcesToDestroy.end())
            {
                if (m_platform != RenderAPI::eVulkan)
                {
                    //! Safety, make sure by the time we get to release this resource it is still alive
                    //! 
                    //! This is important because of the swap-chains and their buffers which are shared with the host.
                    auto unknown = (IUnknown*)(resource->native);
                    unknown->AddRef();
                }
                m_resourcesToDestroy.push_back(rest);
            }
        }
    }
    return ComputeStatus::eOk;
}

ComputeStatus Generic::collectGarbage(uint32_t finishedFrame)
{
    if (finishedFrame != UINT_MAX)
    {
        m_finishedFrame.store(finishedFrame);
    }

    // A destroy task can call into external code which then re-enters CHI to
    // schedule destruction. Snapshot the ready tasks while holding the queue
    // lock, then invoke them after releasing the lock. Work scheduled by a
    // task is deliberately deferred until the next collectGarbage call.
    std::vector<TimestampedLambda> readyTasks;
    {
        std::lock_guard<std::mutex> lock(m_mutexResource);
        auto it = m_destroyWithLambdas.begin();
        while (it != m_destroyWithLambdas.end())
        {
            if (finishedFrame > (it->frame + it->frameDelay))
            {
                readyTasks.push_back(std::move(*it));
                it = m_destroyWithLambdas.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& item : readyTasks)
    {
        SL_LOG_VERBOSE("Calling destroy lambda - scheduled at frame %u - finished frame %u - forced %s", item.frame, m_finishedFrame.load(), finishedFrame != UINT_MAX ? "no" : "yes");
        item.task();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutexResource);
        auto it = m_resourcesToDestroy.begin();
        while (it != m_resourcesToDestroy.end())
        {
            auto& tres = (*it);
            // Release resources dumped more than few frames ago
            if (finishedFrame > (tres.frame + tres.frameDelay))
            {
                if (m_platform != RenderAPI::eVulkan)
                {
                    //! Make sure to release the "safety" reference that was added when scheduling resource for destruction.
                    //! 
                    //! This is important because of the swap-chains and their buffers which are shared with the host.
                    auto unknown = (IUnknown*)(tres.resource->native);
                    unknown->Release();
                }
                auto name = getDebugName(tres.resource);
                auto ref = destroyResourceDeferredImpl(tres.resource);
                SL_LOG_VERBOSE("Destroyed 0x%llx(%S) - scheduled at frame %u - finished frame %u - forced %s - ref count %d", tres.resource, name.c_str(), tres.frame, m_finishedFrame.load(), finishedFrame != UINT_MAX ? "no" : "yes", ref);
                delete tres.resource;
                it = m_resourcesToDestroy.erase(it);
            }
            else
            {
                it++;
            }
        }
    }

    return ComputeStatus::eOk;
}

ComputeStatus Generic::insertGPUBarrierList(CommandList cmdList, const Resource* InResources, unsigned int InResourceCount, BarrierType InBarrierType)
{
    if (InBarrierType == BarrierType::eBarrierTypeUAV)
    {
        std::vector< D3D12_RESOURCE_BARRIER> Barriers;
        for (unsigned int i = 0; i < InResourceCount; i++)
        {
            const Resource& Res = InResources[i];
            ComputeStatus Status = insertGPUBarrier(cmdList, Res, InBarrierType);
            if (ComputeStatus::eOk != Status)
                return Status;
        }
    }
    else
    {
        assert(false);
        return ComputeStatus::eNotSupported;
    }
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getBytesPerPixel(Format InFormat, size_t& size)
{
    static constexpr size_t BYTES_PER_PIXEL_TABLE[] = {
        1,                      // eFormatINVALID - aka unknown - used for buffers
        4 * sizeof(float),      // eFormatRGBA32F,
        4 * sizeof(uint16_t),   // eFormatRGBA16F,
        3 * sizeof(float),      // eFormatRGB32F, 
        3 * sizeof(uint16_t),   // eFormatRGB16F,
        2 * sizeof(uint16_t),   // eFormatRG16F,
        1 * sizeof(uint16_t),   // eFormatR16F,
        2 * sizeof(float),      // eFormatRG32F,
        1 * sizeof(float),      // eFormatR32F,
        1,                      // eFormatR8UN,
        2,                      // eFormatRG8UN,
        sizeof(uint32_t),       // eFormatRGB11F,
        sizeof(uint32_t),       // eFormatRGBA8UN,
        sizeof(uint32_t),       // eFormatSRGBA8UN,
        sizeof(uint32_t),       // eFormatBGRA8UN,
        sizeof(uint32_t),       // eFormatSBGRA8UN,
        2 * sizeof(uint16_t),   // eFormatRG16UI,
        2 * sizeof(uint16_t),   // eFormatRG16SI,
        1 * sizeof(uint8_t),    // eFormatE5M3,
        sizeof(uint32_t),       // eFormatRGB10A2UN,
        1,                      // eFormatR8UI,
        2,                      // eFormatR16UI,
        4,                      // eFormatRG16UN,
        4,                      // eFormatR32UI,
        8,                      // eFormatRG32UI,
        8,                      // eFormatD32S32,
        4,                      // eFormatD24S8,
        8,                      // eFormatD32S8U,
        sizeof(uint32_t),       // eFormatRGBA8SN,
    }; static_assert(countof(BYTES_PER_PIXEL_TABLE) == eFormatCOUNT, "Not enough numbers for eFormatCOUNT");

    size = BYTES_PER_PIXEL_TABLE[InFormat];
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getResourceFootprint(Resource resource, ResourceFootprint& footprint)
{
    if (!resource || !resource->native) return ComputeStatus::eInvalidArgument;

    size_t pixelSizeInBytes{};
    Format format{};
    getFormat(resource->nativeFormat, format);
    getBytesPerPixel(format, pixelSizeInBytes);

    ResourceDescription desc;
    getResourceDescription(resource, desc);

    // D3D12 has special function for this, here we just provide an estimate which should be close enough for regular resolutions
    // Note that resource that we use have a single mip level
    assert(desc.mips == 1);

    footprint.depth = desc.depth;
    footprint.width = desc.width;
    footprint.height = desc.height;
    footprint.offset = 0;
    footprint.rowPitch = desc.width * (uint32_t)pixelSizeInBytes;
    footprint.numRows = desc.height;
    footprint.rowSizeInBytes = desc.height * pixelSizeInBytes;
    footprint.totalBytes = desc.depth * desc.width * desc.height * pixelSizeInBytes;
    footprint.format = format;

    return ComputeStatus::eOk;
}

uint64_t Generic::getResourceSize(Resource res)
{
    ResourceDescription resourceDesc;
    if (getResourceDescription(res, resourceDesc) != ComputeStatus::eOk)
    {
        return 0;
    }
    Format format = resourceDesc.format;
    if (format == eFormatINVALID && resourceDesc.nativeFormat != NativeFormatUnknown)
    {
        getFormat(resourceDesc.nativeFormat, format);
        if (format == eFormatINVALID)
        {
            SL_LOG_ERROR("Don't know the size for resource 0x%llx format %u native %u", res, resourceDesc.format, resourceDesc.nativeFormat);
        }
    }
    size_t bytesPerPixel;
    getBytesPerPixel(format, bytesPerPixel);
    return resourceDesc.width * resourceDesc.height * bytesPerPixel * resourceDesc.depth;
}

ComputeStatus Generic::getNativeFormat(Format format, NativeFormat& native)
{
    native = DXGI_FORMAT_UNKNOWN;
    switch (format)
    {
        case eFormatR8UN: native = DXGI_FORMAT_R8_UNORM; break;
        case eFormatRG8UN: native = DXGI_FORMAT_R8G8_UNORM; break;
        case eFormatRGB10A2UN: native = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case eFormatRGBA8UN: native = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case eFormatRGBA8SN: native = DXGI_FORMAT_R8G8B8A8_SNORM; break;
        case eFormatBGRA8UN: native = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case eFormatRGBA32F: native = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case eFormatRGB32F:  native = DXGI_FORMAT_R32G32B32_FLOAT; break;
        case eFormatRGBA16F: native = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case eFormatRGB11F: native = DXGI_FORMAT_R11G11B10_FLOAT; break;
        case eFormatRG16F: native = DXGI_FORMAT_R16G16_FLOAT; break;
        case eFormatRG16UI: native = DXGI_FORMAT_R16G16_UINT; break;
        case eFormatRG16SI: native = DXGI_FORMAT_R16G16_SINT; break;
        case eFormatRG32F: native = DXGI_FORMAT_R32G32_FLOAT; break;
        case eFormatR16F: native = DXGI_FORMAT_R16_FLOAT; break;
        case eFormatR32F: native = DXGI_FORMAT_R32_FLOAT; break;
        case eFormatR8UI: native = DXGI_FORMAT_R8_UINT; break;
        case eFormatR16UI: native = DXGI_FORMAT_R16_UINT; break;
        case eFormatRG16UN: native = DXGI_FORMAT_R16G16_UNORM; break;
        case eFormatR32UI: native = DXGI_FORMAT_R32_UINT; break;
        case eFormatRG32UI: native = DXGI_FORMAT_R32G32_UINT; break;
        case eFormatD24S8: native = DXGI_FORMAT_R24G8_TYPELESS; break;
        case eFormatD32S32: native = DXGI_FORMAT_R32G8X24_TYPELESS; break;
        case eFormatSBGRA8UN: native = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
        case eFormatSRGBA8UN: native = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
        case eFormatE5M3: // fall through
        default: assert(false);
    }
    return ComputeStatus::eOk;
};

ComputeStatus Generic::getFormat(NativeFormat nativeFmt, Format& format)
{
    format = eFormatINVALID;

    DXGI_FORMAT DXGIFmt = static_cast<DXGI_FORMAT>(nativeFmt);

    switch (DXGIFmt)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        format = eFormatSBGRA8UN; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        format = eFormatBGRA8UN; break;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
        format = eFormatR8UN; break;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_TYPELESS:
        format = eFormatRG8UN; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        format = eFormatRGB10A2UN; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        format = eFormatSRGBA8UN; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        format = eFormatRGBA8UN; break;
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        format = eFormatRGBA8SN; break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        format = eFormatRGBA32F; break;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        format = eFormatRGB32F; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        format = eFormatRGBA16F; break;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        format = eFormatRGB11F; break;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
        format = eFormatRG16F; break;
    case DXGI_FORMAT_R16G16_UINT:
        format = eFormatRG16UI; break;
    case DXGI_FORMAT_R16G16_SINT:
        format = eFormatRG16SI; break;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        format = eFormatRG32F; break;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
        format = eFormatR16F; break;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        format = eFormatR32F; break;
    case DXGI_FORMAT_R8_UINT:
        format = eFormatR8UI; break;
    case DXGI_FORMAT_R16_UINT:
        format = eFormatR16UI; break;
    case DXGI_FORMAT_R16G16_UNORM:
        format = eFormatRG16UN; break;
    case DXGI_FORMAT_R32_UINT:
        format = eFormatR32UI; break;
    case DXGI_FORMAT_R32G32_UINT:
        format = eFormatRG32UI; break;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        format = eFormatD24S8; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        format = eFormatD32S32;
    }

    return ComputeStatus::eOk;
};

ComputeStatus Generic::getFormatAsString(const Format format, std::string& name)
{
#define RETURN_STR(e) case e : name = #e; break;
    switch (format)
    {
        RETURN_STR(eFormatR8UN);
        RETURN_STR(eFormatRG8UN);
        RETURN_STR(eFormatRGB10A2UN);
        RETURN_STR(eFormatRGBA8UN);
        RETURN_STR(eFormatRGBA8SN);
        RETURN_STR(eFormatBGRA8UN);
        RETURN_STR(eFormatRGBA32F);
        RETURN_STR(eFormatRGB32F);
        RETURN_STR(eFormatRGBA16F);
        RETURN_STR(eFormatRGB11F);
        RETURN_STR(eFormatRG16F);
        RETURN_STR(eFormatRG16UI);
        RETURN_STR(eFormatRG16SI);
        RETURN_STR(eFormatRG32F);
        RETURN_STR(eFormatR16F);
        RETURN_STR(eFormatR32F);
        RETURN_STR(eFormatR8UI);
        RETURN_STR(eFormatR16UI);
        RETURN_STR(eFormatRG16UN);
        RETURN_STR(eFormatR32UI);
        RETURN_STR(eFormatRG32UI);
        RETURN_STR(eFormatD24S8);
        RETURN_STR(eFormatD32S32);
        RETURN_STR(eFormatSBGRA8UN);
        RETURN_STR(eFormatSRGBA8UN);
        RETURN_STR(eFormatE5M3);
        default: name = "eInvalid";
    }
    return ComputeStatus::eOk;
}

bool Generic::savePFM(const std::string &path, const char* srcBuffer, const int width, const int height)
{
    auto fpath = path;
    fpath += ".pfm";

    const int totalBytes = width * height * sizeof(float) * 3;

    // Setup header
    const std::string header = "PF\n" + std::to_string(width) + " " + std::to_string(height) + "\n-1.0\n";
    const size_t headerSize = header.length();
    const char* headerArr = header.c_str();

    std::ofstream binWriter(fpath.c_str(), std::ios::binary);

    if (!binWriter)
    {
        SL_LOG_ERROR( "Failed to open %s", fpath.c_str());
        binWriter.close();
        return false;
    }

    binWriter.write(header.c_str(), headerSize);
    binWriter.write(srcBuffer, totalBytes);
    binWriter.close();
    return true;
}

ComputeStatus Generic::setSleepMode(const ReflexOptions& consts)
{
    NV_SET_SLEEP_MODE_PARAMS_V1 params = { 0 };
    params.version = NV_SET_SLEEP_MODE_PARAMS_VER1;
    params.bLowLatencyMode = consts.mode != ReflexMode::eOff;
    params.bLowLatencyBoost = consts.mode == ReflexMode::eLowLatencyWithBoost;
    params.minimumIntervalUs = consts.frameLimitUs;
    params.bUseMarkersToOptimize = consts.useMarkersToOptimize;
    NVAPI_CHECK(NvAPI_D3D_SetSleepMode((IUnknown*)m_typelessDevice, &params));
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getSleepStatus(ReflexState& settings)
{

    NV_GET_SLEEP_STATUS_PARAMS_V1 params = {};
    params.version = NV_GET_SLEEP_STATUS_PARAMS_VER1;
    NVAPI_CHECK(NvAPI_D3D_GetSleepStatus((IUnknown*)m_typelessDevice, &params));
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getFrameGenParams(uint8_t& outFgMultiplier, bool& outDfgControl)
{
    NV_GET_SLEEP_STATUS_PARAMS_V1_BFM_37870595 params{};
    compile_time_assert(sizeof(NV_GET_SLEEP_STATUS_PARAMS_V1_BFM_37870595) == sizeof(NV_GET_SLEEP_STATUS_PARAMS_V1));
    params.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
    NVAPI_CHECK(NvAPI_D3D_GetSleepStatus((IUnknown*)m_typelessDevice, (NV_GET_SLEEP_STATUS_PARAMS*)&params));
    outFgMultiplier = params.fgMultiplier;
    outDfgControl = params.bDfgControl != 0;
    return ComputeStatus::eOk;
}

//TODO: Remove this binary compatible struct once we update the NVAPI headers.
typedef struct
{
    NvU32  version;
    struct FrameReport {
        NvU64 frameID;
        NvU64 inputSampleTime;
        NvU64 simStartTime;
        NvU64 simEndTime;
        NvU64 renderSubmitStartTime;
        NvU64 renderSubmitEndTime;
        NvU64 presentStartTime;
        NvU64 presentEndTime;
        NvU64 driverStartTime;
        NvU64 driverEndTime;
        NvU64 osRenderQueueStartTime;
        NvU64 osRenderQueueEndTime;
        NvU64 gpuRenderStartTime;
        NvU64 gpuRenderEndTime;
        NvU32 gpuActiveRenderTimeUs;
        NvU32 gpuFrameTimeUs;
        NvU64 cameraConstructedTime;    // New
        NvU32 crossAdapterCopyTimeUs;   // New
        NvU8  rsvd[108];                // [120]->[108]
    } frameReport[64];
    NvU8  rsvd[32];
} NV_LATENCY_RESULT_PARAMS_V1_BFM_36495674;

ComputeStatus Generic::getLatencyReport(ReflexState& settings)
{
    NV_LATENCY_RESULT_PARAMS_V1_BFM_36495674 params = {};
    compile_time_assert(sizeof(NV_LATENCY_RESULT_PARAMS_V1_BFM_36495674) == sizeof(NV_LATENCY_RESULT_PARAMS_V1));
    params.version = NV_LATENCY_RESULT_PARAMS_VER1;
    NVAPI_CHECK(NvAPI_D3D_GetLatency((IUnknown*)m_typelessDevice, (NV_LATENCY_RESULT_PARAMS*)&params));

    for (auto i = 0; i < kReflexFrameReportCount; i++)
    {
        settings.frameReport[i].frameID = params.frameReport[i].frameID;
        settings.frameReport[i].inputSampleTime = params.frameReport[i].inputSampleTime;
        settings.frameReport[i].simStartTime = params.frameReport[i].simStartTime;
        settings.frameReport[i].simEndTime = params.frameReport[i].simEndTime;
        settings.frameReport[i].renderSubmitStartTime = params.frameReport[i].renderSubmitStartTime;
        settings.frameReport[i].renderSubmitEndTime = params.frameReport[i].renderSubmitEndTime;
        settings.frameReport[i].presentStartTime = params.frameReport[i].presentStartTime;
        settings.frameReport[i].presentEndTime = params.frameReport[i].presentEndTime;
        settings.frameReport[i].driverStartTime = params.frameReport[i].driverStartTime;
        settings.frameReport[i].driverEndTime = params.frameReport[i].driverEndTime;
        settings.frameReport[i].osRenderQueueStartTime = params.frameReport[i].osRenderQueueStartTime;
        settings.frameReport[i].osRenderQueueEndTime = params.frameReport[i].osRenderQueueEndTime;
        settings.frameReport[i].gpuRenderStartTime = params.frameReport[i].gpuRenderStartTime;
        settings.frameReport[i].gpuRenderEndTime = params.frameReport[i].gpuRenderEndTime;
        settings.frameReport[i].gpuActiveRenderTimeUs = params.frameReport[i].gpuActiveRenderTimeUs;
        settings.frameReport[i].gpuFrameTimeUs = params.frameReport[i].gpuFrameTimeUs;

        if (settings.structVersion >= kStructVersion2)
        {
            settings.frameReport2[i].cameraConstructedTime = params.frameReport[i].cameraConstructedTime;
            settings.frameReport2[i].crossAdapterCopyTimeUs = params.frameReport[i].crossAdapterCopyTimeUs;
        }
    }

    return ComputeStatus::eOk;
}

ComputeStatus Generic::sleep()
{
    NVAPI_CHECK(NvAPI_D3D_Sleep((IUnknown*)m_typelessDevice));
    return ComputeStatus::eOk;
}

ComputeStatus Generic::setReflexMarker(PCLMarker marker, uint64_t frameId)
{
    NV_LATENCY_MARKER_PARAMS_V1 params = { 0 };
    params.version = NV_LATENCY_MARKER_PARAMS_VER1;
    params.frameID = frameId;
    params.markerType = (NV_LATENCY_MARKER_TYPE)marker;
    NVAPI_CHECK(NvAPI_D3D_SetLatencyMarker((IUnknown*)m_typelessDevice, &params));
    return ComputeStatus::eOk;
}

ComputeStatus Generic::setReflexSync(bool enable, int32_t timeInQueueUs, uint32_t timeInQueueUsTarget, uint32_t vblankIntervalUs)
{
    std::unique_lock lock(m_mutexReflexSync);

    m_cachedReflexSyncParams.bEnable = enable ? 1 : 0;
    m_cachedReflexSyncParams.bDisable = enable ? 0 : 1;
    m_cachedReflexSyncParams.timeInQueueUs = timeInQueueUs;
    m_cachedReflexSyncParams.timeInQueueUsTarget = timeInQueueUsTarget;
    m_cachedReflexSyncParams.vblankIntervalUs = vblankIntervalUs;

    NVAPI_CHECK(NvAPI_D3D_SetReflexSync((IUnknown*)m_typelessDevice, (NV_SET_REFLEX_SYNC_PARAMS*)&m_cachedReflexSyncParams));

    m_cachedReflexSyncParams.bEnable = 0;
    m_cachedReflexSyncParams.bDisable = 0;

    return ComputeStatus::eOk;
}

ComputeStatus Generic::setReflexSyncFG(uint8_t dfgMaxMultiplier, uint32_t dfgTargetFps, uint8_t fgMultiplier)
{
    std::unique_lock lock(m_mutexReflexSync);

    m_cachedReflexSyncParams.fgMultiplier = fgMultiplier;
    m_cachedReflexSyncParams.dfgMaxMultiplier = dfgMaxMultiplier;
    m_cachedReflexSyncParams.dfgTargetFps = dfgTargetFps;

    NVAPI_CHECK(NvAPI_D3D_SetReflexSync((IUnknown*)m_typelessDevice, (NV_SET_REFLEX_SYNC_PARAMS*)&m_cachedReflexSyncParams));
    return ComputeStatus::eOk;
}

ComputeStatus Generic::fetchTranslatedResourceFromCache(ICompute* compute, ResourceType type, Resource resource, TranslatedResource& shared, const char friendlyName[])
{
    if (!compute || !resource || !resource->native)
    {
        // Pass through nothing to do since there is no other API
        shared.source = resource;
        shared.translated = resource;
        return ComputeStatus::eOk;
    }

    auto otherAPI = (Generic*)compute;

    auto it = m_sharedResourceMap.find(resource->native);
    // If resource is cached and it is a texture not a fence or semaphore check for recycled pointers
    if (type == ResourceType::eTex2d && it != m_sharedResourceMap.end())
    {
        if (!isResourceTracked(resource))
        {
            // Pointer recycled by DX, remove from cache
            SL_LOG_WARN("Detected recycled resource 0x%llx - removing from the shared resource cache", resource);

            auto& shared = (*it).second;
            destroySharedHandle(shared.handle);
            destroyResource(shared.translated);
            otherAPI->destroyResource(shared.clone);
            m_sharedResourceMap.erase(it);
            it = m_sharedResourceMap.end();
        }
    }
    if (it == m_sharedResourceMap.end())
    {
        chi::ResourceDescription desc{};
        if (type == ResourceType::eTex2d)
        {
            otherAPI->getResourceDescription(resource, desc);
        }
        else if (type == ResourceType::eFence)
        {
            // All semaphores created internally are shareable by default
            desc.flags = chi::ResourceFlags::eSharedResource;
        }
        else
        {
            SL_LOG_ERROR( "Only semaphores and tex2d objects can be shared");
            return ComputeStatus::eInvalidArgument;
        }

        if ((desc.flags & chi::ResourceFlags::eSharedResource))
        {
            CHI_VALIDATE(otherAPI->createSharedHandle(resource, shared.handle));
        }
        else
        {
            // Not shared, need to make a copy first then share
            // 
            // Warn only if not depth stencil attachment since those are special formats which cannot be shared as NT handle
            if (!(desc.flags & ResourceFlags::eDepthStencilAttachment))
            {
                SL_LOG_WARN("Tagged d3d11 resources 0x%llx should be created with the 'D3D11_RESOURCE_MISC_SHARED_NTHANDLE' flag to avoid additional copies", resource);
            }
            desc.flags |= chi::ResourceFlags::eSharedResource;
            std::string name = friendlyName + std::string(".clone");
            CHI_VALIDATE(otherAPI->createTexture2D(desc, shared.clone, name.c_str()));
            CHI_VALIDATE(otherAPI->createSharedHandle(shared.clone, shared.handle));
        }
        CHI_VALIDATE(getResourceFromSharedHandle(type, shared.handle, shared.translated));

        m_sharedResourceMap[resource->native] = shared;
        if (type == ResourceType::eTex2d)
        {
            // Mark for tracking so we can detect recycled pointers
            setResourceTracked(resource, 1);
        }
    }
    else
    {
        shared.translated = (*it).second.translated;
        shared.handle = (*it).second.handle;
        shared.clone = (*it).second.clone;
    }
    shared.source = resource;
    return ComputeStatus::eOk;
}

ComputeStatus Generic::getRefreshRate(WindowHandle window, float& refreshRate)
{
    if (!window)
    {
        return ComputeStatus::eInvalidArgument;
    }

    HWND hwnd = (HWND)window;

    // Make sure the window actually exists
    if (!IsWindow(hwnd))
    {
        SL_LOG_ERROR("Window handle 0x%llx is not a valid window", window);
        return ComputeStatus::eError;
    }
    
    // Get the monitor that contains the window
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor)
    {
        SL_LOG_ERROR("Failed to get monitor from window handle 0x%llx", window);
        return ComputeStatus::eError;
    }

    // Get monitor info
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(hMonitor, &monitorInfo))
    {
        SL_LOG_ERROR("Failed to get monitor info for window handle 0x%llx", window);
        return ComputeStatus::eError;
    }

    // Query display configuration for precise refresh rate
    UINT32 pathCount, modeCount;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    {
        SL_LOG_ERROR("Failed to get display config buffer sizes for window handle 0x%llx", window);
        return ComputeStatus::eError;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), 
                           &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
    {
        SL_LOG_ERROR("Failed to query display config for window handle 0x%llx", window);
        return ComputeStatus::eError;
    }

    // Find the path that matches our monitor
    for (const auto& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
        {
            continue;
        }

        if (wcscmp(monitorInfo.szDevice, sourceName.viewGdiDeviceName) != 0)
        {
            continue;
        }

        // Get precise refresh rate from rational number
        // (numerator/denominator). Divide as double for maximum precision.
        const DISPLAYCONFIG_RATIONAL& rationalRate = path.targetInfo.refreshRate;
        refreshRate = (float)((double)rationalRate.Numerator / rationalRate.Denominator);
        return ComputeStatus::eOk;
    }

    SL_LOG_ERROR("Failed to get refresh rate from window handle 0x%llx", window);
    return ComputeStatus::eError;
}

ComputeStatus Generic::getDisplayId(WindowHandle window, uint32_t& displayId)
{
    displayId = 0;

    if (!window)
    {
        return ComputeStatus::eInvalidArgument;
    }

    HWND hwnd = (HWND)window;

    if (!IsWindow(hwnd))
    {
        SL_LOG_ERROR("Window handle 0x%llx is not a valid window", window);
        return ComputeStatus::eError;
    }

    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor)
    {
        SL_LOG_ERROR("Failed to get monitor from window handle 0x%llx", window);
        return ComputeStatus::eError;
    }

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(hMonitor, &monitorInfo))
    {
        SL_LOG_ERROR("Failed to get monitor info for window handle 0x%llx", window);
        return ComputeStatus::eError;
    }

    char deviceNameA[CCHDEVICENAME];
    WideCharToMultiByte(CP_ACP, 0, monitorInfo.szDevice, -1, deviceNameA, CCHDEVICENAME, nullptr, nullptr);

    NvU32 nvDisplayId = 0;
    NvAPI_Status status = NvAPI_DISP_GetDisplayIdByDisplayName(deviceNameA, &nvDisplayId);
    if (status != NVAPI_OK)
    {
        SL_LOG_ERROR("NvAPI_DISP_GetDisplayIdByDisplayName failed for '%s' with status %d", deviceNameA, status);
        return ComputeStatus::eError;
    }

    displayId = nvDisplayId;
    return ComputeStatus::eOk;
}

ComputeStatus Generic::createResourcePool(IResourcePool** pool, const char* vramSegment)
{
    if (!pool) return ComputeStatus::eInvalidArgument;
    *pool = new ResourcePool(this, vramSegment);
    return ComputeStatus::eOk;
}
ComputeStatus Generic::destroyResourcePool(IResourcePool* pool)
{
    if (!pool) return ComputeStatus::eInvalidArgument;
    pool->clear();
    delete pool;
    return ComputeStatus::eOk;
}

}
}
