// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Emu/D3D12Presenter.h"

#include "Platform/Log.h"

// Generated at build time by FxCompile (see ONYX3DS.vcxproj).
#include "PresentPS.h"
#include "PresentVS.h"

namespace onyx::app {

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

struct PresentConstants {
    float dst_rect[4];
    float sizes[4];
    uint32_t mode;
    float dim;
    float time;
    float pad;
};
static_assert(sizeof(PresentConstants) == 12 * 4);

} // namespace

D3D12Presenter::~D3D12Presenter() {
    running_ = false;
    if (render_thread_.joinable()) render_thread_.join();
    if (device_) WaitForGpu();
    for (auto& s : slots_)
        if (s.shared_handle) CloseHandle(s.shared_handle);
    if (shared_fence_handle_) CloseHandle(shared_fence_handle_);
    if (fence_event_) CloseHandle(fence_event_);
    if (upload_event_) CloseHandle(upload_event_);
}

bool D3D12Presenter::Initialize(std::string& error) {
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.put())))) {
        error = "DXGI factory creation failed";
        return false;
    }
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory_->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        // Device Removed Extended Data: if the GPU device is lost (the hardware
        // renderer runs on this same device), the log can say which command
        // stopped it. Must be set before the device exists; fails harmlessly.
        {
            winrt::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dred;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dred.put())))) {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(device_.put())))) {
            adapter_ = adapter;
            luid_ = desc.AdapterLuid;
            ONYX_INFO("D3D12 presenter on %s (%llu MB VRAM)", Utf8(std::wstring(desc.Description)).c_str(),
                      static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
            break;
        }
    }
    if (!device_) {
        error = "No D3D12 device available";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.put())))) {
        error = "D3D12 queue creation failed";
        return false;
    }
    winrt::check_hresult(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put())));
    fence_event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

    // The fence Vulkan signals after copying a frame. Shared through an NT
    // handle that Dozen imports as a timeline semaphore.
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(shared_fence_.put()))) ||
        FAILED(device_->CreateSharedHandle(shared_fence_.get(), nullptr, GENERIC_ALL, nullptr,
                                           &shared_fence_handle_))) {
        ONYX_WARN("Shared fence unavailable; frames will be synchronised on the CPU");
        shared_fence_handle_ = nullptr;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = kBackBuffers;
    winrt::check_hresult(device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(rtv_heap_.put())));
    rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = kSlots;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    winrt::check_hresult(device_->CreateDescriptorHeap(&srv, IID_PPV_ARGS(srv_heap_.put())));
    srv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (auto& a : allocators_)
        winrt::check_hresult(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             IID_PPV_ARGS(a.put())));
    winrt::check_hresult(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    allocators_[0].get(), nullptr,
                                                    IID_PPV_ARGS(cmd_.put())));
    cmd_->Close();

    if (!CreatePipeline()) {
        error = "Presenter pipeline creation failed";
        return false;
    }
    window_start_ = std::chrono::steady_clock::now();
    return true;
}

bool D3D12Presenter::CreatePipeline() {
    // The root signature is embedded in the vertex shader ([RootSignature]).
    if (FAILED(device_->CreateRootSignature(0, g_PresentVS, sizeof(g_PresentVS),
                                            IID_PPV_ARGS(root_sig_.put()))))
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_sig_.get();
    pd.VS = {g_PresentVS, sizeof(g_PresentVS)};
    pd.PS = {g_PresentPS, sizeof(g_PresentPS)};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kBackBufferFormat;
    pd.SampleDesc.Count = 1;
    return SUCCEEDED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pso_.put())));
}

bool D3D12Presenter::CreateSwapChain(uint32_t width, uint32_t height) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = kBackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBackBuffers;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    winrt::com_ptr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForComposition(queue_.get(), &sd, nullptr, sc1.put()))) {
        ONYX_ERROR("CreateSwapChainForComposition failed");
        return false;
    }
    swapchain_ = sc1.as<IDXGISwapChain3>();
    swapchain_->SetMaximumFrameLatency(1);
    frame_waitable_ = swapchain_->GetFrameLatencyWaitableObject();
    sc_width_ = width;
    sc_height_ = height;
    return ResizeSwapChain(width, height);
}

