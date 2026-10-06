// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include <atomic>
#include <chrono>
#include "Emu/GlHost.h"

#include "Platform/Log.h"

namespace onyx::app {

namespace {

// The few GL / WGL definitions used here (no GL headers in the app).
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;
using GLuint64 = unsigned long long;
using GLsync = void*;
using GLubyte = unsigned char;
using GLboolean = unsigned char;

constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_MAJOR_VERSION = 0x821B;
constexpr GLenum GL_MINOR_VERSION = 0x821C;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_PACK_ROW_LENGTH = 0x0D02;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_RENDERBUFFER_BINDING = 0x8CA7;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED;
constexpr GLenum GL_STREAM_READ = 0x88E1;
constexpr GLbitfield GL_MAP_READ_BIT = 0x0001;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x00000001;
constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;

// PIXELFORMATDESCRIPTOR (wingdi.h declares it for desktop apps only).
struct PixelFormat {
    WORD nSize;
    WORD nVersion;
    DWORD dwFlags;
    BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits,
        cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits,
        cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType,
        bReserved;
    DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
};
static_assert(sizeof(PixelFormat) == 40, "PIXELFORMATDESCRIPTOR layout");
constexpr DWORD kPfdDoubleBuffer = 0x1, kPfdDrawToWindow = 0x4, kPfdSupportOpenGL = 0x20;

// wglGetProcAddress reports failure with several sentinel values, not just NULL.
bool ValidProc(PROC p) {
    const auto v = reinterpret_cast<intptr_t>(p);
    return v != 0 && v != 1 && v != 2 && v != 3 && v != -1;
}

// Mesa keys a window-less framebuffer on the DC value alone. A fresh value per
// context, since a DC's pixel format can only be set once.
std::atomic<uintptr_t> g_next_dc{0x0A11CE000};

} // namespace

struct GlHost::Fn {
    // WGL (exported by Mesa's opengl32.dll)
    int(WINAPI* ChoosePixelFormat)(void*, const PixelFormat*) = nullptr;
    BOOL(WINAPI* SetPixelFormat)(void*, int, const PixelFormat*) = nullptr;
    void*(WINAPI* CreateContext)(void*) = nullptr;
    BOOL(WINAPI* DeleteContext)(void*) = nullptr;
    BOOL(WINAPI* MakeCurrent)(void*, void*) = nullptr;
    PROC(WINAPI* GetProcAddr)(LPCSTR) = nullptr;
    // GL 1.1
    GLenum(APIENTRY* GetError)() = nullptr;
    const GLubyte*(APIENTRY* GetString)(GLenum) = nullptr;
    void(APIENTRY* GetIntegerv)(GLenum, GLint*) = nullptr;
    void(APIENTRY* PixelStorei)(GLenum, GLint) = nullptr;
    void(APIENTRY* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(APIENTRY* Flush)() = nullptr;
    // GL 3.x
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum) = nullptr;
    void(APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void(APIENTRY* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* BindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(APIENTRY* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* BindBuffer)(GLenum, GLuint) = nullptr;
    void(APIENTRY* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void*(APIENTRY* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
    GLboolean(APIENTRY* UnmapBuffer)(GLenum) = nullptr;
    GLsync(APIENTRY* FenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum(APIENTRY* ClientWaitSync)(GLsync, GLbitfield, GLuint64) = nullptr;
    void(APIENTRY* DeleteSync)(GLsync) = nullptr;
};

GlHost* GlHost::s_active = nullptr;

GlHost::GlHost(D3D12Presenter& presenter) : presenter_(presenter), fn_(std::make_unique<Fn>()) {}

GlHost::~GlHost() {
    // The context belongs to the emulation thread, which destroys it before
    // the session goes away; the driver stays loaded until the app exits.
    if (s_active == this) s_active = nullptr;
}

bool GlHost::Load(std::string& error) {
    if (module_) return true;
    module_ = LoadPackagedLibrary(L"opengl32.dll", 0);
    if (!module_) {
        error = "opengl32.dll (Mesa) is missing from the app package (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    auto& f = *fn_;
#define WGL(name, sym) f.name = reinterpret_cast<decltype(f.name)>(::GetProcAddress(module_, sym))
    WGL(ChoosePixelFormat, "wglChoosePixelFormat");
    WGL(SetPixelFormat, "wglSetPixelFormat");
    WGL(CreateContext, "wglCreateContext");
    WGL(DeleteContext, "wglDeleteContext");
    WGL(MakeCurrent, "wglMakeCurrent");
    WGL(GetProcAddr, "wglGetProcAddress");
#undef WGL
    if (!f.ChoosePixelFormat || !f.SetPixelFormat || !f.CreateContext || !f.DeleteContext ||
        !f.MakeCurrent || !f.GetProcAddr) {
        error = "Mesa's opengl32.dll lacks the WGL entry points";
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
    }
    ONYX_INFO("OpenGL driver loaded (Mesa, OpenGL on D3D12)");
    return true;
}

retro_proc_address_t GlHost::GetProcAddress(const char* symbol) {
    GlHost* self = s_active;
    if (!self || !self->module_ || !symbol) return nullptr;
    PROC p = self->fn_->GetProcAddr(symbol);
    if (!ValidProc(p)) p = ::GetProcAddress(self->module_, symbol); // GL 1.1 entry points
    return ValidProc(p) ? reinterpret_cast<retro_proc_address_t>(p) : nullptr;
}

uintptr_t GlHost::GetCurrentFramebuffer() {
    GlHost* self = s_active;
    return self ? self->fbo_ : 0;
}

bool GlHost::LoadFunctions(std::string& error) {
    auto& f = *fn_;
    bool ok = true;
    auto load = [&](auto& ptr, const char* name) {
        ptr = reinterpret_cast<std::remove_reference_t<decltype(ptr)>>(GetProcAddress(name));
        if (!ptr) {
            ok = false;
            if (error.empty()) error = std::string("OpenGL function missing: ") + name;
        }
    };
    load(f.GetError, "glGetError");
    load(f.GetString, "glGetString");
    load(f.GetIntegerv, "glGetIntegerv");
    load(f.PixelStorei, "glPixelStorei");
    load(f.ReadPixels, "glReadPixels");
    load(f.Flush, "glFlush");
    load(f.GenFramebuffers, "glGenFramebuffers");
    load(f.DeleteFramebuffers, "glDeleteFramebuffers");
    load(f.BindFramebuffer, "glBindFramebuffer");
    load(f.CheckFramebufferStatus, "glCheckFramebufferStatus");
    load(f.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    load(f.GenRenderbuffers, "glGenRenderbuffers");
    load(f.DeleteRenderbuffers, "glDeleteRenderbuffers");
    load(f.BindRenderbuffer, "glBindRenderbuffer");
    load(f.RenderbufferStorage, "glRenderbufferStorage");
    load(f.GenBuffers, "glGenBuffers");
    load(f.DeleteBuffers, "glDeleteBuffers");
    load(f.BindBuffer, "glBindBuffer");
    load(f.BufferData, "glBufferData");
    load(f.MapBufferRange, "glMapBufferRange");
    load(f.UnmapBuffer, "glUnmapBuffer");
    load(f.FenceSync, "glFenceSync");
    load(f.ClientWaitSync, "glClientWaitSync");
    load(f.DeleteSync, "glDeleteSync");
    return ok;
}

bool GlHost::CreateContext(unsigned max_width, unsigned max_height, bool depth, bool stencil,
                           std::string& error) {
    if (!module_ && !Load(error)) return false;
    if (context_) DestroyContext();
    auto& f = *fn_;

    // No window: Mesa gives the context a small offscreen default framebuffer,
    // and the core draws into our framebuffer object instead.
    dc_ = reinterpret_cast<void*>(g_next_dc.fetch_add(0x100));
    PixelFormat pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = kPfdDrawToWindow | kPfdSupportOpenGL | kPfdDoubleBuffer;
    pfd.iPixelType = 0; // PFD_TYPE_RGBA
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int format = f.ChoosePixelFormat(dc_, &pfd);
    if (format == 0) {
        error = "OpenGL: no usable pixel format";
        return false;
    }
    if (!f.SetPixelFormat(dc_, format, &pfd)) {
        error = "OpenGL: setting the pixel format failed";
        return false;
    }
    context_ = f.CreateContext(dc_);
    if (!context_) {
        error = "OpenGL: creating the context failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!f.MakeCurrent(dc_, context_)) {
        error = "OpenGL: making the context current failed";
        f.DeleteContext(context_);
        context_ = nullptr;
        return false;
    }
    s_active = this;
    if (!LoadFunctions(error)) {
        DestroyContext();
        return false;
    }
    auto str = [&](GLenum e) {
        const auto* s = reinterpret_cast<const char*>(f.GetString(e));
        return std::string(s ? s : "?");
    };
    description_ = str(GL_RENDERER) + ", OpenGL " + str(GL_VERSION);
    GLint major = 0, minor = 0;
    f.GetIntegerv(GL_MAJOR_VERSION, &major);
    f.GetIntegerv(GL_MINOR_VERSION, &minor);
    ONYX_INFO("OpenGL context: %s / %s (version %d.%d)", str(GL_VENDOR).c_str(),
              description_.c_str(), major, minor);
    if (major < 4 || (major == 4 && minor < 3)) {
        error = "OpenGL " + std::to_string(major) + "." + std::to_string(minor) +
                " is below the 4.3 the emulator needs";
        DestroyContext();
        return false;
    }
    want_depth_ = depth;
    want_stencil_ = stencil;
    CreateTargets(max_width ? max_width : 400, max_height ? max_height : 480);
    if (!fbo_) {
        error = "OpenGL: the output framebuffer could not be created";
        DestroyContext();
        return false;
    }
    frames_ = 0;
    shown_serial_ = 0;
    next_serial_ = 1;
    return true;
}

void GlHost::CreateTargets(unsigned width, unsigned height) {
    auto& f = *fn_;
    f.GenFramebuffers(1, &fbo_);
    f.GenRenderbuffers(1, &color_rb_);
    f.BindRenderbuffer(GL_RENDERBUFFER, color_rb_);
    f.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    if (want_depth_) {
        f.GenRenderbuffers(1, &depth_rb_);
        f.BindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
        f.RenderbufferStorage(GL_RENDERBUFFER, want_stencil_ ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                              width, height);
    }
    f.BindRenderbuffer(GL_RENDERBUFFER, 0);
    f.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    f.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb_);
    if (depth_rb_)
        f.FramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  want_stencil_ ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, depth_rb_);
    const GLenum status = f.CheckFramebufferStatus(GL_FRAMEBUFFER);
    f.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ONYX_ERROR("OpenGL output framebuffer incomplete (0x%04X)", status);
        DestroyTargets();
        return;
    }
    fbo_w_ = width;
    fbo_h_ = height;
    for (auto& s : slots_)
        if (!s.pbo) f.GenBuffers(1, &s.pbo);
    ONYX_INFO("OpenGL output framebuffer %ux%u%s", width, height,
              depth_rb_ ? (want_stencil_ ? " with depth/stencil" : " with depth") : "");
}

void GlHost::DestroyTargets() {
    auto& f = *fn_;
    for (auto& s : slots_) {
        if (s.fence) f.DeleteSync(s.fence);
        if (s.pbo) f.DeleteBuffers(1, &s.pbo);
        s = {};
    }
    if (fbo_) f.DeleteFramebuffers(1, &fbo_);
    if (color_rb_) f.DeleteRenderbuffers(1, &color_rb_);
    if (depth_rb_) f.DeleteRenderbuffers(1, &depth_rb_);
    fbo_ = color_rb_ = depth_rb_ = 0;
    fbo_w_ = fbo_h_ = 0;
}

void GlHost::EnsureSize(unsigned width, unsigned height) {
    if (!context_ || !fbo_ || (width <= fbo_w_ && height <= fbo_h_)) return;
    auto& f = *fn_;
    const unsigned w = std::max(width, fbo_w_), h = std::max(height, fbo_h_);
    GLint prev_rb = 0;
    f.GetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    f.BindRenderbuffer(GL_RENDERBUFFER, color_rb_);
    f.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    if (depth_rb_) {
        f.BindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
        f.RenderbufferStorage(GL_RENDERBUFFER, want_stencil_ ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                              w, h);
    }
    f.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prev_rb));
    fbo_w_ = w;
    fbo_h_ = h;
    ONYX_INFO("OpenGL output framebuffer grown to %ux%u", w, h);
}

void GlHost::DestroyContext() {
    if (!context_) return;
    auto& f = *fn_;
    DestroyTargets();
    f.MakeCurrent(nullptr, nullptr);
    f.DeleteContext(context_);
    context_ = nullptr;
    dc_ = nullptr;
    if (s_active == this) s_active = nullptr;
}

void GlHost::OnFrame(unsigned width, unsigned height) {
    if (!context_ || !fbo_ || width == 0 || height == 0) return;
    auto& f = *fn_;
    ++frames_;
    if (width > fbo_w_ || height > fbo_h_) {
        // The core drew past the framebuffer this frame; make room for the next.
        EnsureSize(width, height);
        return;
    }

    Slot& slot = slots_[next_serial_ % kSlots];
    if (slot.fence) {
        // Three frames old and still not finished: the GPU is far behind. Wait,
        // since the buffer is about to be reused (its picture is stale anyway).
        const auto t0 = std::chrono::steady_clock::now();
        f.ClientWaitSync(slot.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        f.DeleteSync(slot.fence);
        slot.fence = nullptr;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static int s_logged = 0;
        if (ms > 20.0 && s_logged < 40) {
            ++s_logged;
            ONYX_WARN("OpenGL: waited %.0f ms for a frame three frames old", ms);
        }
    }

    // Keep the core's bindings intact: it tracks its own GL state.
    GLint prev_read_fbo = 0, prev_pack_buf = 0, prev_align = 4, prev_row = 0;
    f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack_buf);
    f.GetIntegerv(GL_PACK_ALIGNMENT, &prev_align);
    f.GetIntegerv(GL_PACK_ROW_LENGTH, &prev_row);

    const size_t bytes = static_cast<size_t>(width) * height * 4;
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    if (slot.capacity < bytes) {
        f.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
        slot.capacity = bytes;
    }
    f.PixelStorei(GL_PACK_ALIGNMENT, 4);
    f.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    // Into the buffer object: returns at once, the copy runs on the GPU.
    f.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    slot.fence = f.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot.width = width;
    slot.height = height;
    slot.serial = next_serial_++;
    f.Flush();

    f.PixelStorei(GL_PACK_ALIGNMENT, prev_align);
    f.PixelStorei(GL_PACK_ROW_LENGTH, prev_row);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pack_buf));
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));

    DeliverReady(false);

    if (frames_ == 1) {
        const GLenum err = f.GetError();
        if (err != GL_NO_ERROR) ONYX_WARN("OpenGL error 0x%04X after the first frame read back", err);
    }
}

