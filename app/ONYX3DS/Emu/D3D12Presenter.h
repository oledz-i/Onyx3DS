// SPDX-License-Identifier: GPL-3.0-or-later
//
// Puts emulator frames on the TV.
//
// Azahar renders with Vulkan, which on Xbox runs through Dozen (Mesa's
// Vulkan-on-D3D12 driver) on its own D3D12 device. Each finished frame is
// copied by Vulkan into one of three textures that this presenter created as
// *shared* D3D12 resources, and a shared D3D12 fence tells this device when
// the copy has landed. A dedicated render thread then scales the newest
// frame into a swap chain attached to the XAML SwapChainPanel, at the TV's
// refresh rate, independent of how fast the emulator is running.
#pragma once

#include "onyx/settings.h"

namespace onyx::app {

class D3D12Presenter {
public:
    static constexpr int kSlots = 3;

    struct SharedSlot {
        winrt::com_ptr<ID3D12Resource> texture;
        HANDLE shared_handle = nullptr;  // NT handle Vulkan imports
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t ready_value = 0;        // shared fence value that marks the copy done
        uint64_t last_read_value = 0;    // presenter fence value after the last draw reading it
    };

    struct Stats {
        double present_fps = 0;
        double emu_fps = 0;
        uint32_t frame_width = 0;
        uint32_t frame_height = 0;
    };

    D3D12Presenter() = default;
    ~D3D12Presenter();
    D3D12Presenter(const D3D12Presenter&) = delete;
    D3D12Presenter& operator=(const D3D12Presenter&) = delete;

    // Creates the device, queue and shared fence. Call once at startup.
    bool Initialize(std::string& error);
    // Binds a swap chain to the panel (UI thread) and starts the render thread.
    void Attach(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel);
    void Detach();

    LUID AdapterLuid() const { return luid_; }
    ID3D12Device* Device() const { return device_.get(); }
    HANDLE SharedFenceHandle() const { return shared_fence_handle_; }

    // --- producer side (emulation thread) ---------------------------------
    // Returns a slot the producer may overwrite, (re)creating its texture for
    // the given size. Blocks briefly if the GPU is still reading it.
    SharedSlot* BeginWrite(uint32_t width, uint32_t height, int& slot_index);
    // The copy into `slot_index` signals the shared fence with `ready_value`
    // (0 when the producer already waited on the CPU).
    void EndWrite(int slot_index, uint64_t ready_value);
    // Gives the slot back without publishing it (the copy failed).
    void AbortWrite(int slot_index);
    // Software-rendered frame (XRGB8888 rows, as libretro hands them over):
    // copied into a slot through an upload buffer on this presenter's own
    // device. No Vulkan involved. Returns false if the frame was dropped.
    bool PushCpuFrame(const void* data, uint32_t width, uint32_t height, size_t pitch);
    // Shows colour bars through the same upload path (start-up self-test).
    void ShowTestPattern();

    // Software frames are also kept as BGRA pixels for the XAML image view,
    // which shows them without the D3D12 swap chain. Copies the newest frame
    // if it is newer than `seq` and returns true.
    bool TakeMirror(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height, uint64_t& seq);
    void ClearMirror();

    // Generation counter: bumps when a slot's texture is recreated, so the
    // Vulkan side knows to re-import it.
    uint32_t SlotGeneration(int slot_index) const;

    // --- settings ------------------------------------------------------------
    void SetFilter(ScreenFilter filter) { filter_.store(static_cast<int>(filter)); }
    void SetPaused(bool paused) { paused_.store(paused); }
    void SetDimmed(bool dimmed) { dimmed_.store(dimmed); } // behind the pause menu
    void OnPanelResized(float width, float height, float scale_x, float scale_y);
    Stats GetStats() const;

