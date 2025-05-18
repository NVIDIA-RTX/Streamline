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
#include <d3d12.h>
#include <future>

#include "include/sl.h"
#include "include/sl_consts.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.plugin/plugin.h"
#include "source/core/sl.param/parameters.h"
#include "source/platforms/sl.chi/compute.h"
#include "source/platforms/sl.chi/vulkan.h"
#include "source/plugins/sl.template/versions.h"
#include "source/plugins/sl.common/commonInterface.h"
#include "nvapi.h"
#include "external/json/include/nlohmann/json.hpp"
#include "external/imgui/imgui.h"
#include "external/imgui/imgui_internal.h"
#include "external/implot/implot.h"
#include "external/vulkan/include/vulkan/vulkan_win32.h"
#include "_artifacts/gitVersion.h"
#include "_artifacts/json/imgui_json.h"
#include "source/plugins/sl.imgui/imguiTypes.h"
#include "source/plugins/sl.imgui/input.h"
#include "source/plugins/sl.imgui/imgui.h"
#include "source/plugins/sl.imgui/imgui_impl_dx12.h"
#include "source/plugins/sl.imgui/imgui_impl_vulkan.h"
#include "source/plugins/sl.imgui/imgui_impl_win32.h"
#include "source/plugins/sl.imgui/DroidSans-Mono.h"

using json = nlohmann::json;

constexpr uint32_t NUM_BACK_BUFFERS = 4;

namespace sl
{

using namespace type;

namespace imgui
{
//! Our common context
//! 
//! Here we can keep whatever global state we need
//! 

struct VKSwapchainInfo
{
    VkSwapchainCreateInfoKHR vkSwapChainParams{};
    std::vector<VkImage> vkSwapchainImages{};
    uint32_t backBufferFormat{};
    uint32_t backBufferWidth{};
    uint32_t backBufferHeight{};
    uint32_t bufferCount{};
    HWND backBufferHwnd{};
};

// TODO: Add missing synchronization for some of the graphics APIs such as swapchain-related ones
// accessing ImGUI data structures in a potentially multi-threaded manner.
struct IMGUIContext
{
    SL_PLUGIN_CONTEXT_CREATE_DESTROY(IMGUIContext);
    void onCreateContext() {};

    // Called when plugin is unloaded, destroy any objects on heap here
    void onDestroyContext() {};

    // Compute API
    RenderAPI platform = RenderAPI::eD3D12;
    chi::ICompute* compute{};

    uint32_t currentFrame = 0;
    uint32_t lastRenderedFrame = 0;

    std::vector<RenderCallback>* windowCallbacks{};
    std::vector<RenderCallback>* anywhereCallbacks{};

    sl::imgui::ImGUI ui{};
    sl::imgui::ImGUI::Fonts fonts{};
    
    float uiSmallFontSize = DEFAULT_SMALL_FONT_SIZE;
    float uiMediumFontSize = DEFAULT_MEDIUM_FONT_SIZE;

    std::array<void*, NUM_BACK_BUFFERS> backBuffers = {};
    BOOL  dxgiFullscreenState{};

    ID3D12Device* device{};
    ID3D12DescriptorHeap* pd3dRtvDescHeap{};
    ID3D12DescriptorHeap* pd3dSrvDescHeap{};
    D3D12_CPU_DESCRIPTOR_HANDLE  mainRenderTargetDescriptor[NUM_BACK_BUFFERS]{};

    VkImageView                                vkImageViews[NUM_BACK_BUFFERS]{};
    VkFramebuffer                              vkFrameBuffers[NUM_BACK_BUFFERS]{};
    ImGui_ImplVulkan_InitInfo                  vkInfo{};
    std::vector<std::pair<VkSurfaceKHR, HWND>> surfaceWindows{};
    VkSwapchainCreateInfoKHR                   vkSwapChainParams{};
    std::vector<VkImage> vkSwapchainImages{};
    HHOOK inputHook = nullptr;

    bool                      renderInternal         = true;   // true = render in this plugin, false = render in dlss_g
    chi::ICommandListContext* cmdList                = nullptr;
    chi::CommandQueue         cmdQueue               = nullptr;
    ID3D12Resource*           currentBackBuffer      = nullptr;
    VkImage                   currentBackBufferVk    = nullptr;
    uint32_t                  currentBackBufferIndex = 0;
    uint32_t                  backBufferFormat       = 0;
    uint32_t                  backBufferWidth        = 0;
    uint32_t                  backBufferHeight       = 0;
    HWND                      backBufferHwnd         = {};
    uint32_t                  bufferCount            = 0;
    
    extra::AverageValueMeter  frameMeter             = {};
    VkSwapchainKHR vkSwapchain{};
    std::unordered_map<VkSwapchainKHR, VKSwapchainInfo> vkSwapchainInfoMap{};
};
}



void updateEmbeddedJSON(json& config);

//! Embedded JSON, containing information about the plugin and the hooks it requires.
static std::string JSON = std::string(imgui_json, &imgui_json[imgui_json_len]);

//! Define our plugin, make sure to update version numbers in versions.h
SL_PLUGIN_DEFINE("sl.imgui", Version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH), Version(0, 0, 1), JSON.c_str(), updateEmbeddedJSON, imgui, IMGUIContext)

namespace imgui
{

struct Context
{
    ImGuiContext* imgui;
    ImPlotContext* plot;
    uint32_t backBufferWidth{};
    uint32_t backBufferHeight{};

    void* apiData{};

    DrawData drawData;
    std::vector<DrawList> drawLists;
    std::vector<std::vector<DrawCommand>> drawCommands;

    struct Button
    {
        bool pressed = false;
        bool released = false;
        bool down = false; // Last botton state in this frame
    };
    std::array<Button, 3> mouseEvents;
};

Context* g_ctx = {};

static ::ImVec2& toImVec2(type::Float2& v)
{
    return *reinterpret_cast<::ImVec2*>(&v);
}

static const ::ImVec2& toImVec2(const Float2& v)
{
    return *reinterpret_cast<const ::ImVec2*>(&v);
}

static Float2& toFloat2(::ImVec2& v)
{
    return *reinterpret_cast<Float2*>(&v);
}

static const Float2& toFloat2(const ::ImVec2& v)
{
    return *reinterpret_cast<const Float2*>(&v);
}

static ::ImVec4& toImVec4(Float4& v)
{
    return *reinterpret_cast<::ImVec4*>(&v);
}

static const ::ImVec4& toImVec4(const Float4& v)
{
    return *reinterpret_cast<const ::ImVec4*>(&v);
}

static Float4& toFloat4(::ImVec4& v)
{
    return *reinterpret_cast<Float4*>(&v);
}

static const Float4& toFloat4(const ::ImVec4& v)
{
    return *reinterpret_cast<const Float4*>(&v);
}


Context* createContext(const ContextDesc& desc)
{
    auto& ctx = (*sl::imgui::getContext());

    auto plotCtx = ImPlot::CreateContext();
    auto imguiCtx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;         // We can honor GetMouseCursor() values (optional)
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;          // We can honor io.WantSetMousePos requests (optional, rarely used)
    
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    //io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
    //io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
    //io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    //ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    // Guess DPI scale from window size
    float contentScale = ImGui_ImplWin32_GetDpiScaleForHwnd(desc.hWnd);
    
    // We don't want imgui to take ownership of the underlying host memory for "DroidSans_Mono_ttf",
    // so we flag the atlas accordingly. This is what allows us to destroy & recreate the imgui context without crashing
    FontConfig font_cfg;
    font_cfg.fontDataOwnedByAtlas = false;
    ctx.fonts.uiSmallFont = ctx.ui.addFontFromMemoryTTF(DroidSans_Mono_ttf, DroidSans_Mono_ttf_len, ctx.uiSmallFontSize * contentScale, &font_cfg, nullptr);
    ctx.fonts.uiMediumFont = ctx.ui.addFontFromMemoryTTF(DroidSans_Mono_ttf, DroidSans_Mono_ttf_len, ctx.uiMediumFontSize * contentScale, &font_cfg, nullptr);

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    style.ScaleAllSizes(contentScale);


    //io.DisplaySize.x = (float)desc.width;
    //io.DisplaySize.y = (float)desc.height;
    
    //io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    //io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
    io.IniFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    io.KeyMap[::ImGuiKey_Tab] = (uint32_t)sl::input::KeyValue::eTab;
    io.KeyMap[::ImGuiKey_LeftArrow] = (uint32_t)sl::input::KeyValue::eLeft;
    io.KeyMap[::ImGuiKey_RightArrow] = (uint32_t)sl::input::KeyValue::eRight;
    io.KeyMap[::ImGuiKey_UpArrow] = (uint32_t)sl::input::KeyValue::eUp;
    io.KeyMap[::ImGuiKey_DownArrow] = (uint32_t)sl::input::KeyValue::eDown;
    io.KeyMap[::ImGuiKey_PageUp] = (uint32_t)sl::input::KeyValue::ePageUp;
    io.KeyMap[::ImGuiKey_PageDown] = (uint32_t)sl::input::KeyValue::ePageDown;
    io.KeyMap[::ImGuiKey_Home] = (uint32_t)sl::input::KeyValue::eHome;
    io.KeyMap[::ImGuiKey_End] = (uint32_t)sl::input::KeyValue::eEnd;
    io.KeyMap[::ImGuiKey_Delete] = (uint32_t)sl::input::KeyValue::eDel;
    io.KeyMap[::ImGuiKey_Backspace] = (uint32_t)sl::input::KeyValue::eBackspace;
    io.KeyMap[::ImGuiKey_Enter] = (uint32_t)sl::input::KeyValue::eEnter;
    io.KeyMap[::ImGuiKey_Escape] = (uint32_t)sl::input::KeyValue::eEscape;
    io.KeyMap[::ImGuiKey_Space] = (uint32_t)sl::input::KeyValue::eSpace;
    io.KeyMap[::ImGuiKey_A] = (uint32_t)sl::input::KeyValue::eA;
    io.KeyMap[::ImGuiKey_C] = (uint32_t)sl::input::KeyValue::eC;
    io.KeyMap[::ImGuiKey_V] = (uint32_t)sl::input::KeyValue::eV;
    io.KeyMap[::ImGuiKey_X] = (uint32_t)sl::input::KeyValue::eX;
    io.KeyMap[::ImGuiKey_Y] = (uint32_t)sl::input::KeyValue::eY;
    io.KeyMap[::ImGuiKey_Z] = (uint32_t)sl::input::KeyValue::eZ;

    ImGui_ImplWin32_Init(desc.hWnd);

    void* apiData{};

    if (ctx.platform == RenderAPI::eD3D12 || ctx.platform == RenderAPI::eD3D11)
    {
        // In both cases we use D3D12
        chi::Device device{};
        ctx.compute->getDevice(device);
        auto d3d12Device = (ID3D12Device*)device;
        ctx.device = d3d12Device;

        if(!ctx.pd3dRtvDescHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = NUM_BACK_BUFFERS;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;
            if (d3d12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&ctx.pd3dRtvDescHeap)) != S_OK) return nullptr;

            SIZE_T rtvDescriptorSize = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = ctx.pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < NUM_BACK_BUFFERS; i++)
            {
                ctx.mainRenderTargetDescriptor[i] = rtvHandle;
                rtvHandle.ptr += rtvDescriptorSize;
            }
        }

        if(!ctx.pd3dSrvDescHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = 1;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (d3d12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&ctx.pd3dSrvDescHeap)) != S_OK)  return nullptr;
        }

        ImGui_ImplDX12_Init(d3d12Device, NUM_BACK_BUFFERS,
            (DXGI_FORMAT)desc.backBufferFormat, ctx.pd3dSrvDescHeap,
            ctx.pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
            ctx.pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());
    }
    else
    {
        // VK path
        chi::Device device;
        chi::PhysicalDevice pdevice;
        chi::Instance instance;
        ctx.compute->getDevice(device);
        ctx.compute->getPhysicalDevice(pdevice);
        ctx.compute->getInstance(instance);
        ImGui_ImplVulkan_InitInfo info{};
        info.Instance = (VkInstance)instance;
        info.Device = (VkDevice)device;
        info.PhysicalDevice = (VkPhysicalDevice)pdevice;
        info.Format = (VkFormat)desc.backBufferFormat;
        info.ImageCount = NUM_BACK_BUFFERS;
        info.MinImageCount = NUM_BACK_BUFFERS;

        // Create the Render Pass
        VkRenderPass renderPass{};
        {
            VkAttachmentDescription attachment = {};
            attachment.format = info.Format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference color_attachment = {};
            color_attachment.attachment = 0;
            color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment;
            VkSubpassDependency dependency = {};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            createInfo.attachmentCount = 1;
            createInfo.pAttachments = &attachment;
            createInfo.subpassCount = 1;
            createInfo.pSubpasses = &subpass;
            createInfo.dependencyCount = 1;
            createInfo.pDependencies = &dependency;
            vkCreateRenderPass(info.Device, &createInfo, nullptr, &renderPass);
        }

        ImGui_ImplVulkan_Init(&info, renderPass);

        ctx.vkInfo = info;

        apiData = renderPass;
    }
    
    return new Context{ imguiCtx, plotCtx, desc.width, desc.height, apiData };
}

void destroyContext(Context* ctx)
{
    (void)(ctx);
    auto& pluginCtx = (*sl::imgui::getContext());

    if (pluginCtx.platform == RenderAPI::eVulkan)
    {
        if (pluginCtx.vkInfo.Device != NULL)
        {
            for (uint32_t i = 0; i < NUM_BACK_BUFFERS; i++)
            {
                vkDestroyFramebuffer(pluginCtx.vkInfo.Device, pluginCtx.vkFrameBuffers[i], nullptr);
                pluginCtx.vkFrameBuffers[i] = VK_NULL_HANDLE;
                vkDestroyImageView(pluginCtx.vkInfo.Device, pluginCtx.vkImageViews[i], nullptr);
                pluginCtx.vkImageViews[i] = VK_NULL_HANDLE;
            }
            vkDestroyRenderPass(pluginCtx.vkInfo.Device, (VkRenderPass)(g_ctx->apiData), NULL);
            g_ctx->apiData = VK_NULL_HANDLE;
        }

        pluginCtx.vkSwapchainImages.clear();

        ImGui_ImplVulkan_Shutdown();
    }
    else
    {
        ImGui_ImplDX12_Shutdown();
        SL_SAFE_RELEASE(pluginCtx.pd3dRtvDescHeap);
        SL_SAFE_RELEASE(pluginCtx.pd3dSrvDescHeap);
    }

    if (pluginCtx.cmdList != nullptr)
    {
        pluginCtx.compute->destroyCommandListContext(pluginCtx.cmdList);
    }
    pluginCtx.cmdList = nullptr;

    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext(g_ctx->imgui);
    ImPlot::DestroyContext(g_ctx->plot);

    delete g_ctx;
    g_ctx = {};

    pluginCtx.backBuffers[0] = {};
    pluginCtx.backBuffers[1] = {};
    pluginCtx.backBuffers[2] = {};

    pluginCtx.currentFrame = 0;
}

void setCurrentContext(Context* ctx)
{
    ImGui::SetCurrentContext(ctx->imgui);
    ImPlot::SetCurrentContext(ctx->plot);
    g_ctx = ctx;
}

uint8_t* getFontAtlasPixels(int32_t& width, int32_t& height)
{
    unsigned char* pixels;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    return pixels;
}

void newFrame(float elapsedTime)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.DeltaTime = elapsedTime;

    auto& ctx = (*sl::imgui::getContext());
    if (ctx.platform == RenderAPI::eD3D12 || ctx.platform == RenderAPI::eD3D11)
    {
        ImGui_ImplDX12_NewFrame();
    }
    else
    {
        ImGui_ImplVulkan_NewFrame();
    }

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    ctx.currentFrame++;
}