bool D3D12Presenter::ResizeSwapChain(uint32_t width, uint32_t height) {
    for (auto& b : back_buffers_) b = nullptr;
    if (width != sc_width_ || height != sc_height_) {
        if (FAILED(swapchain_->ResizeBuffers(kBackBuffers, width, height, kBackBufferFormat,
                                             DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
            return false;
        sc_width_ = width;
        sc_height_ = height;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBackBuffers; ++i) {
        winrt::check_hresult(swapchain_->GetBuffer(i, IID_PPV_ARGS(back_buffers_[i].put())));
        device_->CreateRenderTargetView(back_buffers_[i].get(), nullptr, h);
        h.ptr += rtv_stride_;
    }
    // The panel lays out in effective pixels; undo its scale so one back
    // buffer pixel is one physical pixel.
    DXGI_MATRIX_3X2_F inverse{};
    inverse._11 = 1.0f / scale_x_.load();
    inverse._22 = 1.0f / scale_y_.load();
    swapchain_.as<IDXGISwapChain2>()->SetMatrixTransform(&inverse);
    return true;
}

void D3D12Presenter::Attach(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel) {
    panel_ = panel;
    const float sx = panel.CompositionScaleX(), sy = panel.CompositionScaleY();
    scale_x_ = sx > 0 ? sx : 1.0f;
    scale_y_ = sy > 0 ? sy : 1.0f;
    const uint32_t w = std::max<uint32_t>(64, static_cast<uint32_t>(panel.ActualWidth() * scale_x_));
    const uint32_t h = std::max<uint32_t>(64, static_cast<uint32_t>(panel.ActualHeight() * scale_y_));
    want_width_ = w;
    want_height_ = h;
    if (!swapchain_ && !CreateSwapChain(w, h)) return;
    winrt::check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(swapchain_.get()));
    running_ = true;
    render_thread_ = std::thread([this] { RenderLoop(); });
}

void D3D12Presenter::Detach() {
    running_ = false;
    if (render_thread_.joinable()) render_thread_.join();
    WaitForGpu();
    if (panel_) {
        panel_.as<ISwapChainPanelNative>()->SetSwapChain(nullptr);
        panel_ = nullptr;
    }
    for (auto& b : back_buffers_) b = nullptr;
    swapchain_ = nullptr;
    frame_waitable_ = nullptr;
    std::lock_guard lock(slot_mutex_);
    latest_ = displaying_ = -1;
}

void D3D12Presenter::OnPanelResized(float width, float height, float sx, float sy) {
    scale_x_ = sx > 0 ? sx : 1.0f;
    scale_y_ = sy > 0 ? sy : 1.0f;
    want_width_ = std::max<uint32_t>(64, static_cast<uint32_t>(width * scale_x_));
    want_height_ = std::max<uint32_t>(64, static_cast<uint32_t>(height * scale_y_));
    resize_pending_ = true;
}

void D3D12Presenter::WaitForGpu() {
    if (!queue_ || !fence_) return;
    const uint64_t v = ++fence_value_;
    queue_->Signal(fence_.get(), v);
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fence_event_);
        WaitForSingleObjectEx(fence_event_, 2000, FALSE);
    }
}

void D3D12Presenter::UpdateSrv(int slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = slots_[slot].texture->GetDesc().Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * srv_stride_;
    device_->CreateShaderResourceView(slots_[slot].texture.get(), &sd, h);
}