    // Copies the latest frame to CPU memory as RGBA8 (for screenshots and
    // save-state thumbnails). Runs on the caller's thread.
    bool CaptureLatest(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

private:
    void RenderLoop();
    bool CreateSwapChain(uint32_t width, uint32_t height);
    bool ResizeSwapChain(uint32_t width, uint32_t height);
    bool CreatePipeline();
    void DrawFrame(int slot, uint32_t back_index);
    void WaitForGpu();
    void UpdateSrv(int slot);

    winrt::com_ptr<IDXGIFactory4> factory_;
    winrt::com_ptr<IDXGIAdapter1> adapter_;
    winrt::com_ptr<ID3D12Device> device_;
    winrt::com_ptr<ID3D12CommandQueue> queue_;
    winrt::com_ptr<IDXGISwapChain3> swapchain_;
    HANDLE frame_waitable_ = nullptr;
    static constexpr UINT kBackBuffers = 3;
    std::array<winrt::com_ptr<ID3D12Resource>, kBackBuffers> back_buffers_;
    std::array<winrt::com_ptr<ID3D12CommandAllocator>, kBackBuffers> allocators_;
    std::array<uint64_t, kBackBuffers> allocator_fence_{};
    winrt::com_ptr<ID3D12GraphicsCommandList> cmd_;
    winrt::com_ptr<ID3D12DescriptorHeap> rtv_heap_;
    winrt::com_ptr<ID3D12DescriptorHeap> srv_heap_;
    UINT rtv_stride_ = 0;
    UINT srv_stride_ = 0;
    winrt::com_ptr<ID3D12RootSignature> root_sig_;
    winrt::com_ptr<ID3D12PipelineState> pso_;

    // Newest software frame as opaque BGRA for the XAML view.
    std::mutex mirror_mutex_;
    std::vector<uint8_t> mirror_;
    uint32_t mirror_w_ = 0, mirror_h_ = 0;
    uint64_t mirror_seq_ = 0;
    void Mirror(const void* data, uint32_t width, uint32_t height, size_t pitch);

    // CPU frame uploads (software renderer).
    std::mutex upload_mutex_;
    winrt::com_ptr<ID3D12Resource> upload_buffer_;
    uint64_t upload_size_ = 0;
    uint8_t* upload_mapped_ = nullptr;
    winrt::com_ptr<ID3D12CommandAllocator> upload_alloc_;
    winrt::com_ptr<ID3D12GraphicsCommandList> upload_cmd_;
    winrt::com_ptr<ID3D12Fence> upload_fence_;
    uint64_t upload_fence_value_ = 0;
    HANDLE upload_event_ = nullptr;

    // Presenter-local fence (GPU progress of this queue).
    winrt::com_ptr<ID3D12Fence> fence_;
    uint64_t fence_value_ = 0;
    HANDLE fence_event_ = nullptr;

    // Fence shared with Vulkan (signalled by the Dozen device).
    winrt::com_ptr<ID3D12Fence> shared_fence_;
    HANDLE shared_fence_handle_ = nullptr;

    mutable std::mutex slot_mutex_;
    std::condition_variable slot_cv_;
    std::array<SharedSlot, kSlots> slots_{};
    std::array<uint32_t, kSlots> generation_{};
    int latest_ = -1;     // newest finished frame not yet picked up
    int displaying_ = -1; // slot the render thread is showing
    int writing_ = -1;    // slot the producer is filling

    winrt::Windows::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> resize_pending_{false};
    std::atomic<uint32_t> want_width_{1920}, want_height_{1080};
    std::atomic<float> scale_x_{1.0f}, scale_y_{1.0f};
    uint32_t sc_width_ = 0, sc_height_ = 0;
    HRESULT last_present_hr_ = S_OK;
    // While set, newer frames wait so the self-test pattern stays visible.
    std::chrono::steady_clock::time_point hold_until_{};
    int last_drawn_slot_ = -1;

    std::atomic<int> filter_{static_cast<int>(ScreenFilter::Smooth)};
    std::atomic<bool> paused_{false};
    std::atomic<bool> dimmed_{false};

    // Stats
    mutable std::mutex stats_mutex_;
    Stats stats_{};
    uint32_t presents_in_window_ = 0;
    uint32_t frames_in_window_ = 0;
    std::chrono::steady_clock::time_point window_start_{};
    LUID luid_{};
};

} // namespace onyx::app