void render(void* commandList, void* backBuffer, uint32_t index)
{
    auto& ctx = (*imgui::getContext());

    if (ctx.platform == RenderAPI::eD3D12 || ctx.platform == RenderAPI::eD3D11)
    {
        auto cmdList = (ID3D12GraphicsCommandList*)commandList;
        ID3D12Resource* resource = (ID3D12Resource*)backBuffer;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        //ImVec4 clearColor = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

        if (ctx.backBuffers[index] != backBuffer)
        {
            ctx.backBuffers[index] = backBuffer;
            ctx.device->CreateRenderTargetView(resource, nullptr, ctx.mainRenderTargetDescriptor[index]);
        }

        cmdList->ResourceBarrier(1, &barrier);
        //cmdList->ClearRenderTargetView(ctx.mainRenderTargetDescriptor[index], (float*)&clearColor, 0, NULL);
        cmdList->OMSetRenderTargets(1, &ctx.mainRenderTargetDescriptor[index], FALSE, NULL);
        cmdList->SetDescriptorHeaps(1, &ctx.pd3dSrvDescHeap);

        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &barrier);
    }
    else
    {
        auto cmdBuffer = (VkCommandBuffer)commandList;
        
        ImGuiIO& io = ImGui::GetIO();

        if (ctx.backBuffers[index] != backBuffer)
        {
            ctx.backBuffers[index] = backBuffer;

            // Create The Image Views
            {
                VkImageViewCreateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                info.format = ctx.vkInfo.Format;
                info.components.r = VK_COMPONENT_SWIZZLE_R;
                info.components.g = VK_COMPONENT_SWIZZLE_G;
                info.components.b = VK_COMPONENT_SWIZZLE_B;
                info.components.a = VK_COMPONENT_SWIZZLE_A;
                VkImageSubresourceRange image_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                info.subresourceRange = image_range;
                info.image = (VkImage)backBuffer;
                assert(ctx.vkImageViews[index] == nullptr);
                vkCreateImageView(ctx.vkInfo.Device, &info, nullptr, &ctx.vkImageViews[index]);
            }

            // Create Framebuffer
            {
                VkImageView attachment[1];
                VkFramebufferCreateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                info.renderPass = (VkRenderPass)g_ctx->apiData;
                info.attachmentCount = 1;
                info.pAttachments = attachment;
                info.width = (uint32_t)io.DisplaySize.x;
                info.height = (uint32_t)io.DisplaySize.y;
                info.layers = 1;
                attachment[0] = ctx.vkImageViews[index];
                assert(ctx.vkFrameBuffers[index] == nullptr);
                vkCreateFramebuffer(ctx.vkInfo.Device, &info, nullptr, &ctx.vkFrameBuffers[index]);
            }
        }

        ImGui_ImplVulkan_CreateFontsTexture(cmdBuffer);

        ImGui::Render();

        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = (VkRenderPass)g_ctx->apiData;
        info.framebuffer = ctx.vkFrameBuffers[index];
        info.renderArea.extent.width = (uint32_t)io.DisplaySize.x;
        info.renderArea.extent.height = (uint32_t)io.DisplaySize.y;
        info.clearValueCount = 0;
        info.pClearValues = nullptr;
        vkCmdBeginRenderPass(cmdBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdBuffer);

        vkCmdEndRenderPass(cmdBuffer);
    }
}

void plotGraph(const Graph& graph, const std::vector<GraphValues>& values)
{
    if (ImPlot::BeginPlot(graph.title))
    {
        uint32_t shade_mode = 0;
        float fill_ref = 0;
        ImPlotShadedFlags flags = 0;
        ImPlot::SetupAxes(graph.xAxisLabel, graph.yAxisLabel, ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels, 0);
        ImPlot::SetupAxesLimits(graph.minX, graph.maxX, graph.minY, graph.maxY, ImPlotCond_Always);
        {
            ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.25f);
            for (auto& v : values)
            {
                if (v.flags & GraphFlags::eShaded)
                {
                    ImPlot::PlotShaded(v.label, graph.xAxis, v.yAxis, v.numValues, shade_mode == 0 ? -INFINITY : shade_mode == 1 ? INFINITY : fill_ref, flags);
                }
                ImPlot::PlotLine(v.label, graph.xAxis, v.yAxis, v.numValues);
            }
        }
        if (graph.extraLabel)
        {
            ImPlot::PlotDummy(graph.extraLabel);
        }
        ImPlot::EndPlot();
    }
}

void toggleRenderInternal(bool val)
{
    auto& ctx = (*imgui::getContext());
    ctx.renderInternal = val;
    ctx.currentFrame = 0;
    if (ctx.renderInternal)
    {
        ctx.frameMeter.reset();
    }
}

bool getRenderInternal()
{
    auto& ctx = (*imgui::getContext());
    return ctx.renderInternal;
}

Context* getCurrentContext()
{
    return g_ctx;
}

ImGUI::Fonts getFonts()
{
    return imgui::getContext()->fonts;
}

void triggerRenderWindowCallbacks(bool finalFrame)
{
    auto& ctx = (*imgui::getContext());
    if (!ctx.windowCallbacks) return;
    for (auto fun : *ctx.windowCallbacks)
    {
        fun(&ctx.ui, finalFrame);
    }
}

void triggerRenderAnywhereCallbacks(bool finalFrame)
{
    auto& ctx = (*imgui::getContext());
    if (!ctx.anywhereCallbacks) return;
    for (auto fun : *ctx.anywhereCallbacks)
    {
        fun(&ctx.ui, finalFrame);
    }
}

void registerRenderCallbacks(RenderCallback window, RenderCallback anywhere)
{
    auto& ctx = (*imgui::getContext());
    if (window)
    {
        if (!ctx.windowCallbacks) ctx.windowCallbacks = new std::vector<RenderCallback>;
        ctx.windowCallbacks->push_back(window);
    }
    if (anywhere)
    {
        if (!ctx.anywhereCallbacks) ctx.anywhereCallbacks = new std::vector<RenderCallback>;
        ctx.anywhereCallbacks->push_back(anywhere);
    }
}

const DrawData& getDrawData()
{
    assert(g_ctx && "missing current context");

    auto imDrawData = ::ImGui::GetDrawData();

    g_ctx->drawData.displayPos = { imDrawData->DisplayPos.x, imDrawData->DisplayPos.y };
    g_ctx->drawData.displaySize = { imDrawData->DisplaySize.x, imDrawData->DisplaySize.y };
    g_ctx->drawData.framebufferScale = { imDrawData->FramebufferScale.x, imDrawData->FramebufferScale.y };
    g_ctx->drawData.indexCount = imDrawData->TotalIdxCount;
    g_ctx->drawData.vertexCount = imDrawData->TotalVtxCount;
    g_ctx->drawLists.resize(imDrawData->CmdListsCount);
    g_ctx->drawCommands.resize(imDrawData->CmdListsCount);
    for (int i = 0; i < imDrawData->CmdListsCount; i++)
    {
        const ::ImDrawList* imCmdList = imDrawData->CmdLists[i];
        DrawList& drawList = g_ctx->drawLists[i];
        std::vector<DrawCommand>& drawCommands = g_ctx->drawCommands[i];
        drawCommands.resize(imCmdList->CmdBuffer.Size);

        for (int32_t cmd = 0; cmd < imCmdList->CmdBuffer.Size; cmd++)
        {
            const ::ImDrawCmd* pCmd = &imCmdList->CmdBuffer[cmd];
            type::Float4 carbClip;
            memcpy(&carbClip, &pCmd->ClipRect, sizeof(type::Float4));
            drawCommands[cmd] = DrawCommand{ pCmd->ElemCount, carbClip, pCmd->TextureId, (DrawCallback)pCmd->UserCallback, pCmd->UserCallbackData };
        }
        drawList.commandBufferCount = (uint32_t)drawCommands.size();
        drawList.commandBuffers = drawCommands.data();
        drawList.indexBufferSize = imCmdList->IdxBuffer.Size;
        drawList.indexBuffer = imCmdList->IdxBuffer.Data;
        drawList.vertexBufferSize = imCmdList->VtxBuffer.Size;
        drawList.vertexBuffer = (DrawVertex*)imCmdList->VtxBuffer.Data;
    }
    g_ctx->drawData.commandListCount = (uint32_t)g_ctx->drawLists.size();
    g_ctx->drawData.commandLists = g_ctx->drawLists.data();

    return g_ctx->drawData;
}

static void setDisplaySize(type::Float2 size)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = size.x;
    io.DisplaySize.y = size.y;
}

static type::Float2 getDisplaySize()
{
    ImGuiIO& io = ImGui::GetIO();
    return { io.DisplaySize.x, io.DisplaySize.y };
}

static Style* getStyle()
{
    return (Style*)(&::ImGui::GetStyle());
}

static void showDemoWindow(bool* open)
{
    ::ImGui::ShowDemoWindow(open);
}

static void showMetricsWindow(bool* open)
{
    ::ImGui::ShowMetricsWindow(open);
}

static void showStyleEditor(Style* style)
{
    ::ImGui::ShowStyleEditor((ImGuiStyle*)style);
}

static bool showStyleSelector(const char* label)
{
    return ::ImGui::ShowStyleSelector(label);
}

static void showFontSelector(const char* label)
{
    ::ImGui::ShowFontSelector(label);
}

static void showUserGuide()
{
    ::ImGui::ShowUserGuide();
}

static const char* getImGuiVersion()
{
    return ::ImGui::GetVersion();
}

static void setStyleSize(Style* style)
{
    ImGuiStyle& s = *(ImGuiStyle*)style;

    // Settings
    s.WindowPadding = ImVec2(8.0f, 8.0f);
    s.PopupRounding = 4.0f;
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.ItemSpacing = ImVec2(6.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    s.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    s.IndentSpacing = 21.0f;
    s.ScrollbarSize = 16.0f;
    s.GrabMinSize = 8.0f;

    // BorderSize
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;

    // Rounding
    s.WindowRounding = 2.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;

    // Alignment
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.ButtonTextAlign = ImVec2(0.48f, 0.5f);

    s.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);
}