D3D12Presenter::SharedSlot* D3D12Presenter::BeginWrite(uint32_t width, uint32_t height,
                                                       int& slot_index) {
    int s = -1;
    uint64_t wait_value = 0;
    {
        std::lock_guard lock(slot_mutex_);
        for (int i = 0; i < kSlots; ++i) {
            if (i != latest_ && i != displaying_) {
                s = i;
                break;
            }
        }
        if (s < 0) return nullptr; // cannot happen with 3 slots, but be safe
        writing_ = s;
        wait_value = slots_[s].last_read_value;
    }
    // The render thread may still have GPU work in flight that samples this
    // slot from before it moved on; wait for that work, not for vsync.
    if (fence_->GetCompletedValue() < wait_value) {
        fence_->SetEventOnCompletion(wait_value, nullptr); // blocks until reached
    }

    SharedSlot& slot = slots_[s];
    if (!slot.texture || slot.width != width || slot.height != height) {
        if (slot.shared_handle) {
            CloseHandle(slot.shared_handle);
            slot.shared_handle = nullptr;
        }
        slot.texture = nullptr;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // Render-target capable so Dozen can use its meta blit path, and
        // simultaneous-access so neither device has to track resource states.
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                   D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(slot.texture.put()))) ||
            FAILED(device_->CreateSharedHandle(slot.texture.get(), nullptr, GENERIC_ALL, nullptr,
                                               &slot.shared_handle))) {
            ONYX_ERROR("Shared frame texture %ux%u could not be created", width, height);
            slot.texture = nullptr;
            return nullptr;
        }
        slot.width = width;
        slot.height = height;
        UpdateSrv(s);
        ++generation_[s];
        ONYX_DEBUG("Frame slot %d is now %ux%u", s, width, height);
    }
    slot_index = s;
    return &slot;
}

void D3D12Presenter::EndWrite(int slot_index, uint64_t ready_value) {
    std::lock_guard lock(slot_mutex_);
    slots_[slot_index].ready_value = ready_value;
    latest_ = slot_index;
    writing_ = -1;
    ++frames_in_window_;
}

void D3D12Presenter::AbortWrite(int slot_index) {
    std::lock_guard lock(slot_mutex_);
    if (writing_ == slot_index) writing_ = -1;
}

