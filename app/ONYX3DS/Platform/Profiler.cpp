// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
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
    int group; // 0 = emulation thread, 1 = software renderer workers
};

struct Sampler {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::vector<Target> targets;
};
Sampler g_sampler;

void OnThreadNamed(const char* name) {
    if (!name || std::strcmp(name, "SwRenderer workers") != 0) return;
    HANDLE real = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0))
        return;
    std::lock_guard lock(g_sampler.mutex);
    g_sampler.targets.push_back({real, 1});
}

struct Group {
    std::unordered_map<std::uint64_t, int> hot; // exe offset (16-byte bucket) -> samples
    int total = 0, in_exe = 0, in_private = 0;
    void Clear() { hot.clear(); total = in_exe = in_private = 0; }
};

void Report(const char* label, Group& g, int failed) {
    if (g.total == 0) return;
    ONYX_INFO("Profile %s: %d samples: %.0f%% emulator code, %.0f%% generated guest code, %.0f%% waiting/system (%d failed)",
              label, g.total, 100.0 * g.in_exe / g.total, 100.0 * g.in_private / g.total,
              100.0 * (g.total - g.in_exe - g.in_private) / g.total, failed);
    std::vector<std::pair<std::uint64_t, int>> top(g.hot.begin(), g.hot.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < top.size() && i < 40; ++i)
        ONYX_INFO("Profile %s:   +0x%llX  %.1f%%", label,
                  static_cast<unsigned long long>(top[i].first << 4),
                  100.0 * top[i].second / g.total);
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

    Group groups[2];
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
            resume(t.handle);
            if (!ok) { ++failed; continue; }
            Group& g = groups[t.group];
            ++g.total;
            if (rip >= exe && rip < exe_end) {
                ++g.in_exe;
                ++g.hot[(rip - exe) >> 4];
            } else {
                MEMORY_BASIC_INFORMATION m{};
                if (VirtualQuery(reinterpret_cast<void*>(rip), &m, sizeof(m)) && m.Type == MEM_PRIVATE)
                    ++g.in_private; // JIT-generated guest code
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(10)) {
            last_report = now;
            Report("main", groups[0], failed);
            Report("workers", groups[1], failed);
            groups[0].Clear();
            groups[1].Clear();
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