static void setStyleColors(Style* style, StyleColorsPreset preset)
{
    if (!style)
        style = getStyle();

    switch (preset)
    {
    case StyleColorsPreset::eNvidiaDark:
    {
        ImGuiStyle& s = *(ImGuiStyle*)style;

        // Common nvidia size attribute
        setStyleSize(style);

        // Colors
        s.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
        s.Colors[::ImGuiCol_TextDisabled] = ImVec4(0.43f, 0.43f, 0.43f, 1.00f);
        s.Colors[::ImGuiCol_WindowBg] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_ChildBg] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_PopupBg] = ImVec4(0.22f, 0.23f, 0.24f, 1.00f);
        s.Colors[::ImGuiCol_Border] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_BorderShadow] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_FrameBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        s.Colors[::ImGuiCol_TitleBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_MenuBarBg] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_ScrollbarBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_ScrollbarGrab] = ImVec4(0.62f, 0.62f, 0.62f, 1.00f);
        s.Colors[::ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 0.99f, 0.99f, 0.58f);
        s.Colors[::ImGuiCol_ScrollbarGrabActive] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
        s.Colors[::ImGuiCol_CheckMark] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
        s.Colors[::ImGuiCol_SliderGrab] = ImVec4(0.62f, 0.62f, 0.62f, 1.00f);
        s.Colors[::ImGuiCol_SliderGrabActive] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
        s.Colors[::ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        s.Colors[::ImGuiCol_ButtonHovered] = ImVec4(0.62f, 0.62f, 0.62f, 1.00f);
        s.Colors[::ImGuiCol_ButtonActive] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
        s.Colors[::ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        s.Colors[::ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        s.Colors[::ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        s.Colors[::ImGuiCol_Separator] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        s.Colors[::ImGuiCol_SeparatorHovered] = ImVec4(0.23f, 0.44f, 0.69f, 1.00f);
        s.Colors[::ImGuiCol_SeparatorActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        s.Colors[::ImGuiCol_ResizeGrip] = ImVec4(0.47f, 0.53f, 0.54f, 0.76f);
        s.Colors[::ImGuiCol_ResizeGripHovered] = ImVec4(0.23f, 0.44f, 0.69f, 1.00f);
        s.Colors[::ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        s.Colors[::ImGuiCol_Tab] = ImVec4(0.22f, 0.23f, 0.24f, 1.00f);
        s.Colors[::ImGuiCol_TabHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        s.Colors[::ImGuiCol_TabActive] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_TabUnfocused] = ImVec4(0.22f, 0.23f, 0.24f, 1.00f);
        s.Colors[::ImGuiCol_TabUnfocusedActive] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
        s.Colors[::ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
        s.Colors[::ImGuiCol_DockingEmptyBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        s.Colors[::ImGuiCol_PlotLines] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f); // TODO FIXME Steal it for swapChain clear
                                                                             // color!!
        s.Colors[::ImGuiCol_PlotLinesHovered] = ImVec4(0.22f, 0.23f, 0.24f, 1.00f); // TODO FIXME Steal it for menu
                                                                                    // background color!!
        s.Colors[::ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        s.Colors[::ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        s.Colors[::ImGuiCol_TextSelectedBg] = ImVec4(0.97f, 0.97f, 0.97f, 0.19f);
        s.Colors[::ImGuiCol_DragDropTarget] = ImVec4(0.38f, 0.62f, 0.80f, 1.00f);
        s.Colors[::ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        s.Colors[::ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        s.Colors[::ImGuiCol_NavWindowingDimBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        s.Colors[::ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
        break;
    }
    case StyleColorsPreset::eNvidiaLight:
    {
        ImGuiStyle& s = *(ImGuiStyle*)style;

        // Common nvidia size attribute
        setStyleSize(style);
        s.WindowBorderSize = 0.0f;
        s.ChildBorderSize = 0.0f;
        s.PopupBorderSize = 0.0f;
        s.FrameBorderSize = 0.0f;

        // Colors
        s.Colors[::ImGuiCol_Text] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
        s.Colors[::ImGuiCol_TextDisabled] = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
        s.Colors[::ImGuiCol_WindowBg] = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
        s.Colors[::ImGuiCol_ChildBg] = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
        s.Colors[::ImGuiCol_PopupBg] = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
        s.Colors[::ImGuiCol_Border] = ImVec4(0.79f, 0.79f, 0.79f, 1.00f);
        s.Colors[::ImGuiCol_BorderShadow] = ImVec4(0.79f, 0.79f, 0.79f, 1.00f);
        s.Colors[::ImGuiCol_FrameBg] = ImVec4(0.79f, 0.79f, 0.79f, 1.00f);
        s.Colors[::ImGuiCol_FrameBgHovered] = ImVec4(0.49f, 0.49f, 0.49f, 0.80f);
        s.Colors[::ImGuiCol_FrameBgActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
        s.Colors[::ImGuiCol_TitleBg] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
        s.Colors[::ImGuiCol_TitleBgActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
        s.Colors[::ImGuiCol_TitleBgCollapsed] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
        s.Colors[::ImGuiCol_MenuBarBg] = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
        s.Colors[::ImGuiCol_ScrollbarBg] = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
        s.Colors[::ImGuiCol_ScrollbarGrab] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
        s.Colors[::ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.49f, 0.49f, 0.49f, 0.80f);
        s.Colors[::ImGuiCol_ScrollbarGrabActive] = ImVec4(0.49f, 0.49f, 0.49f, 1.00f);
        s.Colors[::ImGuiCol_CheckMark] = ImVec4(0.43f, 0.43f, 0.43f, 1.00f);
        s.Colors[::ImGuiCol_SliderGrab] = ImVec4(0.43f, 0.43f, 0.43f, 0.00f);
        s.Colors[::ImGuiCol_SliderGrabActive] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
        s.Colors[::ImGuiCol_Button] = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
        s.Colors[::ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
        s.Colors[::ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
        s.Colors[::ImGuiCol_Header] = ImVec4(0.84f, 0.84f, 0.84f, 1.00f);
        s.Colors[::ImGuiCol_HeaderHovered] = ImVec4(0.749f, 0.80f, 0.812f, 1.00f);
        s.Colors[::ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
        s.Colors[::ImGuiCol_Separator] = ImVec4(0.40f, 0.50f, 0.60f, 0.00f);
        s.Colors[::ImGuiCol_SeparatorHovered] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
        s.Colors[::ImGuiCol_SeparatorActive] = ImVec4(0.50f, 0.60f, 0.70f, 0.00f);
        s.Colors[::ImGuiCol_ResizeGrip] = ImVec4(0.80f, 0.80f, 0.80f, 0.56f);
        s.Colors[::ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
        s.Colors[::ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
        s.Colors[::ImGuiCol_Tab] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
        s.Colors[::ImGuiCol_TabHovered] = ImVec4(0.84f, 0.84f, 0.84f, 1.00f);
        s.Colors[::ImGuiCol_TabActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
        s.Colors[::ImGuiCol_TabUnfocused] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
        s.Colors[::ImGuiCol_TabUnfocusedActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
        s.Colors[::ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.59f, 0.98f, 0.22f);
        s.Colors[::ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        s.Colors[::ImGuiCol_PlotLines] = ImVec4(0.878f, 0.878f, 0.878f, 1.00f); // TODO FIXME Steal it for swapChain
                                                                                // clear color!!
        s.Colors[::ImGuiCol_PlotLinesHovered] = ImVec4(0.839f, 0.839f, 0.839f, 1.00f); // TODO FIXME Steal it for menu
                                                                                       // background color!!
        s.Colors[::ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        s.Colors[::ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.45f, 0.00f, 1.00f);
        s.Colors[::ImGuiCol_TextSelectedBg] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
        s.Colors[::ImGuiCol_DragDropTarget] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
        s.Colors[::ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
        s.Colors[::ImGuiCol_NavWindowingHighlight] = ImVec4(0.70f, 0.70f, 0.70f, 0.70f);
        s.Colors[::ImGuiCol_NavWindowingDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
        s.Colors[::ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
        break;
    }
    case StyleColorsPreset::eClassic:
        ::ImGui::StyleColorsClassic((ImGuiStyle*)style);
        break;
    case StyleColorsPreset::eDark:
        ::ImGui::StyleColorsDark((ImGuiStyle*)style);
        break;
    case StyleColorsPreset::eLight:
        ::ImGui::StyleColorsLight((ImGuiStyle*)style);
        break;

    default:
        assert(false);
    }
}

static bool begin(const char* label, bool* open, uint32_t windowFlags)
{
    return ::ImGui::Begin(label, open, static_cast<ImGuiWindowFlags>(windowFlags));
}

static void end()
{
    ::ImGui::End();
}

static bool beginChild(const char* strId, type::Float2 size, bool border, WindowFlags flags)
{
    return ::ImGui::BeginChild(strId, toImVec2(size), border, static_cast<ImGuiWindowFlags>(flags));
}

static bool beginChildId(uint32_t id, type::Float2 size, bool border, WindowFlags flags)
{
    return ::ImGui::BeginChild(id, toImVec2(size), border, static_cast<ImGuiWindowFlags>(flags));
}

static void endChild()
{
    ::ImGui::EndChild();
}

static bool isWindowAppearing()
{
    return ::ImGui::IsWindowAppearing();
}

static bool isWindowCollapsed()
{
    return ::ImGui::IsWindowCollapsed();
}

static bool isWindowFocused(FocusedFlags flags)
{
    return ::ImGui::IsWindowFocused(flags);
}

static bool isWindowHovered(HoveredFlags flags)
{
    return ::ImGui::IsWindowHovered(flags);
}

static DrawList* getWindowDrawList()
{
    return reinterpret_cast<DrawList*>(::ImGui::GetWindowDrawList());
}

static float getWindowDpiScale()
{
    return ::ImGui::GetWindowDpiScale();
}

static Float2 getWindowPos()
{
    return toFloat2(::ImGui::GetWindowPos());
}

static Float2 getWindowSize()
{
    return toFloat2(::ImGui::GetWindowSize());
}

static float getWindowWidth()
{
    return ::ImGui::GetWindowWidth();
}

static float getWindowHeight()
{
    return ::ImGui::GetWindowHeight();
}

static Float2 getContentRegionMax()
{
    return toFloat2(::ImGui::GetContentRegionMax());
}

static Float2 getContentRegionAvail()
{
    return toFloat2(::ImGui::GetContentRegionAvail());
}

static float getContentRegionAvailWidth()
{
    return ::ImGui::GetContentRegionAvailWidth();
}


static Float2 getWindowContentRegionMin()
{
    return toFloat2(::ImGui::GetWindowContentRegionMin());
}

static Float2 getWindowContentRegionMax()
{
    return toFloat2(::ImGui::GetWindowContentRegionMax());
}

static float getWindowContentRegionWidth()
{
    return ::ImGui::GetWindowContentRegionWidth();
}

static void setNextWindowPos(type::Float2 position, Condition cond, type::Float2 pivot)
{
    ::ImGui::SetNextWindowPos(toImVec2(position), (int)cond, toImVec2(pivot));
}

static void setNextWindowSize(type::Float2 size, Condition cond)
{
    ::ImGui::SetNextWindowSize(toImVec2(size), (int)cond);
}

static void setNextWindowSizeConstraints(const Float2& sizeMin, const Float2& sizeMax)
{
    ::ImGui::SetNextWindowSizeConstraints(toImVec2(sizeMin), toImVec2(sizeMax));
}

static void setNextWindowContentSize(const Float2& size)
{
    ::ImGui::SetNextWindowContentSize(toImVec2(size));
}

static void setNextWindowCollapsed(bool collapsed, Condition cond)
{
    ::ImGui::SetNextWindowCollapsed(collapsed, (int)cond);
}

static void setNextWindowFocus()
{
    ::ImGui::SetNextWindowFocus();
}

static void setNextWindowBgAlpha(float alpha)
{
    ::ImGui::SetNextWindowBgAlpha(alpha);
}

static void setWindowFontScale(float scale)
{
    ::ImGui::SetWindowFontScale(scale);
}

static void setWindowPos(const char* name, const Float2& pos, Condition cond)
{
    ::ImGui::SetWindowPos(name, toImVec2(pos), (int)cond);
}

static void setWindowSize(const char* name, const Float2& size, Condition cond)
{
    ::ImGui::SetWindowSize(name, toImVec2(size), (int)cond);
}

static void setWindowCollapsed(const char* name, bool collapsed, Condition cond)
{
    ::ImGui::SetWindowCollapsed(name, collapsed, (int)cond);
}

static void setWindowFocus(const char* name)
{
    ::ImGui::SetWindowFocus(name);
}

static float getScrollX()
{
    return ::ImGui::GetScrollX();
}

static float getScrollY()
{
    return ::ImGui::GetScrollY();
}

static float getScrollMaxX()
{
    return ::ImGui::GetScrollMaxX();
}

static float getScrollMaxY()
{
    return ::ImGui::GetScrollMaxY();
}

static void setScrollX(float scrollX)
{
    return ::ImGui::SetScrollX(scrollX);
}

static void setScrollY(float scrollY)
{
    return ::ImGui::SetScrollY(scrollY);
}

static void setScrollHereY(float centerYRatio)
{
    ::ImGui::SetScrollHereY(centerYRatio);
}

static void setScrollFromPosY(float posY, float centerYRatio)
{
    return ::ImGui::SetScrollFromPosY(posY, centerYRatio);
}

static void pushFont(Font* font)
{
    ::ImGui::PushFont((ImFont*)font);
}

static void popFont()
{
    ::ImGui::PopFont();
}

static void pushStyleColor(StyleColor colorIndex, type::Float4 color)
{
    ::ImGui::PushStyleColor(static_cast<ImGuiCol>(colorIndex), toImVec4(color));
}

static void popStyleColor()
{
    ::ImGui::PopStyleColor();
}

static void pushStyleVarFloat(StyleVar styleVarIndex, float value)
{
    ::ImGui::PushStyleVar(static_cast<::ImGuiStyleVar>(styleVarIndex), value);
}

static void pushStyleVarFloat2(StyleVar styleVarIndex, type::Float2 value)
{
    ::ImGui::PushStyleVar(static_cast<::ImGuiStyleVar>(styleVarIndex), toImVec2(value));
}

static void popStyleVar()
{
    ::ImGui::PopStyleVar();
}

static const Float4& getStyleColorVec4(StyleColor colorIndex)
{
    return toFloat4(::ImGui::GetStyleColorVec4(static_cast<ImGuiCol>(colorIndex)));
}

static Font* getFont()
{
    return (Font*)::ImGui::GetFont();
}

static float getFontSize()
{
    return ::ImGui::GetFontSize();
}

static Float2 getFontTexUvWhitePixel()
{
    return toFloat2(::ImGui::GetFontTexUvWhitePixel());
}

static uint32_t getColorU32StyleColor(StyleColor colorIndex, float alphaMul)
{
    return ::ImGui::GetColorU32(static_cast<ImGuiCol>(colorIndex), alphaMul);
}

uint32_t getColorU32Vec4(Float4 color)
{
    return ::ImGui::GetColorU32(toImVec4(color));
}

uint32_t getColorU32(uint32_t color)
{
    return ::ImGui::GetColorU32(color);
}

static void pushItemWidth(float width)
{
    ::ImGui::PushItemWidth(width);
}

static void popItemWidth()
{
    ::ImGui::PopItemWidth();
}

static type::Float2 calcItemSize(type::Float2 size, float defaultX, float defaultY)
{
    return toFloat2(::ImGui::CalcItemSize(toImVec2(size), defaultX, defaultY));
}

static float calcItemWidth()
{
    return ::ImGui::CalcItemWidth();
}

static void pushItemFlag(ItemFlags option, bool enabled)
{
    ::ImGui::PushItemFlag(option, enabled);
}

static void popItemFlag()
{
    ::ImGui::PopItemFlag();
}

static void pushTextWrapPos(float wrapPosX)
{
    ::ImGui::PushTextWrapPos(wrapPosX);
}

static void popTextWrapPos()
{
    ::ImGui::PopTextWrapPos();
}

static void pushAllowKeyboardFocus(bool allow)
{
    ::ImGui::PushAllowKeyboardFocus(allow);
}

static void popAllowKeyboardFocus()
{
    ::ImGui::PopAllowKeyboardFocus();
}

static void pushButtonRepeat(bool repeat)
{
    ::ImGui::PushButtonRepeat(repeat);
}

static void popButtonRepeat()
{
    ::ImGui::PopButtonRepeat();
}

static void separator()
{
    ::ImGui::Separator();
}

static void sameLineEx(float posX, float spacingW)
{
    ::ImGui::SameLine(posX, spacingW);
}

static void newLine()
{
    ::ImGui::NewLine();
}

static void spacing()
{
    ::ImGui::Spacing();
}

static void dummy(type::Float2 size)
{
    ::ImGui::Dummy(toImVec2(size));
}

static void indent(float indentWidth)
{
    ::ImGui::Indent(indentWidth);
}

static void unindent(float indentWidth)
{
    ::ImGui::Unindent(indentWidth);
}

static void beginGroup()
{
    ::ImGui::BeginGroup();
}

static void endGroup()
{
    ::ImGui::EndGroup();
}

static Float2 getCursorPos()
{
    return toFloat2(::ImGui::GetCursorPos());
}

static float getCursorPosX()
{
    return ::ImGui::GetCursorPosX();
}

static float getCursorPosY()
{
    return ::ImGui::GetCursorPosY();
}

static void setCursorPos(const Float2& localPos)
{
    ::ImGui::SetCursorPos(toImVec2(localPos));
}

static void setCursorPosX(float x)
{
    ::ImGui::SetCursorPosX(x);
}

static void setCursorPosY(float y)
{
    ::ImGui::SetCursorPosY(y);
}

static Float2 getCursorStartPos()
{
    return toFloat2(::ImGui::GetCursorStartPos());
}

static Float2 getCursorScreenPos()
{
    return toFloat2(::ImGui::GetCursorScreenPos());
}

static void setCursorScreenPos(const Float2& pos)
{
    return ::ImGui::SetCursorScreenPos(toImVec2(pos));
}

static void alignTextToFramePadding()
{
    return ::ImGui::AlignTextToFramePadding();
}

static float getTextLineHeight()
{
    return ::ImGui::GetTextLineHeight();
}

static float getTextLineHeightWithSpacing()
{
    return ::ImGui::GetTextLineHeightWithSpacing();
}

static float getFrameHeight()
{
    return ::ImGui::GetFrameHeight();
}

static float getFrameHeightWithSpacing()
{
    return ::ImGui::GetFrameHeightWithSpacing();
}

static void pushIdString(const char* id)
{
    ::ImGui::PushID(id);
}

static void pushIdStringBeginEnd(const char* idBegin, const char* idEnd)
{
    ::ImGui::PushID(idBegin, idEnd);
}

static void pushIdInt(int id)
{
    ::ImGui::PushID(id);
}

static void pushIdPtr(const void* ptr)
{
    ::ImGui::PushID(ptr);
}

static void popId()
{
    ::ImGui::PopID();
}

static uint32_t getIdString(const char* id)
{
    return ::ImGui::GetID(id);
}

static uint32_t getIdStringBeginEnd(const char* idBegin, const char* idEnd)
{
    return ::ImGui::GetID(idBegin, idEnd);
}

static uint32_t getIdPtr(const void* id)
{
    return ::ImGui::GetID(id);
}

static void textUnformatted(const char* text)
{
    ::ImGui::TextUnformatted(text);
}

static void text(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextV(fmt, args);
    va_end(args);
}

static void textColored(const Float4& color, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextColoredV(toImVec4(color), fmt, args);
    va_end(args);
}

static void labelColored(const Float4& color, const char* label, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::Text(label);
    ::ImGui::SameLine();
    ::ImGui::TextColoredV(toImVec4(color), fmt, args);
    va_end(args);
}

static void textDisabled(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextDisabledV(fmt, args);
    va_end(args);
}

static void textWrapped(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextWrappedV(fmt, args);
    va_end(args);
}

static void labelText(const char* label, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::LabelTextV(label, fmt, args);
    va_end(args);
}

static void bulletText(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ::ImGui::BulletTextV(fmt, args);
    va_end(args);
}

static bool buttonEx(const char* label, const Float2& size)
{
    return ::ImGui::Button(label, toImVec2(size));
}

static bool smallButton(const char* label)
{
    return ::ImGui::SmallButton(label);
}

static bool invisibleButton(const char* id, const Float2& size)
{
    return ::ImGui::InvisibleButton(id, toImVec2(size));
}

static bool arrowButton(const char* id, Direction dir)
{
    return ::ImGui::ArrowButton(id, (int)dir);
}

static void image(TextureId userTextureId,
    const Float2& size,
    const Float2& uv0,
    const Float2& uv1,
    const Float4& tintColor,
    const Float4& borderColor)
{
    return ::ImGui::Image((ImTextureID)userTextureId.ptr, toImVec2(size), toImVec2(uv0), toImVec2(uv1),
        toImVec4(tintColor), toImVec4(borderColor));
}

static bool imageButton(TextureId userTextureId,
    const Float2& size,
    const Float2& uv0,
    const Float2& uv1,
    int framePadding,
    const Float4& bgColor,
    const Float4& tintColor)
{
    return ::ImGui::ImageButton((ImTextureID)userTextureId.ptr, toImVec2(size), toImVec2(uv0), toImVec2(uv1),
        framePadding, toImVec4(bgColor), toImVec4(tintColor));
}

static bool checkbox(const char* label, bool* value)
{
    return ::ImGui::Checkbox(label, value);
}

static bool checkboxFlags(const char* label, uint32_t* flags, uint32_t flagsValue)
{
    return ::ImGui::CheckboxFlags(label, flags, flagsValue);
}

static bool radioButton(const char* label, bool active)
{
    return ::ImGui::RadioButton(label, active);
}

static bool radioButtonEx(const char* label, int* v, int vButton)
{
    return ::ImGui::RadioButton(label, v, vButton);
}

static void progressBar(float fraction, type::Float2 size, const char* overlay)
{
    ::ImGui::ProgressBar(fraction, toImVec2(size), overlay);
}

static void bullet()
{
    ::ImGui::Bullet();
}

static bool beginCombo(const char* label, const char* previewValue, ComboFlags flags)
{
    return ::ImGui::BeginCombo(label, previewValue, flags);
}

static void endCombo()
{
    ::ImGui::EndCombo();
}

static bool combo(const char* label, int* currentItem, const char* const* items, int itemCount)
{
    return ::ImGui::Combo(label, currentItem, items, itemCount);
}

static bool dragFloat(
    const char* label, float* v, float vSpeed, float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::DragFloat(label, v, vSpeed, vMin, vMax, displayFormat, power);
}

static bool dragFloat2(
    const char* label, float v[2], float vSpeed, float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::DragFloat2(label, v, vSpeed, vMin, vMax, displayFormat, power);
}

static bool dragFloat3(
    const char* label, float v[3], float vSpeed, float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::DragFloat3(label, v, vSpeed, vMin, vMax, displayFormat, power);
}

static bool dragFloat4(
    const char* label, float v[4], float vSpeed, float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::DragFloat4(label, v, vSpeed, vMin, vMax, displayFormat, power);
}

static bool dragFloatRange2(const char* label,
    float* vCurrentMin,
    float* vCurrentMax,
    float vSpeed,
    float vMin,
    float vMax,
    const char* displayFormat,
    const char* displayFormatMax,
    float power)
{
    return ::ImGui::DragFloatRange2(
        label, vCurrentMin, vCurrentMax, vSpeed, vMin, vMax, displayFormat, displayFormatMax, power);
}

static bool dragInt(const char* label, int* v, float vSpeed, int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::DragInt(label, v, vSpeed, vMin, vMax, displayFormat);
}

static bool dragInt2(const char* label, int v[2], float vSpeed, int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::DragInt2(label, v, vSpeed, vMin, vMax, displayFormat);
}

static bool dragInt3(const char* label, int v[3], float vSpeed, int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::DragInt3(label, v, vSpeed, vMin, vMax, displayFormat);
}

static bool dragInt4(const char* label, int v[4], float vSpeed, int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::DragInt4(label, v, vSpeed, vMin, vMax, displayFormat);
}

static bool dragIntRange2(const char* label,
    int* vCurrentMin,
    int* vCurrentMax,
    float vSpeed,
    int vMin,
    int vMax,
    const char* displayFormat,
    const char* displayFormatMax)
{
    return ::ImGui::DragIntRange2(label, vCurrentMin, vCurrentMax, vSpeed, vMin, vMax, displayFormat, displayFormatMax);
}

static bool dragScalar(const char* label,
    DataType dataType,
    void* v,
    float vSpeed,
    const void* vMin,
    const void* vMax,
    const char* displayFormat,
    float power)
{
    return ::ImGui::DragScalar(label, static_cast<ImGuiDataType_>(dataType), v, vSpeed, vMin, vMax, displayFormat, power);
}

static bool dragScalarN(const char* label,
    DataType dataType,
    void* v,
    int components,
    float vSpeed,
    const void* vMin,
    const void* vMax,
    const char* displayFormat,
    float power)
{
    return ::ImGui::DragScalarN(
        label, static_cast<ImGuiDataType_>(dataType), v, components, vSpeed, vMin, vMax, displayFormat, power);
}

static bool sliderFloat(const char* label, float* v, float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::SliderFloat(label, v, vMin, vMax, displayFormat, power);
}

static bool sliderFloat2(const char* label, float v[2], float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::SliderFloat2(label, v, vMin, vMax, displayFormat, power);
}

static bool sliderFloat3(const char* label, float v[3], float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::SliderFloat3(label, v, vMin, vMax, displayFormat, power);
}

static bool sliderFloat4(const char* label, float v[4], float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::SliderFloat4(label, v, vMin, vMax, displayFormat, power);
}

static bool sliderAngle(const char* label, float* vRad, float vDegreesMin, float vDegreesMax)
{
    return ::ImGui::SliderAngle(label, vRad, vDegreesMin, vDegreesMax);
}

static bool sliderInt(const char* label, int* v, int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::SliderInt(label, v, vMin, vMax, displayFormat);
}

static bool sliderInt2(const char* label, int v[2], int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::SliderInt2(label, v, vMin, vMax, displayFormat);
}

static bool sliderInt3(const char* label, int v[3], int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::SliderInt3(label, v, vMin, vMax, displayFormat);
}

static bool sliderInt4(const char* label, int v[4], int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::SliderInt4(label, v, vMin, vMax, displayFormat);
}

static bool sliderScalar(const char* label,
    DataType dataType,
    void* v,
    const void* vMin,
    const void* vMax,
    const char* displayFormat,
    float power)
{
    return ::ImGui::SliderScalar(label, static_cast<ImGuiDataType>(dataType), v, vMin, vMax, displayFormat, power);
}

static bool sliderScalarN(const char* label,
    DataType dataType,
    void* v,
    int components,
    const void* vMin,
    const void* vMax,
    const char* displayFormat,
    float power)
{
    return ::ImGui::SliderScalarN(
        label, static_cast<ImGuiDataType>(dataType), v, components, vMin, vMax, displayFormat, power);
}

static bool vSliderFloat(
    const char* label, const Float2& size, float* v, float vMin, float vMax, const char* displayFormat, float power)
{
    return ::ImGui::VSliderFloat(label, toImVec2(size), v, vMin, vMax, displayFormat, power);
}

static bool vSliderInt(const char* label, const Float2& size, int* v, int vMin, int vMax, const char* displayFormat)
{
    return ::ImGui::VSliderInt(label, toImVec2(size), v, vMin, vMax, displayFormat);
}

static bool vSliderScalar(const char* label,
    const Float2& size,
    DataType dataType,
    void* v,
    const void* vMin,
    const void* vMax,
    const char* displayFormat,
    float power)
{
    return ::ImGui::VSliderScalar(
        label, toImVec2(size), static_cast<ImGuiDataType>(dataType), v, vMin, vMax, displayFormat, power);
}

static bool inputText(
    const char* label, char* buf, size_t bufSize, InputTextFlags flags, TextEditCallback callback, void* userData)
{
    return ::ImGui::InputText(label, buf, bufSize, flags, (ImGuiTextEditCallback)callback, userData);
}

static bool inputTextWithHint(const char* label,
    const char* hint,
    char* buf,
    size_t bufSize,
    InputTextFlags flags,
    TextEditCallback callback,
    void* userData)
{
    return ::ImGui::InputTextWithHint(label, hint, buf, bufSize, flags, (ImGuiTextEditCallback)callback, userData);
}

static bool inputTextMultiline(const char* label,
    char* buf,
    size_t bufSize,
    const Float2& size,
    InputTextFlags flags,
    TextEditCallback callback,
    void* userData)
{
    return ::ImGui::InputTextMultiline(
        label, buf, bufSize, toImVec2(size), flags, (ImGuiTextEditCallback)callback, userData);
}

static bool inputFloat(const char* label, float* v, float step, float stepFast, int decimalPrecision, InputTextFlags extraFlags)
{
    return ::ImGui::InputFloat(label, v, step, stepFast, decimalPrecision, extraFlags);
}

static bool inputFloat2(const char* label, float v[2], int decimalPrecision, InputTextFlags extraFlags)
{
    return ::ImGui::InputFloat2(label, v, decimalPrecision, extraFlags);
}

static bool inputFloat3(const char* label, float v[3], int decimalPrecision, InputTextFlags extraFlags)
{
    return ::ImGui::InputFloat3(label, v, decimalPrecision, extraFlags);
}

static bool inputFloat4(const char* label, float v[4], int decimalPrecision, InputTextFlags extraFlags)
{
    return ::ImGui::InputFloat4(label, v, decimalPrecision, extraFlags);
}

static bool inputInt(const char* label, int* v, int step, int stepFast, InputTextFlags extraFlags)
{
    return ::ImGui::InputInt(label, v, step, stepFast, extraFlags);
}

static bool inputInt2(const char* label, int v[2], InputTextFlags extraFlags)
{
    return ::ImGui::InputInt2(label, v, extraFlags);
}

static bool inputInt3(const char* label, int v[3], InputTextFlags extraFlags)
{
    return ::ImGui::InputInt3(label, v, extraFlags);
}

static bool inputInt4(const char* label, int v[4], InputTextFlags extraFlags)
{
    return ::ImGui::InputInt4(label, v, extraFlags);
}

static bool inputDouble(
    const char* label, double* v, double step, double stepFast, const char* displayFormat, InputTextFlags extraFlags)
{
    return ::ImGui::InputDouble(label, v, step, stepFast, displayFormat, extraFlags);
}

static bool inputScalar(const char* label,
    DataType dataType,
    void* v,
    const void* step,
    const void* stepFast,
    const char* displayFormat,
    InputTextFlags extraFlags)
{
    return ::ImGui::InputScalar(
        label, static_cast<::ImGuiDataType_>(dataType), v, step, stepFast, displayFormat, extraFlags);
}


static bool inputScalarN(const char* label,
    DataType dataType,
    void* v,
    int components,
    const void* step,
    const void* stepFast,
    const char* displayFormat,
    InputTextFlags extraFlags)
{
    return ::ImGui::InputScalarN(
        label, static_cast<::ImGuiDataType_>(dataType), v, components, step, stepFast, displayFormat, extraFlags);
}

static bool colorEdit3(const char* label, float col[3], ColorEditFlags flags)
{
    return ::ImGui::ColorEdit3(label, col, flags);
}

static bool colorEdit4(const char* label, float col[4], ColorEditFlags flags)
{
    return ::ImGui::ColorEdit4(label, col, flags);
}

static bool colorPicker3(const char* label, float col[3], ColorEditFlags flags)
{
    return ::ImGui::ColorPicker3(label, col, flags);
}

static bool colorPicker4(const char* label, float col[4], ColorEditFlags flags, const float* refCol)
{
    return ::ImGui::ColorPicker4(label, col, flags, refCol);
}

static bool colorButton(const char* descId, const Float4& col, ColorEditFlags flags, Float2 size)
{
    return ::ImGui::ColorButton(descId, toImVec4(col), flags, toImVec2(size));
}

static void setColorEditOptions(ColorEditFlags flags)
{
    return ::ImGui::SetColorEditOptions(flags);
}

static bool treeNode(const char* label)
{
    return ::ImGui::TreeNode(label);
}

static bool treeNodeString(const char* strId, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const bool res = ::ImGui::TreeNodeV(strId, fmt, args);
    va_end(args);
    return res;
}

static bool treeNodePtr(const void* ptrId, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const bool res = ::ImGui::TreeNodeV(ptrId, fmt, args);
    va_end(args);
    return res;
}

static bool treeNodeEx(const char* label, TreeNodeFlags flags)
{
    return ::ImGui::TreeNodeEx(label, flags);
}

static bool treeNodeStringEx(const char* strId, TreeNodeFlags flags, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const bool res = ::ImGui::TreeNodeExV(strId, flags, fmt, args);
    va_end(args);
    return res;
}

static bool treeNodePtrEx(const void* ptrId, TreeNodeFlags flags, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const bool res = ::ImGui::TreeNodeExV(ptrId, flags, fmt, args);
    va_end(args);
    return res;
}

static void treePushString(const char* strId)
{
    ::ImGui::TreePush(strId);
}

static void treePushPtr(const void* ptrId)
{
    ::ImGui::TreePush(ptrId);
}

static void treePop()
{
    ::ImGui::TreePop();
}

static void treeAdvanceToLabelPos()
{
    ::ImGui::TreeAdvanceToLabelPos();
}

static float getTreeNodeToLabelSpacing()
{
    return ::ImGui::GetTreeNodeToLabelSpacing();
}

static void setNextTreeNodeOpen(bool isOpen, Condition cond)
{
    return ::ImGui::SetNextTreeNodeOpen(isOpen, (int)cond);
}

static bool collapsingHeader(const char* label, TreeNodeFlags flags)
{
    return ::ImGui::CollapsingHeader(label, flags);
}

static bool collapsingHeaderEx(const char* label, bool* pOpen, TreeNodeFlags flags)
{
    return ::ImGui::CollapsingHeader(label, pOpen, flags);
}

static bool selectable(const char* label,
    bool selected /* = false*/,
    SelectableFlags flags /* = 0*/,
    const Float2& size /* = Float2(0,0)*/)
{
    return ::ImGui::Selectable(label, selected, flags, toImVec2(size));
}

static bool selectableEx(const char* label,
    bool* pSelected,
    SelectableFlags flags /* = 0*/,
    const Float2& size /* = Float2(0,0)*/)
{
    return ::ImGui::Selectable(label, pSelected, flags, toImVec2(size));
}

static bool listBox(const char* label, int* currentItem, const char* const items[], int itemCount, int heightInItems /* = -1*/)
{
    return ::ImGui::ListBox(label, currentItem, items, itemCount, heightInItems);
}

static bool listBoxEx(const char* label,
    int* currentItem,
    bool (*itemsGetterFn)(void* data, int idx, const char** out_text),
    void* data,
    int itemCount,
    int heightInItems /* = -1*/)
{
    return ::ImGui::ListBox(label, currentItem, itemsGetterFn, data, itemCount, heightInItems);
}

static bool listBoxHeader(const char* label, const Float2& size /* = Float2(0,0)*/)
{
    return ::ImGui::ListBoxHeader(label, toImVec2(size));
}

static bool listBoxHeaderEx(const char* label, int itemCount, int heightInItems /* = -1*/)
{
    return ::ImGui::ListBoxHeader(label, itemCount, heightInItems);
}

static void listBoxFooter()
{
    ::ImGui::ListBoxFooter();
}

static void plotLines(const char* label,
    const float* values,
    int valuesCount,
    int valuesOffset,
    const char* overlayText,
    float scaleMin,
    float scaleMax,
    Float2 graphSize,
    int stride)
{
    return ::ImGui::PlotLines(
        label, values, valuesCount, valuesOffset, overlayText, scaleMin, scaleMax, toImVec2(graphSize), stride);
}

static void plotLinesEx(const char* label,
    float (*valuesGetterFn)(void* data, int idx),
    void* data,
    int valuesCount,
    int valuesOffset,
    const char* overlayText,
    float scaleMin,
    float scaleMax,
    Float2 graphSize)
{
    return ::ImGui::PlotLines(
        label, valuesGetterFn, data, valuesCount, valuesOffset, overlayText, scaleMin, scaleMax, toImVec2(graphSize));
}

static void plotHistogram(const char* label,
    const float* values,
    int valuesCount,
    int valuesOffset,
    const char* overlayText,
    float scaleMin,
    float scaleMax,
    Float2 graphSize,
    int stride)
{
    return ::ImGui::PlotHistogram(
        label, values, valuesCount, valuesOffset, overlayText, scaleMin, scaleMax, toImVec2(graphSize), stride);
}

static void plotHistogramEx(const char* label,
    float (*valuesGetterFn)(void* data, int idx),
    void* data,
    int valuesCount,
    int valuesOffset,
    const char* overlayText,
    float scaleMin,
    float scaleMax,
    Float2 graphSize)
{
    return ::ImGui::PlotHistogram(
        label, valuesGetterFn, data, valuesCount, valuesOffset, overlayText, scaleMin, scaleMax, toImVec2(graphSize));
}

static void valueBool(const char* prefix, bool b)
{
    ::ImGui::Value(prefix, b);
}

static void valueInt(const char* prefix, int v)
{
    ::ImGui::Value(prefix, v);
}

static void valueUInt32(const char* prefix, uint32_t v)
{
    ::ImGui::Value(prefix, v);
}

static void valueFloat(const char* prefix, float v, const char* floatFormat /* = nullptr*/)
{
    ::ImGui::Value(prefix, v, floatFormat);
}

static void setTooltip(const char* fmt, ...)
{
    // Hack: tooltip color to typical yellow tooltip color and black text
    ::ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.99f, 0.96f, 0.78f, 1));
    ::ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, .7f));
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));

    va_list args;
    va_start(args, fmt);
    ::ImGui::SetTooltipV(fmt, args);
    va_end(args);

    ::ImGui::PopStyleVar();
    ::ImGui::PopStyleColor();
    ::ImGui::PopStyleColor();
}

static void beginTooltip()
{
    ::ImGui::BeginTooltip();
}

static void endTooltip()
{
    ::ImGui::EndTooltip();
}

static bool beginMainMenuBar()
{
    return ::ImGui::BeginMainMenuBar();
}

static void endMainMenuBar()
{
    return ::ImGui::EndMainMenuBar();
}

static bool beginMenuBar()
{
    return ::ImGui::BeginMenuBar();
}

static void endMenuBar()
{
    return ::ImGui::EndMenuBar();
}

static bool beginMenu(const char* label, bool enabled /* = true*/)
{
    return ::ImGui::BeginMenu(label, enabled);
}

static void endMenu()
{
    return ::ImGui::EndMenu();
}

static bool menuItem(const char* label,
    const char* shortcut /* = NULL*/,
    bool selected /* = false*/,
    bool enabled /* = true*/)
{
    return ::ImGui::MenuItem(label, shortcut, selected, enabled);
}

static bool menuItemEx(const char* label, const char* shortcut, bool* pSelected, bool enabled /* = true*/)
{
    return ::ImGui::MenuItem(label, shortcut, pSelected, enabled);
}

static void openPopup(const char* strId)
{
    return ::ImGui::OpenPopup(strId);
}

static bool beginPopup(const char* strId, WindowFlags flags /* = 0*/)
{
    return ::ImGui::BeginPopup(strId, flags);
}

static bool beginPopupContextItem(const char* strId /* = NULL*/, int mouseButton /* = 1*/)
{
    return ::ImGui::BeginPopupContextItem(strId, mouseButton);
}

static bool beginPopupContextWindow(const char* strId /* = NULL*/, int mouseButton /* = 1*/, bool alsoOverItems)
{
    return ::ImGui::BeginPopupContextWindow(strId, mouseButton, alsoOverItems);
}

static bool beginPopupContextVoid(const char* strId /* = NULL*/, int mouseButton /* = 1*/)
{
    return ::ImGui::BeginPopupContextVoid(strId, mouseButton);
}

static bool beginPopupModal(const char* name, bool* pOpen /* = NULL*/, WindowFlags flags /* = 0*/)
{
    return ::ImGui::BeginPopupModal(name, pOpen, flags);
}

static void endPopup()
{
    ::ImGui::EndPopup();
}

static bool openPopupOnItemClick(const char* strId /* = NULL*/, int mouseButton /* = 1*/)
{
    return ::ImGui::OpenPopupOnItemClick(strId, mouseButton);
}

static bool isPopupOpen(const char* strId)
{
    return ::ImGui::IsPopupOpen(strId);
}

static bool isModalPopupOpen()
{
    return (::ImGui::GetTopMostPopupModal() != nullptr);
}

static void popStyleVarMultiple(int count)
{
    ::ImGui::PopStyleVar(count);
}

static bool beginTable(const char* str_id, int columns_count, TableFlags flags, const Float2& outer_size, float inner_width)
{
    return ::ImGui::BeginTable(str_id, columns_count,  flags, toImVec2(outer_size), inner_width);
}

static void tableNextRow()
{
    return ::ImGui::TableNextRow();
}

static bool tableSetColumnIndex(int col)
{
    return ::ImGui::TableSetColumnIndex(col);
}

static void endTable()
{
    return ::ImGui::EndTable();
}

static void ImPlot_PushStyleVar(int idx, const Float2& val)
{
    return ImPlot::PushStyleVar(idx, toImVec2(val));
}

static bool ImPlot_BeginPlot(const char* title_id, const Float2& size, int flags)
{
    return ImPlot::BeginPlot(title_id, toImVec2(size), flags);
}
static void ImPlot_SetupLegend(int location, int flags)
{
    return ::ImPlot::SetupLegend(location, flags);
}
static void ImPlot_PopStyleVar(int count)
{
    return ::ImPlot::PopStyleVar(count);
}
static void ImPlot_SetupAxes(const char* x_label, const char* y_label, int x_flags, int y_flags)
{
    return ::ImPlot::SetupAxes(x_label, y_label, x_flags, y_flags);
}
static void ImPlot_SetupAxisLimits(int idx, double min_lim, double max_lim, int cond)
{
    return ::ImPlot::SetupAxisLimits(idx, min_lim, max_lim, cond);
}
static void ImPlot_SetupAxisTicks(int idx, const double* values, int n_ticks, const char* const labels[], bool show_default)
{
    return ::ImPlot::SetupAxisTicks(idx, values, n_ticks, labels, show_default);
}
static void ImPlot_SetNextLineStyle(const Float4& col, float weight)
{
    return ::ImPlot::SetNextLineStyle(toImVec4(col), weight);
}

// NvPerf: these functions are needed to prevent using NvPerf code from within this plugin
// NvPerf will call into ImPlot_PlotLineG/ImPlot_PlotShadedG, and in those functions
// these getters are used to call back into nvperf to access the data stored there

DoubleGetter staticTsGetter;
DoubleGetter staticValGetter;
DoubleGetter staticTsGetterData2;
DoubleGetter staticValGetterData2;

int staticDataOffset;

ImPlotPoint ImPlotPointGetter(int idx, void* user_data)
{
    // call back into nvperf to get the values stored there
    ImPlotPoint point(staticTsGetter(idx + staticDataOffset), staticValGetter(idx + staticDataOffset));
    return point;
}

ImPlotPoint ImPlotPointGetterSlot2(int idx, void* user_data)
{
    // call back into nvperf to get the values stored there
    ImPlotPoint point(staticTsGetterData2(idx + staticDataOffset), staticValGetterData2(idx + staticDataOffset));
    return point;
}

ImPlotPoint ImPlotPointGetterSlot2ForceZero(int idx, void* user_data)
{
    // call back into nvperf to get the values stored there
    ImPlotPoint point(staticTsGetterData2(idx + staticDataOffset), 0.0);
    return point;
}

// End of NvPerf

static void ImPlot_PlotLineG(const char* label_id, void* data, int count, int flags)
{
    SL_LOG_ERROR("sl.imgui unsupported function: ImPlot_PlotLineG");
    assert(0);
    return;
}

static bool ImPlot_IsLegendEntryHovered(const char* label_id)
{
    return ::ImPlot::IsLegendEntryHovered(label_id);
}

static void ImPlot_SetNextFillStyle(const Float4& col, float alpha)
{
    return ::ImPlot::SetNextFillStyle(toImVec4(col), alpha);
}

static void ImPlot_PlotShadedG(const char* label_id, bool useGetterWithoutVal, void* data1, void* data2, int count, int flags)
{       
    SL_LOG_ERROR("sl.imgui unsupported function: ImPlot_PlotShadedG");
    assert(0);
    return;
}

static void ImPlot_EndPlot()
{
    return ::ImPlot::EndPlot();
}

static PlotStyle* ImPlot_GetStyle()
{
    return (PlotStyle*)(&::ImPlot::GetStyle());
}

static void ImPlot_PlotLineG_NSightPerf(const char* label_id, void* data, int count, int flags, DoubleGetter tsGetter, DoubleGetter valGetter, int dataOffset)
{
    staticTsGetter = tsGetter;
    staticValGetter = valGetter;
    staticDataOffset = dataOffset;
    return ::ImPlot::PlotLineG(label_id, &ImPlotPointGetter, nullptr, count, flags);
}

static void ImPlot_PlotShadedG_NSightPerf(const char* label_id, bool useGetterWithoutVal, void* data1, void* data2, int count, int flags, DoubleGetter tsGetter, DoubleGetter valGetter, DoubleGetter tsGetterData2, DoubleGetter valGetterData2, int dataOffset)
{
    staticTsGetter = tsGetter;
    staticValGetter = valGetter;
    staticTsGetterData2 = tsGetterData2;
    staticValGetterData2 = valGetterData2;
    staticDataOffset = dataOffset;

    if (useGetterWithoutVal)
    {
        return ::ImPlot::PlotShadedG(label_id, &ImPlotPointGetter, data1, &ImPlotPointGetterSlot2ForceZero, data2, count, flags);
    }
    else
    {
        return ::ImPlot::PlotShadedG(label_id, &ImPlotPointGetter, data1, &ImPlotPointGetterSlot2, data2, count, flags);
    }
}

static void destroyContextOnResize(unsigned int width, unsigned int height)
{
    if(g_ctx != nullptr &&
      (g_ctx->backBufferWidth != width ||
       g_ctx->backBufferHeight != height))
    {        
        destroyContext(nullptr);
    }   
}

static void closeCurrentPopup()
{
    ::ImGui::CloseCurrentPopup();
}

static void columns(int count /* = 1*/, const char* id /* = NULL*/, bool border /* = true*/)
{
    ::ImGui::Columns(count, id, border);
}

static void nextColumn()
{
    ::ImGui::NextColumn();
}

static int getColumnIndex()
{
    return ::ImGui::GetColumnIndex();
}

static float getColumnWidth(int columnIndex /* = -1*/)
{
    return ::ImGui::GetColumnWidth(columnIndex);
}

static void setColumnWidth(int columnIndex, float width)
{
    ::ImGui::SetColumnWidth(columnIndex, width);
}

static float getColumnOffset(int columnIndex /* = -1*/)
{
    return ::ImGui::GetColumnOffset(columnIndex);
}

static void setColumnOffset(int columnIndex, float offsetX)
{
    ::ImGui::SetColumnOffset(columnIndex, offsetX);
}

static int getColumnsCount()
{
    return ::ImGui::GetColumnsCount();
}

static bool beginTabBar(const char* strId, TabBarFlags flags)
{
    return ::ImGui::BeginTabBar(strId, flags);
}

static void endTabBar()
{
    ::ImGui::EndTabBar();
}

static bool beginTabItem(const char* label, bool* open, TabItemFlags flags)
{
    return ::ImGui::BeginTabItem(label, open, flags);
}

static void endTabItem()
{
    ::ImGui::EndTabItem();
}

static void setTabItemClosed(const char* tabOrDockedWindowLabel)
{
    ::ImGui::SetTabItemClosed(tabOrDockedWindowLabel);
}

static void dockSpace(uint32_t id, const Float2& size, DockNodeFlags flags, const WindowClass* windowClass)
{
    ::ImGui::DockSpace(id, { size.x, size.y }, flags, reinterpret_cast<const ImGuiWindowClass*>(windowClass));
}

static uint32_t dockSpaceOverViewport(Viewport* viewport, DockNodeFlags dockspaceFlags, const WindowClass* windowClass)
{
    return ::ImGui::DockSpaceOverViewport(reinterpret_cast<ImGuiViewport*>(viewport), dockspaceFlags,
        reinterpret_cast<const ImGuiWindowClass*>(windowClass));
}

static void setNextWindowDockId(ImGuiID dockId, Condition cond)
{
    ::ImGui::SetNextWindowDockID(dockId, static_cast<ImGuiCond>(cond));
}

static void setNextWindowClass(const WindowClass* windowClass)
{
    ::ImGui::SetNextWindowClass(reinterpret_cast<const ImGuiWindowClass*>(windowClass));
}

static uint32_t getWindowDockId()
{
    return ::ImGui::GetWindowDockID();
}

static DockNode* getWindowDockNode()
{
    ::ImGuiContext* ctx = g_ctx->imgui;
    return (DockNode*)ctx->CurrentWindow->DockNode;
}

static bool isWindowDocked()
{
    return ::ImGui::IsWindowDocked();
}

static bool beginDragDropSource(DragDropFlags flags)
{
    return ::ImGui::BeginDragDropSource(static_cast<ImGuiDragDropFlags>(flags));
}

static bool setDragDropPayload(const char* type, const void* data, size_t size, Condition cond)
{
    return ::ImGui::SetDragDropPayload(type, data, size, static_cast<ImGuiCond>(cond));
}

static void endDragDropSource()
{
    ::ImGui::EndDragDropSource();
}

static bool beginDragDropTarget()
{
    return ::ImGui::BeginDragDropTarget();
}

static const Payload* acceptDragDropPayload(const char* type, DragDropFlags flags)
{
    return reinterpret_cast<const Payload*>(::ImGui::AcceptDragDropPayload(type, static_cast<ImGuiDragDropFlags>(flags)));
}

static void endDragDropTarget()
{
    ::ImGui::EndDragDropTarget();
}

static const Payload* getDragDropPayload()
{
    return reinterpret_cast<const Payload*>(::ImGui::GetDragDropPayload());
}

static void pushClipRect(const Float2& clipRectMin, const Float2& clipRectMax, bool intersectWithCurrentClipRect)
{
    ::ImGui::PushClipRect(toImVec2(clipRectMin), toImVec2(clipRectMax), intersectWithCurrentClipRect);
}

static void popClipRect()
{
    ::ImGui::PopClipRect();
}

static void setItemDefaultFocus()
{
    ::ImGui::SetItemDefaultFocus();
}

static void setKeyboardFocusHere(int offset /* = 0*/)
{
    ::ImGui::SetKeyboardFocusHere(offset);
}

static void clearActiveId()
{
    ::ImGui::ClearActiveID();
}

static bool isItemHovered(HoveredFlags flags /* = 0*/)
{
    // Hack
    return ::ImGui::IsItemHovered(flags);
}

static bool isItemActive()
{
    return ::ImGui::IsItemActive();
}

static bool isItemFocused()
{
    return ::ImGui::IsItemFocused();
}

static bool isItemClicked(int mouseButton /* = 0*/)
{
    return ::ImGui::IsItemClicked(mouseButton);
}

static bool isItemVisible()
{
    return ::ImGui::IsItemVisible();
}

static bool isItemEdited()
{
    return ::ImGui::IsItemEdited();
}

static bool isItemDeactivated()
{
    return ::ImGui::IsItemDeactivated();
}

static bool isItemDeactivatedAfterEdit()
{
    return ::ImGui::IsItemDeactivatedAfterEdit();
}

static bool isAnyItemHovered()
{
    return ::ImGui::IsAnyItemHovered();
}

static bool isAnyItemActive()
{
    return ::ImGui::IsAnyItemActive();
}

static bool isAnyItemFocused()
{
    return ::ImGui::IsAnyItemFocused();
}

static Float2 getItemRectMin()
{
    return toFloat2(::ImGui::GetItemRectMin());
}

static Float2 getItemRectMax()
{
    return toFloat2(::ImGui::GetItemRectMax());
}

static Float2 getItemRectSize()
{
    return toFloat2(::ImGui::GetItemRectSize());
}

static void setItemAllowOverlap()
{
    return ::ImGui::SetItemAllowOverlap();
}

static bool isRectVisible(const Float2& size)
{
    return ::ImGui::IsRectVisible(toImVec2(size));
}

static bool isRectVisibleEx(const Float2& rectMin, const Float2& rectMax)
{
    return ::ImGui::IsRectVisible(toImVec2(rectMin), toImVec2(rectMax));
}

static float getTime()
{
    return static_cast<float>(::ImGui::GetTime());
}

static int getFrameCount()
{
    return ::ImGui::GetFrameCount();
}

static DrawList* getOverlayDrawList()
{
    // TODO: DrawList API support
    // return ::ImGui::GetOverlayDrawList();
    return nullptr;
}

static const char* getStyleColorName(StyleColor color)
{
    return ::ImGui::GetStyleColorName((int)color);
}

static Float2 calcTextSize(const char* text,
    const char* textEnd /* = nullptr*/,
    bool hideTextAfterDoubleHash /* = false*/,
    float wrap_width /* = -1.0f*/)
{
    return toFloat2(::ImGui::CalcTextSize(text, textEnd, hideTextAfterDoubleHash, wrap_width));
}

static void calcListClipping(int itemCount, float itemsHeight, int* outItemsDisplayStart, int* outItemsDisplayEnd)
{
    return ::ImGui::CalcListClipping(itemCount, itemsHeight, outItemsDisplayStart, outItemsDisplayEnd);
}

static bool beginChildFrame(uint32_t id, const Float2& size, WindowFlags flags /* = 0*/)
{
    return ::ImGui::BeginChildFrame(id, toImVec2(size), flags);
}

static void endChildFrame()
{
    ::ImGui::EndChildFrame();
}

static Float4 colorConvertU32ToFloat4(uint32_t in)
{
    return toFloat4(::ImGui::ColorConvertU32ToFloat4(in));
}

static uint32_t colorConvertFloat4ToU32(const Float4& in)
{
    return ::ImGui::ColorConvertFloat4ToU32(toImVec4(in));
}

static void colorConvertRGBtoHSV(float r, float g, float b, float& outH, float& outS, float& outV)
{
    ::ImGui::ColorConvertRGBtoHSV(r, g, b, outH, outS, outV);
}

static void colorConvertHSVtoRGB(float h, float s, float v, float& outR, float& outG, float& outB)
{
    ::ImGui::ColorConvertHSVtoRGB(h, s, v, outR, outG, outB);
}

static int getKeyIndex(KeyIndices imguiKeyIndex)
{
    auto interfaceToImguiKeyIndex = [](KeyIndices imguiKeyIndex) -> ImGuiKey {
        switch (imguiKeyIndex)
        {
        case KeyIndices::eTab:
            return ::ImGuiKey_Tab;
        case KeyIndices::eLeftArrow:
            return ::ImGuiKey_LeftArrow;
        case KeyIndices::eRightArrow:
            return ::ImGuiKey_RightArrow;
        case KeyIndices::eUpArrow:
            return ::ImGuiKey_UpArrow;
        case KeyIndices::eDownArrow:
            return ::ImGuiKey_DownArrow;
        case KeyIndices::ePageUp:
            return ::ImGuiKey_PageUp;
        case KeyIndices::ePageDown:
            return ::ImGuiKey_PageDown;
        case KeyIndices::eHome:
            return ::ImGuiKey_Home;
        case KeyIndices::eEnd:
            return ::ImGuiKey_End;
        case KeyIndices::eInsert:
            return ::ImGuiKey_Insert;
        case KeyIndices::eDelete:
            return ::ImGuiKey_Delete;
        case KeyIndices::eBackspace:
            return ::ImGuiKey_Backspace;
        case KeyIndices::eSpace:
            return ::ImGuiKey_Space;
        case KeyIndices::eEnter:
            return ::ImGuiKey_Enter;
        case KeyIndices::eEscape:
            return ::ImGuiKey_Escape;
        case KeyIndices::eA:
            return ::ImGuiKey_A;
        case KeyIndices::eC:
            return ::ImGuiKey_C;
        case KeyIndices::eV:
            return ::ImGuiKey_V;
        case KeyIndices::eX:
            return ::ImGuiKey_X;
        case KeyIndices::eY:
            return ::ImGuiKey_Y;
        case KeyIndices::eZ:
            return ::ImGuiKey_Z;
        default:
            return ::ImGuiKey_COUNT;
        };
    };

    return ::ImGui::GetKeyIndex(interfaceToImguiKeyIndex(imguiKeyIndex));
}

static bool isKeyDown(int userKeyIndex)
{
    return ::ImGui::IsKeyDown(userKeyIndex);
}

static bool isKeyPressed(int userKeyIndex, bool repeat /* = true*/)
{
    return ::ImGui::IsKeyPressed(userKeyIndex, repeat);
}

static bool isKeyReleased(int userKeyIndex)
{
    return ::ImGui::IsKeyReleased(userKeyIndex);
}

static int getKeyPressedAmount(int keyIndex, float repeatDelay, float rate)
{
    return ::ImGui::GetKeyPressedAmount(keyIndex, repeatDelay, rate);
}

static KeyModifiers getKeyModifiers()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    KeyModifiers modifiers = kKeyModifierNone;
    if (io.KeyCtrl)
        modifiers |= kKeyModifierCtrl;
    if (io.KeyShift)
        modifiers |= kKeyModifierShift;
    if (io.KeyAlt)
        modifiers |= kKeyModifierAlt;
    if (io.KeySuper)
        modifiers |= kKeyModifierSuper;
    return modifiers;
}

static bool isMouseDown(int button)
{
    return ::ImGui::IsMouseDown(button);
}

static bool isAnyMouseDown()
{
    return ::ImGui::IsAnyMouseDown();
}

static bool isMouseClicked(int button, bool repeat /* = false*/)
{
    return ::ImGui::IsMouseClicked(button, repeat);
}

static bool isMouseDoubleClicked(int button)
{
    return ::ImGui::IsMouseDoubleClicked(button);
}

static bool isMouseReleased(int button)
{
    return ::ImGui::IsMouseReleased(button);
}

static bool isMouseDragging(int button /* = 0*/, float lockThreshold /* = -1.0f*/)
{
    return ::ImGui::IsMouseDragging(button, lockThreshold);
}

static bool isMouseHoveringRect(const Float2& rMin, const Float2& rMax, bool clip /* = true*/)
{
    return ::ImGui::IsMouseHoveringRect(toImVec2(rMin), toImVec2(rMax), clip);
}

static bool isMousePosValid(const Float2* mousePos /* = nullptr*/)
{
    return ::ImGui::IsMousePosValid((ImVec2*)mousePos);
}

static Float2 getMousePos()
{
    return toFloat2(::ImGui::GetMousePos());
}

static Float2 getMousePosOnOpeningCurrentPopup()
{
    return toFloat2(::ImGui::GetMousePosOnOpeningCurrentPopup());
}

static Float2 getMouseDragDelta(int button /* = 0*/, float lockThreshold /* = -1.0f*/)
{
    return toFloat2(::ImGui::GetMouseDragDelta(button, lockThreshold));
}

static void resetMouseDragDelta(int button /* = 0*/)
{
    return ::ImGui::ResetMouseDragDelta(button);
}

static type::Float2 getMouseWheel()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return type::Float2{ io.MouseWheelH, io.MouseWheel };
}

static MouseCursor getMouseCursor()
{
    return (MouseCursor)::ImGui::GetMouseCursor();
}

static void setMouseCursor(MouseCursor type)
{
    ::ImGui::SetMouseCursor((int)type);
}

static void captureKeyboardFromApp(bool capture /* = true*/)
{
    ::ImGui::CaptureKeyboardFromApp(capture);
}

static void captureMouseFromApp(bool capture /* = true*/)
{
    ::ImGui::CaptureMouseFromApp(capture);
}

static const char* getClipboardText()
{
    return ::ImGui::GetClipboardText();
}

static void setClipboardText(const char* text)
{
    ::ImGui::SetClipboardText(text);
}

static bool getWantSaveIniSettings()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.WantSaveIniSettings;
}

static void setWantSaveIniSettings(bool wantSaveIniSettings)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.WantSaveIniSettings = wantSaveIniSettings;
}

static void loadIniSettingsFromMemory(const char* iniData, size_t iniSize)
{
    ::ImGui::LoadIniSettingsFromMemory(iniData, iniSize);
}

static const char* saveIniSettingsToMemory(size_t* iniSize)
{
    return ::ImGui::SaveIniSettingsToMemory(iniSize);
}

static Viewport* getMainViewport()
{
    return reinterpret_cast<Viewport*>(::ImGui::GetMainViewport());
}

static void dockBuilderDockWindow(const char* windowName, uint32_t nodeId)
{
    ::ImGui::DockBuilderDockWindow(windowName, nodeId);
}

static DockNode* dockBuilderGetNode(uint32_t nodeId)
{
    return reinterpret_cast<DockNode*>(::ImGui::DockBuilderGetNode(nodeId));
}

static void dockBuilderAddNode(uint32_t nodeId, DockNodeFlags flags)
{
    ::ImGui::DockBuilderAddNode(nodeId, flags);
}

static void dockBuilderRemoveNode(uint32_t nodeId)
{
    ::ImGui::DockBuilderRemoveNode(nodeId);
}

static void dockBuilderRemoveNodeDockedWindows(uint32_t nodeId, bool clearPersistentDockingReferences)
{
    ::ImGui::DockBuilderRemoveNodeDockedWindows(nodeId, clearPersistentDockingReferences);
}

static void dockBuilderRemoveNodeChildNodes(uint32_t nodeId)
{
    ::ImGui::DockBuilderRemoveNodeChildNodes(nodeId);
}

static uint32_t dockBuilderSplitNode(
    uint32_t nodeId, Direction splitDir, float sizeRatioForNodeAtDir, uint32_t* outIdDir, uint32_t* outIdOther)
{
    return ::ImGui::DockBuilderSplitNode(
        nodeId, static_cast<ImGuiDir>(splitDir), sizeRatioForNodeAtDir, outIdDir, outIdOther);
}

static void dockBuilderFinish(uint32_t nodeId)
{
    ::ImGui::DockBuilderFinish(nodeId);
}

static Font* addFont(const FontConfig* fontCfg)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return (Font*)io.Fonts->AddFont((ImFontConfig*)fontCfg);
}

static Font* addFontDefault(const FontConfig* fontCfg)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return (Font*)io.Fonts->AddFontDefault((ImFontConfig*)fontCfg);
}

static Font* addFontFromFileTTF(const char* filename, float sizePixels, const FontConfig* fontCfg, const Wchar* glyphRanges)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return (Font*)io.Fonts->AddFontFromFileTTF(filename, sizePixels, (ImFontConfig*)fontCfg, glyphRanges);
}

static Font* addFontFromMemoryTTF(
    void* fontData, int fontSize, float sizePixels, const FontConfig* fontCfg, const Wchar* glyphRanges)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return (Font*)io.Fonts->AddFontFromMemoryTTF(fontData, fontSize, sizePixels, (ImFontConfig*)fontCfg, glyphRanges);
}

static Font* addFontFromMemoryCompressedTTF(const void* compressedFontData,
    int compressedFontSize,
    float sizePixels,
    const FontConfig* fontCfg,
    const Wchar* glyphRanges)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return (Font*)io.Fonts->AddFontFromMemoryCompressedTTF(
        compressedFontData, compressedFontSize, sizePixels, (ImFontConfig*)fontCfg, glyphRanges);
}

static Font* addFontFromMemoryCompressedBase85TTF(const char* compressedFontDataBase85,
    float sizePixels,
    const FontConfig* fontCfg,
    const Wchar* glyphRanges)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return (Font*)io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        compressedFontDataBase85, sizePixels, (ImFontConfig*)fontCfg, glyphRanges);
}


static int addFontCustomRectGlyph(Font* font, Wchar id, int width, int height, float advanceX, const type::Float2& offset)
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->AddCustomRectFontGlyph((ImFont*)font, id, width, height, advanceX, toImVec2(offset));
}

static const FontCustomRect* getFontCustomRectByIndex(int index)
{
    if (index < 0)
        return nullptr;
    ::ImGuiIO& io = ::ImGui::GetIO();

    return (imgui::FontCustomRect*)io.Fonts->GetCustomRectByIndex(index);
}

static bool buildFont()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->Build();
}

static bool isFontBuilt()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->IsBuilt();
}

static void getFontTexDataAsAlpha8(unsigned char** outPixels, int* outWidth, int* outHeight)
{
    ::ImGui::GetIO().Fonts->GetTexDataAsAlpha8(outPixels, outWidth, outHeight);
}

static void getFontTexDataAsRgba32(unsigned char** outPixels, int* outWidth, int* outHeight)
{
    ::ImGui::GetIO().Fonts->GetTexDataAsRGBA32(outPixels, outWidth, outHeight);
}

static void clearFontInputData()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.Fonts->ClearInputData();
}

static void clearFontTexData()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.Fonts->ClearTexData();
}

static void clearFonts()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.Fonts->ClearFonts();
}

void clearFontInputOutput()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    io.Fonts->Clear();
}

