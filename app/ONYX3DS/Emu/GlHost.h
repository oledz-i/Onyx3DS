// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OpenGL half of the libretro hardware-render contract, backed by Mesa's
// OpenGL-on-D3D12 driver (vendor/mesa-gl: opengl32.dll + libgallium_wgl.dll).
//
//   * Load() loads the driver from the app package once.
//   * CreateContext() runs on the emulation thread: a WGL context with no
//     window behind it (Mesa then gives it a 1x1 offscreen framebuffer), made
//     current there for the whole game, plus the framebuffer object the core
//     draws its output into.
//   * OnFrame() reads that output back with pixel-buffer objects, one frame
//     behind so the CPU never waits for the GPU, and hands it to the presenter.
//
// There is one game at a time, so the libretro callbacks (which have no user
// pointer) reach the host through a single static instance.
#pragma once

#include "Emu/D3D12Presenter.h"

namespace onyx::app {

class GlHost {
public:
    explicit GlHost(D3D12Presenter& presenter);
    ~GlHost();

    bool Load(std::string& error);
    bool Loaded() const { return module_ != nullptr; }

    // Emulation thread only, between retro_load_game and context_reset.
    bool CreateContext(unsigned max_width, unsigned max_height, bool depth, bool stencil,
                       std::string& error);
    void DestroyContext();
    bool HasContext() const { return context_ != nullptr; }
    // Grow the output framebuffer (new max geometry from the core).
    void EnsureSize(unsigned width, unsigned height);

    // A frame is ready in the output framebuffer (video_refresh with
    // RETRO_HW_FRAME_BUFFER_VALID).
    void OnFrame(unsigned width, unsigned height);

    const std::string& Description() const { return description_; }

    // retro_hw_render_callback entry points.
    static uintptr_t GetCurrentFramebuffer();
    static retro_proc_address_t GetProcAddress(const char* symbol);

private:
    struct Fn;
    bool LoadFunctions(std::string& error);
    void CreateTargets(unsigned width, unsigned height);
    void DestroyTargets();
    void DeliverReady(bool wait_for_newest);

    D3D12Presenter& presenter_;
    HMODULE module_ = nullptr;
    std::unique_ptr<Fn> fn_;
    void* dc_ = nullptr;      // HDC: a token Mesa uses only as a key
    void* context_ = nullptr; // HGLRC
    std::string description_;

    // Output framebuffer the core renders into.
    unsigned fbo_ = 0, color_rb_ = 0, depth_rb_ = 0;
    unsigned fbo_w_ = 0, fbo_h_ = 0;
    bool want_depth_ = false, want_stencil_ = false;

    // Read-back ring: frame N goes to slot N % kSlots; the newest finished one
    // is shown.
    static constexpr int kSlots = 3;
    struct Slot {
        unsigned pbo = 0;
        size_t capacity = 0;
        void* fence = nullptr; // GLsync
        unsigned width = 0, height = 0;
        uint64_t serial = 0;
    };
    Slot slots_[kSlots];
    uint64_t next_serial_ = 1;
    uint64_t shown_serial_ = 0;
    uint64_t frames_ = 0;

    static GlHost* s_active;
};

} // namespace onyx::app
