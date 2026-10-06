// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Platform/Log.h"

// Defined by the core (patches/azahar/0013): called with every thread name.
namespace Common {
extern void (*g_thread_name_hook)(const char*);
}

namespace onyx::app {
namespace {

using SuspendFn = DWORD(WINAPI*)(HANDLE);
using GetCtxFn = BOOL(WINAPI*)(HANDLE, LPCONTEXT);

struct Target {
    HANDLE handle;
    int group; // 0 = emulation thread, 1 = software renderer workers, 2 = Vulkan worker
};

struct Sampler {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::vector<Target> targets;
};
Sampler g_sampler;

// steady_clock nanoseconds when the current retro_run started; 0 between frames.
std::atomic<long long> g_frame_begin_ns{0};
constexpr long long kStallNs = 100'000'000; // frames over 100 ms count as stalls

long long NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

using LookupFn = PRUNTIME_FUNCTION(NTAPI*)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
using UnwindFn = PEXCEPTION_ROUTINE(NTAPI*)(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT,
                                            PVOID*, PDWORD64, PKNONVOLATILE_CONTEXT_POINTERS);

// Walks a thread's stack from a captured context with the x64 unwind tables.
// The thread runs again by then; it is (almost always) still blocked in the
// call that stalls, and every read is guarded, so the worst case is a short or
// stale stack.
int UnwindStack(LookupFn lookup, UnwindFn unwind, const CONTEXT* start, std::uintptr_t* frames,
                int max) {
    int n = 0;
    __try {
        CONTEXT ctx = *start;
        while (n < max && ctx.Rip) {
            frames[n++] = static_cast<std::uintptr_t>(ctx.Rip);
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = lookup(ctx.Rip, &base, nullptr);
            const DWORD64 old_sp = ctx.Rsp;
            if (!fn) { // leaf function or generated code: return address on top
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handler_data = nullptr;
                DWORD64 frame = 0;
                unwind(0 /* UNW_FLAG_NHANDLER */, base, ctx.Rip, fn, &ctx, &handler_data, &frame,
                       nullptr);
            }
            if (ctx.Rsp <= old_sp) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// Does the code just before this address look like a call? Filters stale
// values out of the raw stack scan below.
bool LooksLikeReturnAddress(std::uintptr_t a) {
    __try {
        const auto* b = reinterpret_cast<const std::uint8_t*>(a);
        if (b[-5] == 0xE8) return true;                                 // call rel32
        if (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10) return true;       // call [rip+disp32] etc.
        if (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) return true;       // call reg / [reg]
        if (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) return true;       // call [reg+disp8]
        if (b[-7] == 0xFF && (b[-6] & 0x38) == 0x10) return true;       // call [reg+disp32]
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnThreadNamed(const char* name) {
    if (!name) return;
    int group;
    if (std::strcmp(name, "SwRenderer workers") == 0) group = 1;
    else if (std::strcmp(name, "VulkanWorker") == 0) group = 2; // hardware renderer recording
    else return;
    HANDLE real = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0))
        return;
    std::lock_guard lock(g_sampler.mutex);
    g_sampler.targets.push_back({real, group});
}

// Copies a few words from the (suspended) target thread's stack. Guarded: near the
// top of a stack the read can run off the end.
bool CopyStack(const void* sp, std::uintptr_t* out, int n) {
    __try {
        const auto* p = static_cast<const std::uintptr_t*>(sp);
        for (int i = 0; i < n; ++i) out[i] = p[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr int kStackWords = 96;

struct Group {
    std::unordered_map<std::uint64_t, int> hot; // exe offset (16-byte bucket) -> samples
    std::unordered_map<std::uint64_t, int> callers; // exe offset of the caller of system code
    std::unordered_map<std::string, int> modules;   // system module -> samples
    int total = 0, in_exe = 0, in_private = 0;
    void Clear() { hot.clear(); callers.clear(); modules.clear(); total = in_exe = in_private = 0; }
};

struct StallStacks {
    std::map<std::string, int> stacks; // "frame, frame, ..." -> samples
    int samples = 0;
    void Clear() { stacks.clear(); samples = 0; }
};

void ReportStalls(StallStacks& st) {
    if (st.samples == 0) return;
    ONYX_INFO("Profile stall: %d samples taken while a frame had run over 100 ms "
              "(exe frames are +0x offsets; decode with the map)", st.samples);
    std::vector<std::pair<std::string, int>> top(st.stacks.begin(), st.stacks.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < top.size() && i < 6; ++i)
        ONYX_INFO("Profile stall stack %zu (%.0f%%): %s", i + 1,
                  100.0 * top[i].second / st.samples, top[i].first.c_str());
}

void Report(const char* label, Group& g, int failed) {
    if (g.total == 0) return;
    ONYX_INFO("Profile %s: %d samples: %.0f%% emulator code, %.0f%% generated guest code, %.0f%% waiting/system (%d failed)",
              label, g.total, 100.0 * g.in_exe / g.total, 100.0 * g.in_private / g.total,
              100.0 * (g.total - g.in_exe - g.in_private) / g.total, failed);
    std::vector<std::pair<std::uint64_t, int>> top(g.hot.begin(), g.hot.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    // Up to 200 hot spots, 20 per line ("offset:percent"), so whole functions can be
    // added up against the map file.
    std::string line;
    int on_line = 0;
    for (size_t i = 0; i < top.size() && i < 200; ++i) {
        const double pct = 100.0 * top[i].second / g.total;
        if (pct < 0.05) break;
        char item[48];
        std::snprintf(item, sizeof(item), " %llX:%.2f",
                      static_cast<unsigned long long>(top[i].first << 4), pct);
        line += item;
        if (++on_line == 20) {
            ONYX_INFO("Profile %s hot:%s", label, line.c_str());
            line.clear();
            on_line = 0;
        }
    }
    if (!line.empty()) ONYX_INFO("Profile %s hot:%s", label, line.c_str());
    for (auto& [name, n] : g.modules)
        if (100.0 * n / g.total >= 1.0)
            ONYX_INFO("Profile %s: system code in %s  %.1f%%", label, name.c_str(),
                      100.0 * n / g.total);
    std::vector<std::pair<std::uint64_t, int>> cal(g.callers.begin(), g.callers.end());
    std::sort(cal.begin(), cal.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < cal.size() && i < 25; ++i)
        ONYX_INFO("Profile %s:   system code called from +0x%llX  %.1f%%", label,
                  static_cast<unsigned long long>(cal[i].first << 4),
                  100.0 * cal[i].second / g.total);
}

void SampleLoop() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = GetModuleHandleW(L"kernel32.dll");
    auto suspend = kb ? reinterpret_cast<SuspendFn>(GetProcAddress(kb, "SuspendThread")) : nullptr;
    auto resume = kb ? reinterpret_cast<SuspendFn>(GetProcAddress(kb, "ResumeThread")) : nullptr;
    auto get_ctx = kb ? reinterpret_cast<GetCtxFn>(GetProcAddress(kb, "GetThreadContext")) : nullptr;
    if (!suspend || !resume || !get_ctx) {
        ONYX_WARN("Profiler: thread sampling is not available on this system");
        return;
    }
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t exe_end = exe;
    for (auto p = exe;;) { // the image is a run of regions with one AllocationBase
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) ||
            reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != exe)
            break;
        p += mbi.RegionSize;
        exe_end = p;
    }

    // Names for the system modules that show up in samples.
    std::vector<std::pair<std::uintptr_t, std::string>> known;
    for (const wchar_t* m : {L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"win32u.dll",
                             L"ucrtbase.dll", L"vcruntime140_app.dll", L"vcruntime140_1_app.dll",
                             L"msvcp140_app.dll", L"vulkan_dzn.dll", L"d3d12.dll", L"dxgi.dll",
                             L"combase.dll", L"dxil.dll"}) {
        if (HMODULE h = GetModuleHandleW(m))
            known.emplace_back(reinterpret_cast<std::uintptr_t>(h), Utf8(std::wstring(m)));
    }
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto lookup = nt ? reinterpret_cast<LookupFn>(GetProcAddress(nt, "RtlLookupFunctionEntry")) : nullptr;
    auto unwind = nt ? reinterpret_cast<UnwindFn>(GetProcAddress(nt, "RtlVirtualUnwind")) : nullptr;
    auto module_name = [&](std::uintptr_t addr) -> std::string {
        if (addr >= exe && addr < exe_end) {
            char b[32];
            std::snprintf(b, sizeof(b), "+0x%llX", static_cast<unsigned long long>(addr - exe));
            return b;
        }
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &m, sizeof(m))) return "?";
        if (m.Type == MEM_PRIVATE) return "jit";
        const auto base = reinterpret_cast<std::uintptr_t>(m.AllocationBase);
        std::string mod;
        for (auto& [b, n] : known)
            if (b == base) { mod = n; break; }
        if (mod.empty()) {
            wchar_t path[MAX_PATH];
            const DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                std::wstring w(path, len);
                const size_t slash = w.find_last_of(L"\\/");
                mod = Utf8(slash == std::wstring::npos ? w : w.substr(slash + 1));
            } else {
                char b[32];
                std::snprintf(b, sizeof(b), "mod@%llX", static_cast<unsigned long long>(base));
                mod = b;
            }
            known.emplace_back(base, mod); // name it once
        }
        char off[32];
        std::snprintf(off, sizeof(off), "+0x%llX", static_cast<unsigned long long>(addr - base));
        return mod + off;
    };
    StallStacks stalls;
    Group groups[3];
    int failed = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (!g_sampler.stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::vector<Target> targets;
        {
            std::lock_guard lock(g_sampler.mutex);
            targets = g_sampler.targets;
        }
        for (const Target& t : targets) {
            if (suspend(t.handle) == static_cast<DWORD>(-1)) { ++failed; continue; }
            // A frame that has run for over 100 ms: take the whole context for a stack walk.
            const long long begin = t.group == 0 ? g_frame_begin_ns.load() : 0;
            const bool stalled = begin != 0 && lookup && unwind && NowNs() - begin > kStallNs;
            CONTEXT ctx{};
            ctx.ContextFlags = stalled ? CONTEXT_FULL : CONTEXT_CONTROL;
            const BOOL ok = get_ctx(t.handle, &ctx);
            const auto rip = ok ? static_cast<std::uintptr_t>(ctx.Rip) : 0;
            std::uintptr_t stack[kStackWords];
            bool have_stack = false;
            if (ok && (rip < exe || rip >= exe_end))
                have_stack = CopyStack(reinterpret_cast<const void*>(ctx.Rsp), stack, kStackWords);
            resume(t.handle);
            if (!ok) { ++failed; continue; }
            if (stalled) {
                std::uintptr_t frames[24];
                const int n = UnwindStack(lookup, unwind, &ctx, frames, 24);
                std::string key;
                for (int i = 0; i < n; ++i) {
                    if (i) key += ", ";
                    key += module_name(frames[i]);
                }
                ++stalls.stacks[key.empty() ? std::string("?") : key];
                ++stalls.samples;
            }
            Group& g = groups[t.group];
            ++g.total;
            if (rip >= exe && rip < exe_end) {
                ++g.in_exe;
                ++g.hot[(rip - exe) >> 4];
            } else {
                MEMORY_BASIC_INFORMATION m{};
                if (VirtualQuery(reinterpret_cast<void*>(rip), &m, sizeof(m)) && m.Type == MEM_PRIVATE) {
                    ++g.in_private; // JIT-generated guest code
                } else {
                    // System or other module code: which module, and who called into it.
                    const auto base = reinterpret_cast<std::uintptr_t>(m.AllocationBase);
                    std::string mod = "other";
                    for (auto& [b, n] : known)
                        if (b == base) { mod = n; break; }
                    if (mod == "other") {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "module@%p", reinterpret_cast<void*>(base));
                        mod = buf;
                    }
                    ++g.modules[mod];
                    if (have_stack) {
                        for (int i = 0; i < kStackWords; ++i) {
                            if (stack[i] >= exe && stack[i] < exe_end &&
                                LooksLikeReturnAddress(stack[i])) {
                                ++g.callers[(stack[i] - exe) >> 4];
                                break;
                            }
                        }
                    }
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(10)) {
            last_report = now;
            Report("main", groups[0], failed);
            Report("workers", groups[1], failed);
            Report("vkworker", groups[2], failed);
            ReportStalls(stalls);
            stalls.Clear();
            groups[0].Clear();
            groups[1].Clear();
            groups[2].Clear();
            failed = 0;
        }
    }
}

} // namespace

void ProfilerStart() {
    if (g_sampler.thread.joinable()) return;
    {
        // Which build is this? Matches the "Timestamp is ..." line of its map file.
        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        ONYX_INFO("Build stamp: %08x", static_cast<unsigned>(nt->FileHeader.TimeDateStamp));
    }
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
        ONYX_WARN("Profiler: could not open the emulation thread");
        return;
    }
    {
        std::lock_guard lock(g_sampler.mutex);
        g_sampler.targets.clear();
        g_sampler.targets.push_back({self, 0});
    }
    Common::g_thread_name_hook = OnThreadNamed;
    g_sampler.stop = false;
    g_sampler.thread = std::thread(SampleLoop);
}

void ProfilerFrameBegin() { g_frame_begin_ns.store(NowNs()); }
void ProfilerFrameEnd() { g_frame_begin_ns.store(0); }

void ProfilerStop() {
    if (!g_sampler.thread.joinable()) return;
    g_sampler.stop = true;
    g_sampler.thread.join();
    Common::g_thread_name_hook = nullptr;
    std::lock_guard lock(g_sampler.mutex);
    for (auto& t : g_sampler.targets) CloseHandle(t.handle);
    g_sampler.targets.clear();
}

} // namespace onyx::app