static const Wchar* getFontGlyphRangesDefault()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesDefault();
}

static const Wchar* getFontGlyphRangesKorean()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesKorean();
}

static const Wchar* getFontGlyphRangesJapanese()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesJapanese();
}

static const Wchar* getFontGlyphRangesChineseFull()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesChineseFull();
}

static const Wchar* getFontGlyphRangesChineseSimplifiedCommon()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
}

static const Wchar* getFontGlyphRangesCyrillic()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesCyrillic();
}

static const Wchar* getFontGlyphRangesThai()
{
    ::ImGuiIO& io = ::ImGui::GetIO();
    return io.Fonts->GetGlyphRangesThai();
}

static void setFontGlobalScale(float scale)
{
    ImGuiIO& io = ::ImGui::GetIO();
    io.FontGlobalScale = scale;
}

static void addWindowDrawCallback(DrawCallback userCallback, void* userData)
{
    ImDrawList* drawList = ::ImGui::GetWindowDrawList();
    if (drawList)
    {
        drawList->AddCallback((ImDrawCallback)userCallback, userData);
    }
}

static void addLine(DrawList* drawList, const type::Float2& a, const type::Float2& b, uint32_t col, float thickness)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddLine(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, col, thickness);
}

static void addRect(DrawList* drawList,
    const type::Float2& a,
    const type::Float2& b,
    uint32_t col,
    float rounding,
    DrawCornerFlags roundingCornersFlags,
    float thickness)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddRect(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, col, rounding, roundingCornersFlags, thickness);
}

