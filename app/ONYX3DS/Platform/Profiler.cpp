// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Platform/Log.h"

namespace onyx::app {
namespace {

using SuspendFn = DWORD(WINAPI*)(HANDLE);
using GetCtxFn = BOOL(WINAPI*)(HANDLE, LPCONTEXT);

struct Sampler {
    std::thread thread;
    std::atomic<bool> stop{false};
    HANDLE target = nullptr;
};
Sampler g_sampler;

void SampleLoop(HANDLE target) {
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
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(exe), &mbi, sizeof(mbi));
    std::uintptr_t exe_end = exe;
    for (auto p = exe;;) { // the image is a run of regions with one AllocationBase
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) ||
            reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != exe)
            break;
        p += mbi.RegionSize;
        exe_end = p;
    }

    std::unordered_map<std::uint64_t, int> hot; // exe offset (16-byte bucket) -> samples
    std::unordered_map<std::uintptr_t, int> other; // other module/region base -> samples
    int total = 0, in_exe = 0, in_private = 0, failed = 0;
    auto last_report = std::chrono::steady_clock::now();

    while (!g_sampler.stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (suspend(target) == static_cast<DWORD>(-1)) { ++failed; continue; }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        const BOOL ok = get_ctx(target, &ctx);
        const auto rip = ok ? static_cast<std::uintptr_t>(ctx.Rip) : 0;
        resume(target);
        if (!ok) { ++failed; continue; }
        ++total;
        if (rip >= exe && rip < exe_end) {
            ++in_exe;
            ++hot[(rip - exe) >> 4];
        } else {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQuery(reinterpret_cast<void*>(rip), &m, sizeof(m)) && m.Type == MEM_PRIVATE) {
                ++in_private; // JIT-generated guest code
            } else {
                ++other[reinterpret_cast<std::uintptr_t>(m.AllocationBase)];
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(10) && total > 0) {
            last_report = now;
            const int others = total - in_exe - in_private;
            ONYX_INFO("Profile: %d samples: %.0f%% emulator code, %.0f%% generated guest code, %.0f%% other modules (%d failed)",
                      total, 100.0 * in_exe / total, 100.0 * in_private / total,
                      100.0 * others / total, failed);
            std::vector<std::pair<std::uint64_t, int>> top(hot.begin(), hot.end());
            std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < top.size() && i < 40; ++i)
                ONYX_INFO("Profile:   ONYX3DS.exe+0x%llX  %.1f%%",
                          static_cast<unsigned long long>(top[i].first << 4),
                          100.0 * top[i].second / total);
            for (auto& [base, n] : other) {
                if (100.0 * n / total >= 1.0)
                    ONYX_INFO("Profile:   other module/region at %p  %.1f%%",
                              reinterpret_cast<void*>(base), 100.0 * n / total);
            }
            hot.clear();
            other.clear();
            total = in_exe = in_private = failed = 0;
        }
    }
}

} // namespace

void ProfilerStart() {
    if (g_sampler.thread.joinable()) return;
    HANDLE real = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
        ONYX_WARN("Profiler: could not open the emulation thread");
        return;
    }
    g_sampler.target = real;
    g_sampler.stop = false;
    g_sampler.thread = std::thread(SampleLoop, real);
}

void ProfilerStop() {
    if (!g_sampler.thread.joinable()) return;
    g_sampler.stop = true;
    g_sampler.thread.join();
    CloseHandle(g_sampler.target);
    g_sampler.target = nullptr;
}

} // namespace onyx::app