void GlHost::DeliverReady(bool /*wait_for_newest*/) {
    auto& f = *fn_;
    // Fences finish in order, so the newest finished frame is the one to show;
    // anything older is dropped unseen.
    int order[kSlots];
    int n = 0;
    for (int i = 0; i < kSlots; ++i)
        if (slots_[i].fence && slots_[i].serial > shown_serial_) order[n++] = i;
    std::sort(order, order + n,
              [&](int a, int b) { return slots_[a].serial > slots_[b].serial; });
    for (int k = 0; k < n; ++k) {
        Slot& s = slots_[order[k]];
        const GLenum r = f.ClientWaitSync(s.fence, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) continue;

        GLint prev_pack_buf = 0;
        f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack_buf);
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
        const size_t bytes = static_cast<size_t>(s.width) * s.height * 4;
        if (const void* px = f.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                              static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT)) {
            static uint64_t s_shown = 0;
            ++s_shown;
            if (s_shown == 1 || s_shown == 60 || s_shown % 1800 == 0) {
                // How much of the picture is lit, on a sparse grid: tells a black
                // frame from the emulator apart from a display problem.
                const auto* p = static_cast<const uint32_t*>(px);
                uint64_t lit = 0, total = 0;
                for (unsigned y = 0; y < s.height; y += 16)
                    for (unsigned x = 0; x < s.width; x += 16, ++total)
                        if (p[static_cast<size_t>(y) * s.width + x] & 0x00F0F0F0u) ++lit;
                ONYX_INFO("OpenGL frame %llu shown (%ux%u), %.1f%% lit",
                          static_cast<unsigned long long>(s_shown), s.width, s.height,
                          total ? 100.0 * lit / total : 0.0);
            }
            presenter_.PushCpuFrame(px, s.width, s.height, static_cast<size_t>(s.width) * 4,
                                    /*rgba=*/true, /*flip_y=*/true);
            f.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pack_buf));
        shown_serial_ = s.serial;
        // This one and every older one are done.
        for (auto& o : slots_) {
            if (o.fence && o.serial <= shown_serial_) {
                f.DeleteSync(o.fence);
                o.fence = nullptr;
            }
        }
        break;
    }
}

} // namespace onyx::app