static void addRectFilled(DrawList* drawList,
    const type::Float2& a,
    const type::Float2& b,
    uint32_t col,
    float rounding,
    DrawCornerFlags roundingCornersFlags)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddRectFilled(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, col, rounding, roundingCornersFlags);
}

static void addRectFilledMultiColor(DrawList* drawList,
    const type::Float2& a,
    const type::Float2& b,
    uint32_t colUprLeft,
    uint32_t colUprRight,
    uint32_t colBotRight,
    uint32_t colBotLeft)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddRectFilledMultiColor(
        ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, colBotLeft, colBotRight, colBotLeft, colBotRight);
}

static void addQuad(DrawList* drawList,
    const type::Float2& a,
    const type::Float2& b,
    const type::Float2& c,
    const type::Float2& d,
    uint32_t col,
    float thickness)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddQuad(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ c.x, c.y }, ImVec2{ d.x, d.y }, col, thickness);
}

static void addQuadFilled(DrawList* drawList,
    const type::Float2& a,
    const type::Float2& b,
    const type::Float2& c,
    const type::Float2& d,
    uint32_t col)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddQuadFilled(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ c.x, c.y }, ImVec2{ d.x, d.y }, col);
}

static void addTriangle(
    DrawList* drawList, const type::Float2& a, const type::Float2& b, const type::Float2& c, uint32_t col, float thickness)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddTriangle(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ c.x, c.y }, col, thickness);
}