bool D3D12Presenter::PushCpuFrame(const void* data, uint32_t width, uint32_t height,
                                  size_t pitch) {
    if (!data || width == 0 || height == 0 || !device_) return false;
    std::lock_guard upload_lock(upload_mutex_);
    static int s_logged = 0;
    auto fail = [&](const char* what, HRESULT hr) {
        if (s_logged < 8) {
            ++s_logged;
            ONYX_ERROR("Software frame %ux%u dropped: %s (0x%08X)", width, height, what,
                       static_cast<unsigned>(hr));
        }
        return false;
    };
    try {
        if (!upload_cmd_) {
            winrt::check_hresult(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                 IID_PPV_ARGS(upload_alloc_.put())));
            winrt::check_hresult(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            upload_alloc_.get(), nullptr,
                                                            IID_PPV_ARGS(upload_cmd_.put())));
            upload_cmd_->Close();
            winrt::check_hresult(
                device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(upload_fence_.put())));
            upload_event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        }

        int s = -1;
        SharedSlot* slot = BeginWrite(width, height, s);
        if (!slot) return fail("no frame slot", device_->GetDeviceRemovedReason());

        const D3D12_RESOURCE_DESC desc = slot->texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

        if (!upload_buffer_ || upload_size_ < total) {
            if (upload_buffer_) upload_buffer_->Unmap(0, nullptr);
            upload_buffer_ = nullptr;
            upload_mapped_ = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(upload_buffer_.put()));
            if (SUCCEEDED(hr))
                hr = upload_buffer_->Map(0, nullptr, reinterpret_cast<void**>(&upload_mapped_));
            if (FAILED(hr)) {
                upload_buffer_ = nullptr;
                upload_mapped_ = nullptr;
                AbortWrite(s);
                return fail("upload buffer", hr);
            }
            upload_size_ = total;
        }

        // XRGB8888 (bytes B,G,R,X) -> RGBA8 with opaque alpha.
        const auto* src = static_cast<const uint8_t*>(data);
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t* in = reinterpret_cast<const uint32_t*>(src + y * pitch);
            uint32_t* out = reinterpret_cast<uint32_t*>(upload_mapped_ + fp.Offset +
                                                        static_cast<size_t>(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t p = in[x];
                out[x] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
        }

        upload_alloc_->Reset();
        upload_cmd_->Reset(upload_alloc_.get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = slot->texture.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION srcloc{};
        srcloc.pResource = upload_buffer_.get();
        srcloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcloc.PlacedFootprint = fp;
        // Simultaneous-access texture: implicitly promoted to COPY_DEST, decays to COMMON.
        upload_cmd_->CopyTextureRegion(&dst, 0, 0, 0, &srcloc, nullptr);
        upload_cmd_->Close();
        ID3D12CommandList* lists[] = {upload_cmd_.get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t v = ++upload_fence_value_;
        queue_->Signal(upload_fence_.get(), v);
        if (upload_fence_->GetCompletedValue() < v) {
            upload_fence_->SetEventOnCompletion(v, upload_event_);
            if (WaitForSingleObject(upload_event_, 1000) != WAIT_OBJECT_0) {
                AbortWrite(s);
                return fail("GPU copy timed out", device_->GetDeviceRemovedReason());
            }
        }
        EndWrite(s, 0); // already complete on the GPU
        static uint64_t s_frames = 0;
        if (++s_frames == 1 || s_frames % 600 == 0)
            ONYX_INFO("Software frame %llu shown (%ux%u)", static_cast<unsigned long long>(s_frames),
                      width, height);
        return true;
    } catch (winrt::hresult_error const& e) {
        return fail("D3D12 error", e.code());
    } catch (...) {
        return fail("unexpected error", E_FAIL);
    }
}

uint32_t D3D12Presenter::SlotGeneration(int slot_index) const {
    std::lock_guard lock(slot_mutex_);
    return generation_[slot_index];
}

void D3D12Presenter::RenderLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (running_) {
        if (frame_waitable_) WaitForSingleObjectEx(frame_waitable_, 100, TRUE);
        if (!running_) break;

        if (resize_pending_.exchange(false)) {
            WaitForGpu();
            ResizeSwapChain(want_width_, want_height_);
        }

        int slot;
        {
            std::lock_guard lock(slot_mutex_);
            if (latest_ >= 0 && !paused_) {
                displaying_ = latest_;
                latest_ = -1;
            }
            slot = displaying_;
        }
        const UINT back = swapchain_->GetCurrentBackBufferIndex();
        if (fence_->GetCompletedValue() < allocator_fence_[back]) {
            fence_->SetEventOnCompletion(allocator_fence_[back], fence_event_);
            WaitForSingleObjectEx(fence_event_, 1000, FALSE);
        }
        DrawFrame(slot, back);
        swapchain_->Present(1, 0);
        ++presents_in_window_;

        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - window_start_).count();
        if (secs >= 0.5) {
            std::lock_guard lock(stats_mutex_);
            stats_.present_fps = presents_in_window_ / secs;
            {
                std::lock_guard slock(slot_mutex_);
                stats_.emu_fps = frames_in_window_ / secs;
                frames_in_window_ = 0;
                if (slot >= 0) {
                    stats_.frame_width = slots_[slot].width;
                    stats_.frame_height = slots_[slot].height;
                }
            }
            presents_in_window_ = 0;
            window_start_ = now;
        }
    }
}

