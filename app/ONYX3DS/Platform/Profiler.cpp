// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = get_ctx(t.handle, &ctx);
            const auto rip = ok ? static_cast<std::uintptr_t>(ctx.Rip) : 0;
            std::uintptr_t stack[kStackWords];
            bool have_stack = false;
            if (ok && (rip < exe || rip >= exe_end))
                have_stack = CopyStack(reinterpret_cast<const void*>(ctx.Rsp), stack, kStackWords);
            resume(t.handle);
            if (!ok) { ++failed; continue; }
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
                            if (stack[i] >= exe && stack[i] < exe_end) {
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