static void addTriangleFilled(
    DrawList* drawList, const type::Float2& a, const type::Float2& b, const type::Float2& c, uint32_t col)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddTriangleFilled(ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ c.x, c.y }, col);
}

static void addCircle(
    DrawList* drawList, const type::Float2& centre, float radius, uint32_t col, int32_t numSegments, float thickness)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddCircle(ImVec2{ centre.x, centre.y }, radius, col, numSegments, thickness);
}

static void addCircleFilled(DrawList* drawList, const type::Float2& centre, float radius, uint32_t col, int32_t numSegments)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddCircleFilled(ImVec2{ centre.x, centre.y }, radius, col, numSegments);
}

static void addText(DrawList* drawList, const type::Float2& pos, uint32_t col, const char* textBegin, const char* textEnd)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddText(ImVec2{ pos.x, pos.y }, col, textBegin, textEnd);
}

static void addTextEx(DrawList* drawList,
    const Font* font,
    float fontSize,
    const type::Float2& pos,
    uint32_t col,
    const char* textBegin,
    const char* textEnd,
    float wrapWidth,
    const type::Float4* cpuFineClipRect)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddText(reinterpret_cast<const ::ImFont*>(font), fontSize, ImVec2{ pos.x, pos.y }, col, textBegin,
        textEnd, wrapWidth, reinterpret_cast<const ::ImVec4*>(cpuFineClipRect));
}

static void addImage(DrawList* drawList,
    TextureId textureId,
    const type::Float2& a,
    const type::Float2& b,
    const type::Float2& uvA,
    const type::Float2& uvB,
    uint32_t col)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddImage(
        textureId.ptr, ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ uvA.x, uvA.y }, ImVec2{ uvB.x, uvB.y }, col);
}

static void addImageQuad(DrawList* drawList,
    TextureId textureId,
    const type::Float2& a,
    const type::Float2& b,
    const type::Float2& c,
    const type::Float2& d,
    const type::Float2& uvA,
    const type::Float2& uvB,
    const type::Float2& uvC,
    const type::Float2& uvD,
    uint32_t col)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddImageQuad(textureId.ptr, ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ c.x, c.y },
        ImVec2{ d.x, d.y }, ImVec2{ uvA.x, uvA.y }, ImVec2{ uvB.x, uvB.y }, ImVec2{ uvC.x, uvC.y },
        ImVec2{ uvD.x, uvD.y }, col);
}

static void addImageRounded(DrawList* drawList,
    TextureId textureId,
    const type::Float2& a,
    const type::Float2& b,
    const type::Float2& uvA,
    const type::Float2& uvB,
    uint32_t col,
    float rounding,
    DrawCornerFlags roundingCorners)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddImageRounded(textureId.ptr, ImVec2{ a.x, a.y }, ImVec2{ b.x, b.y }, ImVec2{ uvA.x, uvA.y },
        ImVec2{ uvB.x, uvB.y }, col, rounding, roundingCorners);
}

static void addPolyline(
    DrawList* drawList, const type::Float2* points, const int32_t numPoints, uint32_t col, bool closed, float thickness)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddPolyline(reinterpret_cast<const ::ImVec2*>(points), numPoints, col, closed, thickness);
}

static void addConvexPolyFilled(DrawList* drawList, const type::Float2* points, const int32_t numPoints, uint32_t col)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddConvexPolyFilled(reinterpret_cast<const ImVec2*>(points), numPoints, col);
}

static void addBezierCurve(DrawList* drawList,
    const type::Float2& pos0,
    const type::Float2& cp0,
    const type::Float2& cp1,
    const type::Float2& pos1,
    uint32_t col,
    float thickness,
    int32_t numSegments)
{
    auto imDrawList = reinterpret_cast<ImDrawList*>(drawList);

    imDrawList->AddBezierCurve(ImVec2{ pos0.x, pos0.y }, ImVec2{ cp0.x, cp0.y }, ImVec2{ cp1.x, cp1.y },
        ImVec2{ pos1.x, pos1.y }, col, thickness, numSegments);
}

static ListClipper* createListClipper(int32_t itemsCount, float itemsHeight)
{
    ::ImGuiListClipper* clipper = new ::ImGuiListClipper(itemsCount, itemsHeight);
    return reinterpret_cast<ListClipper*>(clipper);
}

static bool stepListClipper(ListClipper* listClipper)
{
    return reinterpret_cast<::ImGuiListClipper*>(listClipper)->Step();
}

static void destroyListClipper(ListClipper* listClipper)
{
    delete reinterpret_cast<::ImGuiListClipper*>(listClipper);
}