void D3D12Presenter::DrawFrame(int slot, UINT back) {
    ID3D12CommandAllocator* alloc = allocators_[back].get();
    alloc->Reset();
    cmd_->Reset(alloc, pso_.get());

    ID3D12Resource* bb = back_buffers_[back].get();
    auto to_rt = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd_->ResourceBarrier(1, &to_rt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(back) * rtv_stride_;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd_->ClearRenderTargetView(rtv, clear, 0, nullptr);

    uint64_t wait_value = 0;
    if (slot >= 0 && slots_[slot].texture) {
        const SharedSlot& s = slots_[slot];
        wait_value = s.ready_value;
        // Letterbox: fit the frame, keep its aspect ratio, centre it.
        const float fw = static_cast<float>(s.width), fh = static_cast<float>(s.height);
        const float ow = static_cast<float>(sc_width_), oh = static_cast<float>(sc_height_);
        const float scale = std::min(ow / fw, oh / fh);
        const float dw = fw * scale, dh = fh * scale;
        PresentConstants c{};
        c.dst_rect[0] = -dw / ow;
        c.dst_rect[1] = dh / oh;
        c.dst_rect[2] = dw / ow;
        c.dst_rect[3] = -dh / oh;
        c.sizes[0] = fw;
        c.sizes[1] = fh;
        c.sizes[2] = dw;
        c.sizes[3] = dh;
        c.mode = static_cast<uint32_t>(filter_.load());
        c.dim = dimmed_ ? 0.35f : 1.0f;
        c.time = 0;

        ID3D12DescriptorHeap* heaps[] = {srv_heap_.get()};
        cmd_->SetDescriptorHeaps(1, heaps);
        cmd_->SetGraphicsRootSignature(root_sig_.get());
        cmd_->SetGraphicsRoot32BitConstants(0, 12, &c, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<UINT64>(slot) * srv_stride_;
        cmd_->SetGraphicsRootDescriptorTable(1, srv);
        D3D12_VIEWPORT vp{0, 0, ow, oh, 0, 1};
        D3D12_RECT sr{0, 0, static_cast<LONG>(sc_width_), static_cast<LONG>(sc_height_)};
        cmd_->RSSetViewports(1, &vp);
        cmd_->RSSetScissorRects(1, &sr);
        cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmd_->DrawInstanced(4, 1, 0, 0);
    }

    auto to_present = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd_->ResourceBarrier(1, &to_present);
    cmd_->Close();

    // GPU-side wait for Vulkan's copy into this slot; free when already done.
    if (wait_value && shared_fence_) queue_->Wait(shared_fence_.get(), wait_value);
    ID3D12CommandList* lists[] = {cmd_.get()};
    queue_->ExecuteCommandLists(1, lists);
    const uint64_t v = ++fence_value_;
    queue_->Signal(fence_.get(), v);
    allocator_fence_[back] = v;
    if (slot >= 0) {
        std::lock_guard lock(slot_mutex_);
        slots_[slot].last_read_value = v;
    }
}

D3D12Presenter::Stats D3D12Presenter::GetStats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

bool D3D12Presenter::CaptureLatest(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    int slot;
    uint64_t ready;
    winrt::com_ptr<ID3D12Resource> tex;
    {
        std::lock_guard lock(slot_mutex_);
        slot = displaying_ >= 0 ? displaying_ : latest_;
        if (slot < 0 || !slots_[slot].texture) return false;
        tex = slots_[slot].texture; // keeps the texture alive even if the slot is recreated
        ready = slots_[slot].ready_value;
        width = slots_[slot].width;
        height = slots_[slot].height;
    }
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    winrt::com_ptr<ID3D12Resource> readback;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(readback.put()))))
        return false;

    winrt::com_ptr<ID3D12CommandAllocator> alloc;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put()));
    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr,
                               IID_PPV_ARGS(list.put()));
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex.get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->Close();
    if (ready && shared_fence_) queue_->Wait(shared_fence_.get(), ready);
    ID3D12CommandList* lists[] = {list.get()};
    queue_->ExecuteCommandLists(1, lists);
    WaitForGpu();

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, &mapped))) return false;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch;
        std::memcpy(rgba.data() + static_cast<size_t>(y) * width * 4, row, width * 4);
    }
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 0xFF; // opaque PNGs
    return true;
}

} // namespace onyx::app