static bool feedKeyboardEvent(Context* ctx, const input::KeyboardEvent& evt)
{
    return false;
}

static bool feedMouseEvent(Context* ctx, const input::MouseEvent& e)
{
    ::ImGuiIO& io = ::ImGui::GetIO();

    switch (e.type)
    {
    case sl::input::MouseEventType::eLeftButtonDown:
        g_ctx->mouseEvents[0].pressed = true;
        g_ctx->mouseEvents[0].down = true;
        break;
    case sl::input::MouseEventType::eLeftButtonUp:
        g_ctx->mouseEvents[0].released = true;
        g_ctx->mouseEvents[0].down = false;
        break;
    case sl::input::MouseEventType::eRightButtonDown:
        g_ctx->mouseEvents[1].pressed = true;
        g_ctx->mouseEvents[1].down = true;
        break;
    case sl::input::MouseEventType::eRightButtonUp:
        g_ctx->mouseEvents[1].released = true;
        g_ctx->mouseEvents[1].down = false;
        break;
    case sl::input::MouseEventType::eMiddleButtonDown:
        g_ctx->mouseEvents[2].pressed = true;
        g_ctx->mouseEvents[2].down = true;
        break;
    case sl::input::MouseEventType::eMiddleButtonUp:
        g_ctx->mouseEvents[2].released = true;
        g_ctx->mouseEvents[2].down = false;
        break;
    case sl::input::MouseEventType::eMove:
        // Mouse is assumed at the position when button is first time pressed in this frame.
        if (!std::any_of(g_ctx->mouseEvents.begin(), g_ctx->mouseEvents.end(), [](auto button) { return button.pressed; }))
        {
            io.MousePos.x = e.coords.x;// static_cast<float>(e.coords.x * io.DisplaySize.x);
            io.MousePos.y = e.coords.y;// static_cast<float>(e.coords.y * io.DisplaySize.y);
        }
        break;
    case sl::input::MouseEventType::eScroll:
        io.MouseWheelH = static_cast<float>(io.MouseWheelH + e.scrollDelta.x);
        io.MouseWheel = static_cast<float>(io.MouseWheel + e.scrollDelta.y);
        break;
    }

    return !io.WantCaptureMouse;
}

} // namespace imgui

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

    auto& ctx = (*imgui::getContext());

    auto parameters = api::getContext()->parameters;

    //! Plugin manager gives us the device type and the application id
    //! 
    json& config = *(json*)api::getContext()->loaderConfig;
    uint32_t deviceType{};
    int appId{};
    config.at("appId").get_to(appId);
    config.at("deviceType").get_to(deviceType);

    //! Now let's obtain compute interface if we need to dispatch some compute work
    //! 
    ctx.platform = (RenderAPI)deviceType;
    if (ctx.platform == RenderAPI::eD3D11)
    {
        if (!param::getPointerParam(parameters, sl::param::common::kComputeDX11On12API, &ctx.compute))
        {
            // Log error
            return false;
        }
    }
    else
    {
        if (!param::getPointerParam(parameters, sl::param::common::kComputeAPI, &ctx.compute))
        {
            // Log error
            return false;
        }
    }

    auto listener = [](int nCode, WPARAM wParam, LPARAM lParam)->LRESULT
    {
        extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

        if (nCode >= 0)
        {
            MSG* cwp = (MSG*)lParam;
            if (ImGui_ImplWin32_WndProcHandler(cwp->hwnd, cwp->message, cwp->wParam, cwp->lParam)) return 0;
        }

        return CallNextHookEx(NULL, nCode, wParam, lParam);
    };

    ctx.inputHook = SetWindowsHookEx(WH_GETMESSAGE, listener, GetModuleHandle(NULL), GetCurrentThreadId());
    SL_LOG_INFO("SetWindowsHookEx result - %s", std::system_category().message(GetLastError()).c_str());

    using namespace sl::imgui;

    
    ctx.ui = {
        createContext,
        destroyContext,
        setCurrentContext,
        getFontAtlasPixels,
        newFrame,
        render,
        getDrawData,
        triggerRenderWindowCallbacks,
        triggerRenderAnywhereCallbacks,
        registerRenderCallbacks,
        plotGraph,
        setDisplaySize,
        getDisplaySize,
        getStyle,
        showDemoWindow,
        showMetricsWindow,
        showStyleEditor,
        showStyleSelector,
        showFontSelector,
        showUserGuide,
        getImGuiVersion,
        setStyleColors,
        begin,
        end,
        beginChild,
        beginChildId,
        endChild,
        isWindowAppearing,
        isWindowCollapsed,
        isWindowFocused,
        isWindowHovered,
        getWindowDrawList,
        getWindowDpiScale,
        getWindowPos,
        getWindowSize,
        getWindowWidth,
        getWindowHeight,
        getContentRegionMax,
        getContentRegionAvail,
        getContentRegionAvailWidth,
        getWindowContentRegionMin,
        getWindowContentRegionMax,
        getWindowContentRegionWidth,
        setNextWindowPos,
        setNextWindowSize,
        setNextWindowSizeConstraints,
        setNextWindowContentSize,
        setNextWindowCollapsed,
        setNextWindowFocus,
        setNextWindowBgAlpha,
        setWindowFontScale,
        setWindowPos,
        setWindowSize,
        setWindowCollapsed,
        setWindowFocus,
        getScrollX,
        getScrollY,
        getScrollMaxX,
        getScrollMaxY,
        setScrollX,
        setScrollY,
        setScrollHereY,
        setScrollFromPosY,
        pushFont,
        popFont,
        pushStyleColor,
        popStyleColor,
        pushStyleVarFloat,
        pushStyleVarFloat2,
        popStyleVar,
        getStyleColorVec4,
        getFont,
        getFontSize,
        getFontTexUvWhitePixel,
        getColorU32StyleColor,
        getColorU32Vec4,
        getColorU32,
        pushItemWidth,
        popItemWidth,
        calcItemSize,
        calcItemWidth,
        pushItemFlag,
        popItemFlag,
        pushTextWrapPos,
        popTextWrapPos,
        pushAllowKeyboardFocus,
        popAllowKeyboardFocus,
        pushButtonRepeat,
        popButtonRepeat,
        separator,
        sameLineEx,
        newLine,
        spacing,
        dummy,
        indent,
        unindent,
        beginGroup,
        endGroup,
        getCursorPos,
        getCursorPosX,
        getCursorPosY,
        setCursorPos,
        setCursorPosX,
        setCursorPosY,
        getCursorStartPos,
        getCursorScreenPos,
        setCursorScreenPos,
        alignTextToFramePadding,
        getTextLineHeight,
        getTextLineHeightWithSpacing,
        getFrameHeight,
        getFrameHeightWithSpacing,
        pushIdString,
        pushIdStringBeginEnd,
        pushIdInt,
        pushIdPtr,
        popId,
        getIdString,
        getIdStringBeginEnd,
        getIdPtr,
        textUnformatted,
        text,
        textColored,
        labelColored,
        textDisabled,
        textWrapped,
        labelText,
        bulletText,
        buttonEx,
        smallButton,
        invisibleButton,
        arrowButton,
        image,
        imageButton,
        checkbox,
        checkboxFlags,
        radioButton,
        radioButtonEx,
        progressBar,
        bullet,
        beginCombo,
        endCombo,
        combo,
        dragFloat,
        dragFloat2,
        dragFloat3,
        dragFloat4,
        dragFloatRange2,
        dragInt,
        dragInt2,
        dragInt3,
        dragInt4,
        dragIntRange2,
        dragScalar,
        dragScalarN,
        sliderFloat,
        sliderFloat2,
        sliderFloat3,
        sliderFloat4,
        sliderAngle,
        sliderInt,
        sliderInt2,
        sliderInt3,
        sliderInt4,
        sliderScalar,
        sliderScalarN,
        vSliderFloat,
        vSliderInt,
        vSliderScalar,
        inputText,
        inputTextWithHint,
        inputTextMultiline,
        inputFloat,
        inputFloat2,
        inputFloat3,
        inputFloat4,
        inputInt,
        inputInt2,
        inputInt3,
        inputInt4,
        inputDouble,
        inputScalar,
        inputScalarN,
        colorEdit3,
        colorEdit4,
        colorPicker3,
        colorPicker4,
        colorButton,
        setColorEditOptions,
        treeNode,
        treeNodeString,
        treeNodePtr,
        treeNodeEx,
        treeNodeStringEx,
        treeNodePtrEx,
        treePushString,
        treePushPtr,
        treePop,
        treeAdvanceToLabelPos,
        getTreeNodeToLabelSpacing,
        setNextTreeNodeOpen,
        collapsingHeader,
        collapsingHeaderEx,
        selectable,
        selectableEx,
        listBox,
        listBoxEx,
        listBoxHeader,
        listBoxHeaderEx,
        listBoxFooter,
        plotLines,
        plotLinesEx,
        plotHistogram,
        plotHistogramEx,
        valueBool,
        valueInt,
        valueUInt32,
        valueFloat,
        beginMainMenuBar,
        endMainMenuBar,
        beginMenuBar,
        endMenuBar,
        beginMenu,
        endMenu,
        menuItem,
        menuItemEx,
        setTooltip,
        beginTooltip,
        endTooltip,
        openPopup,
        beginPopup,
        beginPopupContextItem,
        beginPopupContextWindow,
        beginPopupContextVoid,
        beginPopupModal,
        endPopup,
        openPopupOnItemClick,
        isPopupOpen,
        closeCurrentPopup,
        columns,
        nextColumn,
        getColumnIndex,
        getColumnWidth,
        setColumnWidth,
        getColumnOffset,
        setColumnOffset,
        getColumnsCount,
        beginTabBar,
        endTabBar,
        beginTabItem,
        endTabItem,
        setTabItemClosed,
        dockSpace,
        dockSpaceOverViewport,
        setNextWindowDockId,
        setNextWindowClass,
        getWindowDockId,
        getWindowDockNode,
        isWindowDocked,
        beginDragDropSource,
        setDragDropPayload,
        endDragDropSource,
        beginDragDropTarget,
        acceptDragDropPayload,
        endDragDropTarget,
        getDragDropPayload,
        pushClipRect,
        popClipRect,
        setItemDefaultFocus,
        setKeyboardFocusHere,
        clearActiveId,
        isItemHovered,
        isItemActive,
        isItemFocused,
        isItemClicked,
        isItemVisible,
        isItemEdited,
        isItemDeactivated,
        isItemDeactivatedAfterEdit,
        isAnyItemHovered,
        isAnyItemActive,
        isAnyItemFocused,
        getItemRectMin,
        getItemRectMax,
        getItemRectSize,
        setItemAllowOverlap,
        isRectVisible,
        isRectVisibleEx,
        getTime,
        getFrameCount,
        getOverlayDrawList,
        getStyleColorName,
        calcTextSize,
        calcListClipping,
        beginChildFrame,
        endChildFrame,
        colorConvertU32ToFloat4,
        colorConvertFloat4ToU32,
        colorConvertRGBtoHSV,
        colorConvertHSVtoRGB,
        getKeyIndex,
        isKeyDown,
        isKeyPressed,
        isKeyReleased,
        getKeyPressedAmount,
        getKeyModifiers,
        isMouseDown,
        isAnyMouseDown,
        isMouseClicked,
        isMouseDoubleClicked,
        isMouseReleased,
        isMouseDragging,
        isMouseHoveringRect,
        isMousePosValid,
        getMousePos,
        getMousePosOnOpeningCurrentPopup,
        getMouseDragDelta,
        resetMouseDragDelta,
        getMouseWheel,
        getMouseCursor,
        setMouseCursor,
        captureKeyboardFromApp,
        captureMouseFromApp,
        getClipboardText,
        setClipboardText,
        getWantSaveIniSettings,
        setWantSaveIniSettings,
        loadIniSettingsFromMemory,
        saveIniSettingsToMemory,
        getMainViewport,
        dockBuilderDockWindow,
        dockBuilderGetNode,
        dockBuilderAddNode,
        dockBuilderRemoveNode,
        dockBuilderRemoveNodeDockedWindows,
        dockBuilderRemoveNodeChildNodes,
        dockBuilderSplitNode,
        dockBuilderFinish,
        addFont,
        addFontDefault,
        addFontFromFileTTF,
        addFontFromMemoryTTF,
        addFontFromMemoryCompressedTTF,
        addFontFromMemoryCompressedBase85TTF,
        addFontCustomRectGlyph,
        getFontCustomRectByIndex,
        buildFont,
        isFontBuilt,
        getFontTexDataAsAlpha8,
        getFontTexDataAsRgba32,
        clearFontInputData,
        clearFontTexData,
        clearFonts,
        clearFontInputOutput,
        getFontGlyphRangesDefault,
        getFontGlyphRangesKorean,
        getFontGlyphRangesJapanese,
        getFontGlyphRangesChineseFull,
        getFontGlyphRangesChineseSimplifiedCommon,
        getFontGlyphRangesCyrillic,
        getFontGlyphRangesThai,
        setFontGlobalScale,
        addWindowDrawCallback,
        addLine,
        addRect,
        addRectFilled,
        addRectFilledMultiColor,
        addQuad,
        addQuadFilled,
        addTriangle,
        addTriangleFilled,
        addCircle,
        addCircleFilled,
        addText,
        addTextEx,
        addImage,
        addImageQuad,
        addImageRounded,
        addPolyline,
        addConvexPolyFilled,
        addBezierCurve,
        createListClipper,
        stepListClipper,
        destroyListClipper,
        feedKeyboardEvent,
        feedMouseEvent,
        isModalPopupOpen,
        popStyleVarMultiple,
        beginTable,
        tableNextRow,
        tableSetColumnIndex,
        endTable,
        ImPlot_PushStyleVar,
        ImPlot_BeginPlot,
        ImPlot_SetupLegend,
        ImPlot_PopStyleVar,
        ImPlot_SetupAxes,
        ImPlot_SetupAxisLimits,
        ImPlot_SetupAxisTicks,
        ImPlot_SetNextLineStyle,
        ImPlot_PlotLineG,
        ImPlot_IsLegendEntryHovered,
        ImPlot_SetNextFillStyle,
        ImPlot_PlotShadedG,
        ImPlot_EndPlot,
        ImPlot_GetStyle,
        toggleRenderInternal,
        getRenderInternal,
        getCurrentContext, 
        ImPlot_PlotLineG_NSightPerf,
        ImPlot_PlotShadedG_NSightPerf,
        destroyContextOnResize,
        getFonts,
    };

    parameters->set(param::imgui::kInterface, &ctx.ui);

    return true;
}

void slOnPluginShutdown()
{
    auto& ctx = (*imgui::getContext());

    if (imgui::g_ctx != nullptr)
    {
        imgui::destroyContext(nullptr);
        if (ctx.platform == RenderAPI::eVulkan)
        {
            delete ctx.cmdQueue;
        }
        ctx.cmdQueue = nullptr;
        ctx.vkSwapchainInfoMap.clear();
    }

    UnhookWindowsHookEx(ctx.inputHook);

    // Common shutdown
    plugin::onShutdown(api::getContext());
}

//! Figure out if we are supported on the current hardware or not
//! 
void updateEmbeddedJSON(json& config)
{
    // Check if plugin is supported or not on this platform and set the flag accordingly
    common::SystemCaps* caps = {};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kSystemCaps, &caps);
    common::PFunUpdateCommonEmbeddedJSONConfig* updateCommonEmbeddedJSONConfig{};
    param::getPointerParam(api::getContext()->parameters, sl::param::common::kPFunUpdateCommonEmbeddedJSONConfig, &updateCommonEmbeddedJSONConfig);
    if (caps && updateCommonEmbeddedJSONConfig)
    {
        // Our plugin runs on any system so use all defaults
        common::PluginInfo info{};
        info.SHA = GIT_LAST_COMMIT_SHORT;
        updateCommonEmbeddedJSONConfig(&config, info);
    }
    auto& ctx = (*sl::imgui::getContext());

    json& extraConfig = *(json*)api::getContext()->extConfig;

    extraConfig.contains("smallFontSize") ? extraConfig.at("smallFontSize").get_to(ctx.uiSmallFontSize) : imgui::DEFAULT_SMALL_FONT_SIZE;
    extraConfig.contains("mediumFontSize") ? extraConfig.at("mediumFontSize").get_to(ctx.uiMediumFontSize) : imgui::DEFAULT_MEDIUM_FONT_SIZE;
}

void renderInternal()
{
    auto& ctx = (*sl::imgui::getContext());   
    sl::imgui::setDisplaySize(Float2{ (float)ctx.backBufferWidth, (float)ctx.backBufferHeight });
    sl::imgui::newFrame((float)ctx.frameMeter.getMean());
    
    sl::imgui::pushFont(ctx.fonts.uiSmallFont);

    // Auto adjust the side bar based on latest size
    static Float2 s_lastSize{};

    sl::imgui::setNextWindowPos(imgui::Float2{ ctx.backBufferWidth - s_lastSize.x - 10, ctx.backBufferHeight * 0.5f - s_lastSize.y * 0.5f }, imgui::Condition::eAlways, imgui::Float2{ 0.0f, 0.0f });
    sl::imgui::setNextWindowBgAlpha(0.5f);

    sl::imgui::begin("Streamline", 0, imgui::kWindowFlagAlwaysAutoResize | imgui::kWindowFlagNoCollapse);
    
    // render all the other plugins UI via callbacks
    sl::imgui::triggerRenderWindowCallbacks(false);
    sl::imgui::triggerRenderAnywhereCallbacks(false);

    s_lastSize = sl::imgui::getWindowSize();
    sl::imgui::end(); // Side bar    
    sl::imgui::popFont();

    //! Let rendering stabilize before trying to trigger UI rendering
    //!
    //! This helps with exclusive full screen transitions in some games,
    //! it can leave UI in invalid state and cause issues. 
    //! Note, this comment was copied from present.cpp line 109, so this
    //! delay is a known issue with imgui.
    //! Most importantly, this delay is needed for Unreal apps to work
    if(ctx.currentFrame > 60)
    {
        ctx.cmdList->beginCommandList();
        if (ctx.platform == RenderAPI::eD3D12 || ctx.platform == RenderAPI::eD3D11)
        {           
            sl::imgui::render(ctx.cmdList->getCmdList(), (void*)ctx.currentBackBuffer, ctx.currentBackBufferIndex);
        }
        else if (ctx.platform == RenderAPI::eVulkan)
        {            
            sl::imgui::render(ctx.cmdList->getCmdList(), (void*)ctx.currentBackBufferVk, ctx.currentBackBufferIndex);
        }
        else
        {
            SL_LOG_ERROR("Unsupported render API used in sl.imgui plugin");
            assert(0);
        }
        
        ctx.cmdList->executeCommandList();
    }
    else
    {
        ImGui::EndFrame();
    }
}

// DXGI Hooks

HRESULT slHookCreateSwapChainForCoreWindow(IDXGIFactory2* pFactory, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    auto& ctx = (*sl::imgui::getContext());
    ctx.cmdQueue = (chi::CommandQueue)pDevice;
    ctx.bufferCount = pDesc->BufferCount;
    ctx.compute->createCommandListContext(ctx.cmdQueue, ctx.bufferCount, ctx.cmdList, "imgui-cmdlist");
    ctx.frameMeter.reset();
    return S_OK;
}

HRESULT slHookCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{    
    auto& ctx = (*sl::imgui::getContext());
    ctx.cmdQueue = (chi::CommandQueue)pDevice;    
    ctx.bufferCount = pDesc->BufferCount;    
    ctx.compute->createCommandListContext(ctx.cmdQueue, ctx.bufferCount, ctx.cmdList, "imgui-cmdlist");    
    ctx.frameMeter.reset();    
    return S_OK;
}

HRESULT slHookCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{    
    auto& ctx = (*sl::imgui::getContext());
    ctx.cmdQueue = (chi::CommandQueue)pDevice;
    ctx.bufferCount = pDesc->BufferCount;
    ctx.compute->createCommandListContext(ctx.cmdQueue, ctx.bufferCount, ctx.cmdList, "imgui-cmdlist");
    ctx.frameMeter.reset();
    return S_OK;
}

HRESULT slHookPresent(IDXGISwapChain* swapChain, UINT SyncInterval, UINT Flags, bool& Skip)
{    
    auto& ctx = (*sl::imgui::getContext());

    if (ctx.renderInternal)
    {
        if (ctx.cmdQueue && ctx.cmdList)
        {
            ctx.frameMeter.timestamp();
            ctx.currentBackBufferIndex = ((IDXGISwapChain4*)swapChain)->GetCurrentBackBufferIndex();
            swapChain->GetBuffer(ctx.currentBackBufferIndex, __uuidof(ID3D12Resource), (void**)&ctx.currentBackBuffer);
            DXGI_SWAP_CHAIN_DESC swapChainDesc;
            swapChain->GetDesc(&swapChainDesc);
            BOOL currentFullscreenState = {};
            swapChain->GetFullscreenState(&currentFullscreenState, NULL);

            if (ctx.backBufferWidth    != swapChainDesc.BufferDesc.Width  ||
                ctx.backBufferHeight   != swapChainDesc.BufferDesc.Height ||
                currentFullscreenState != ctx.dxgiFullscreenState)
            {
                ctx.backBufferWidth = swapChainDesc.BufferDesc.Width;
                ctx.backBufferHeight = swapChainDesc.BufferDesc.Height;
                ctx.dxgiFullscreenState = currentFullscreenState;
                if (sl::imgui::g_ctx != nullptr)
                {
                    ctx.ui.destroyContext(nullptr);
                }
            }

            if (sl::imgui::g_ctx == nullptr)
            {               
                ctx.backBufferFormat = swapChainDesc.BufferDesc.Format;
                ctx.backBufferWidth = swapChainDesc.BufferDesc.Width;
                ctx.backBufferHeight = swapChainDesc.BufferDesc.Height;
                ctx.backBufferHwnd = swapChainDesc.OutputWindow;

                sl::imgui::ContextDesc contextDesc =
                {
                    (uint32_t)ctx.backBufferFormat,
                    ctx.backBufferWidth,
                    ctx.backBufferHeight,
                    ctx.backBufferHwnd
                };

                sl::imgui::Context* imguiCtx = ctx.ui.createContext(contextDesc);
                ctx.ui.setCurrentContext(imguiCtx);
                auto style = ctx.ui.getStyle();
                ctx.ui.setStyleColors(style, imgui::StyleColorsPreset::eNvidiaDark);
            }

            renderInternal();

            ctx.currentBackBuffer->Release();
        }
    }
    return S_OK;
}

HRESULT slHookPresent1(IDXGISwapChain* swapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters, bool& Skip)
{
    return slHookPresent(swapChain, SyncInterval, Flags, Skip);    
}

// VK Hooks

VkResult slHookVkCreateSwapchainKHR(VkDevice Device, const VkSwapchainCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSwapchainKHR* Swapchain, bool& Skip)
{
    auto& ctx = (*sl::imgui::getContext());

    if (ctx.renderInternal)
    {
        // Cache swap params (defer creation of imgui context until present)
        ctx.vkSwapChainParams = *CreateInfo;
        ctx.backBufferFormat  = ctx.vkSwapChainParams.imageFormat;
        ctx.backBufferWidth   = ctx.vkSwapChainParams.imageExtent.width;
        ctx.backBufferHeight  = ctx.vkSwapChainParams.imageExtent.height;
        ctx.bufferCount       = ctx.vkSwapChainParams.minImageCount;

        // Find & cache the HWND
        const auto surfaceIt = std::find_if(ctx.surfaceWindows.begin(), ctx.surfaceWindows.end(), [CreateInfo](const auto& it) { return it.first == CreateInfo->surface; });
        if (surfaceIt != ctx.surfaceWindows.end())
        {
            ctx.backBufferHwnd = surfaceIt->second;
        }
        else
        {
            // Find first window associated with our process (there is no way to get HWND from VK swap-chain)
            HWND hCurWnd = NULL;
            do
            {
                hCurWnd = FindWindowEx(NULL, hCurWnd, NULL, NULL);
                DWORD dwProcID = 0;
                GetWindowThreadProcessId(hCurWnd, &dwProcID);
                if (dwProcID == GetCurrentProcessId())
                {
                    ctx.backBufferHwnd = hCurWnd;
                    break;
                }
            } while (hCurWnd != NULL);
        }
    }

    return VK_SUCCESS;
}

VkResult slHookVkCreateSwapchainKHRPost(VkDevice Device, const VkSwapchainCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSwapchainKHR* Swapchain)
{
    auto& ctx = (*sl::imgui::getContext());

    if (ctx.renderInternal)
    {
        ctx.vkSwapchainInfoMap[*Swapchain] = imgui::VKSwapchainInfo{ ctx.vkSwapChainParams, {}, ctx.backBufferFormat, ctx.backBufferWidth, ctx.backBufferHeight, ctx.bufferCount, ctx.backBufferHwnd };
    }

    return VK_SUCCESS;
}

void slHookVkDestroySwapchainKHR(VkDevice Device, VkSwapchainKHR Swapchain, const VkAllocationCallbacks* Allocator, bool& Skip)
{
    auto& ctx = (*sl::imgui::getContext());

    if (ctx.renderInternal)
    {
        if (auto search = ctx.vkSwapchainInfoMap.find(Swapchain); search != ctx.vkSwapchainInfoMap.end())
        {
            ctx.vkSwapchainInfoMap.erase(search);
        }

        if (ctx.vkSwapchain == Swapchain && sl::imgui::g_ctx != nullptr)
        {
            ctx.ui.destroyContext(nullptr);
        }

        if (ctx.vkSwapchainInfoMap.empty())
        {
            ctx.vkSwapChainParams = {};
            ctx.vkSwapchainImages.clear();
            ctx.backBufferFormat = {};
            ctx.backBufferWidth = {};
            ctx.backBufferHeight = {};
            ctx.bufferCount = {};
            ctx.backBufferHwnd = {};
            ctx.vkSwapchain = {};
        }
    }
}

VkResult slHookVkCreateWin32SurfaceKHR(VkInstance Instance, const VkWin32SurfaceCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSurfaceKHR* Surface)
{
    auto& ctx = (*sl::imgui::getContext());
    ctx.surfaceWindows.emplace_back(*Surface, CreateInfo->hwnd);
    return VK_SUCCESS;
}

void slHookVkDestroySurfaceKHR(VkInstance Instance, VkSurfaceKHR Surface, const VkAllocationCallbacks* Allocator, bool& skip)
{
    auto& ctx = (*sl::imgui::getContext());
    ctx.surfaceWindows.erase(std::remove_if(ctx.surfaceWindows.begin(), ctx.surfaceWindows.end(), [Surface](const auto& it) { return it.first == Surface; }), ctx.surfaceWindows.end());
}

VkResult slHookVkGetSwapchainImagesKHR(VkDevice Device, VkSwapchainKHR Swapchain, uint32_t* SwapchainImageCount, VkImage* SwapchainImages, bool& Skip)
{
    auto& ctx = (*sl::imgui::getContext());
    if (ctx.renderInternal)
    {
        auto& vkSwapchainImages = ctx.vkSwapchainInfoMap[Swapchain].vkSwapchainImages;
        uint32_t imageCount{ static_cast<uint32_t>(vkSwapchainImages.size()) };
        vkGetSwapchainImagesKHR(Device, Swapchain, &imageCount, SwapchainImages == NULL ? NULL : vkSwapchainImages.data());
        if (imageCount != 0 && vkSwapchainImages.size() != imageCount)
        {
            vkSwapchainImages.resize(imageCount);
            ctx.vkSwapchainInfoMap[Swapchain].bufferCount = imageCount;
        }
    }

    return VK_SUCCESS;
}

VkResult slHookVkPresent(VkQueue Queue, const VkPresentInfoKHR* PresentInfo, bool& Skip)
{
    auto& ctx = (*sl::imgui::getContext());

    if (ctx.renderInternal)
    {
        // This may have to be modified to account for presents on multiple swapchains in future (PresentInfo->swapchainCount > 1)
        assert(PresentInfo->swapchainCount == 1);
        auto currentSwapChain = PresentInfo->pSwapchains[0];
        auto search = ctx.vkSwapchainInfoMap.find(currentSwapChain);
        if (search == ctx.vkSwapchainInfoMap.end())
        {
            SL_LOG_ERROR("Invalid swapchain %p, not created by VkCreateSwapchainKHR API, passed in! Using previous valid swapchain data", currentSwapChain);
        }
        else
        {
            const auto& swapchainData = search->second;
            ctx.vkSwapChainParams = swapchainData.vkSwapChainParams;
            ctx.vkSwapchainImages = swapchainData.vkSwapchainImages;
            ctx.backBufferFormat = swapchainData.backBufferFormat;
            ctx.backBufferWidth = swapchainData.backBufferWidth;
            ctx.backBufferHeight = swapchainData.backBufferHeight;
            ctx.backBufferHwnd = swapchainData.backBufferHwnd;
            ctx.bufferCount = swapchainData.bufferCount;
        }

        if (sl::imgui::g_ctx == nullptr)
        {
            // 1: Queue
            uint32_t presentQueueFamilyIndex{}, presentQueueIndex{};
            auto hostPresentQueue = reinterpret_cast<chi::CommandQueue>(Queue);
            interposer::QueueVkInfo qInfo{};
            if (ctx.compute->getHostQueueInfo(hostPresentQueue, &qInfo) != chi::ComputeStatus::eOk)
            {
                SL_LOG_ERROR("Invalid VK app queue - %p!", Queue);
            }
            // ImGUI can render only using graphics queue.
            assert((qInfo.flags & VK_QUEUE_GRAPHICS_BIT) != 0);
            if (ctx.cmdQueue == nullptr)
            {
                ctx.cmdQueue = new chi::CommandQueueVk(Queue, chi::CommandQueueType::eGraphics, qInfo.familyIndex, qInfo.index);
            }
            else
            {
                chi::CommandQueueVk* pCmdQueue = reinterpret_cast<chi::CommandQueueVk*>(ctx.cmdQueue);
                pCmdQueue->native = Queue;
                pCmdQueue->family = qInfo.familyIndex;
                pCmdQueue->index = qInfo.index;
            }

            // 2: Command list            
            if (ctx.cmdList != nullptr)
            {
                ctx.compute->destroyCommandListContext(ctx.cmdList);
            }
            chi::ComputeStatus ret = ctx.compute->createCommandListContext(ctx.cmdQueue, ctx.bufferCount, ctx.cmdList, "imgui-cmdlist");
            if (ret != chi::ComputeStatus::eOk)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            // 3: imgui context
            sl::imgui::ContextDesc contextDesc =
            {
                (uint32_t)ctx.backBufferFormat,
                ctx.backBufferWidth,
                ctx.backBufferHeight,
                ctx.backBufferHwnd
            };

            sl::imgui::Context* imguiCtx = ctx.ui.createContext(contextDesc);
            ctx.ui.setCurrentContext(imguiCtx);
            ctx.vkSwapchain = currentSwapChain;
            auto style = ctx.ui.getStyle();
            ctx.ui.setStyleColors(style, imgui::StyleColorsPreset::eNvidiaDark);           
        }

        ctx.frameMeter.timestamp();
       
        ctx.currentBackBufferIndex = PresentInfo->pImageIndices[0];
        ctx.currentBackBufferVk = ctx.vkSwapchainImages[ctx.currentBackBufferIndex];

        renderInternal();
    }
    return VK_SUCCESS;
}

//! The only exported function - gateway to all functionality
SL_EXPORT void* slGetPluginFunction(const char* functionName)
{
    //! Core API
    SL_EXPORT_FUNCTION(slOnPluginLoad);
    SL_EXPORT_FUNCTION(slOnPluginShutdown);
    SL_EXPORT_FUNCTION(slOnPluginStartup);

    // Hooks
    SL_EXPORT_FUNCTION(slHookCreateSwapChainForCoreWindow);
    SL_EXPORT_FUNCTION(slHookCreateSwapChainForHwnd);
    SL_EXPORT_FUNCTION(slHookCreateSwapChain);
    SL_EXPORT_FUNCTION(slHookPresent);
    SL_EXPORT_FUNCTION(slHookPresent1);
    SL_EXPORT_FUNCTION(slHookVkCreateSwapchainKHR);
    SL_EXPORT_FUNCTION(slHookVkCreateSwapchainKHRPost);
    SL_EXPORT_FUNCTION(slHookVkDestroySwapchainKHR);
    SL_EXPORT_FUNCTION(slHookVkCreateWin32SurfaceKHR);
    SL_EXPORT_FUNCTION(slHookVkDestroySurfaceKHR);
    SL_EXPORT_FUNCTION(slHookVkGetSwapchainImagesKHR);
    SL_EXPORT_FUNCTION(slHookVkPresent);

    return nullptr;
}
}

