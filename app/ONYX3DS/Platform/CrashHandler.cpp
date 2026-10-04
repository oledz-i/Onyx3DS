// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/CrashHandler.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include "Platform/Log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace onyx::app {
namespace {

thread_local bool t_in_handler = false; // a fault while reporting must not recurse

bool IsExecutable(DWORD protect) {
    return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY)) != 0;
}

// The DLL name stored in a loaded image's export directory ("vulkan_dzn.dll").
// Reads the mapped headers only, so no loader APIs are needed.
const char* ImageName(const BYTE* base) {
    if (!base) return "?";
    if (base == reinterpret_cast<const BYTE*>(&__ImageBase)) return "ONYX3DS.exe";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "?";
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return "?";
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0) return "(unnamed module)";
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    return reinterpret_cast<const char*>(base + exp->Name);
}

// "vulkan_dzn.dll+0x1234", or what kind of memory the address is in.
void Describe(const void* addr, char* out, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!addr || VirtualQuery(addr, &mbi, sizeof(mbi)) == 0 || mbi.State == MEM_FREE) {
        std::snprintf(out, n, "%p (unmapped)", addr);
        return;
    }
    if (mbi.Type == MEM_IMAGE) {
        const auto* base = static_cast<const BYTE*>(mbi.AllocationBase);
        std::snprintf(out, n, "%s+0x%llx", ImageName(base),
                      static_cast<unsigned long long>(static_cast<const BYTE*>(addr) - base));
        return;
    }
    const char* kind = mbi.Type == MEM_MAPPED ? "mapped memory" : "private memory";
    if (mbi.Type == MEM_PRIVATE && IsExecutable(mbi.Protect)) kind = "private executable memory (JIT code)";
    std::snprintf(out, n, "%p (%s, protect 0x%lx)", addr, kind, mbi.Protect);
}

const char* CodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_BREAKPOINT: return "breakpoint (failed ASSERT in Azahar?)";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

// Decorated type name of a thrown MSVC C++ exception (".?AVError@Xbyak@@"),
// read from the throw info the compiler emits. x64 layout: ThrowInfo has the
// catchable-type array RVA at +12, the array holds RVAs to CatchableTypes,
// and a CatchableType has its TypeDescriptor RVA at +4 (name at +16).
const char* CppTypeName(const EXCEPTION_RECORD* rec) {
#if defined(_M_X64)
    if (rec->NumberParameters < 4) return "?";
    const auto* throw_info = reinterpret_cast<const BYTE*>(rec->ExceptionInformation[2]);
    const auto* image = reinterpret_cast<const BYTE*>(rec->ExceptionInformation[3]);
    if (!throw_info || !image) return "?";
    const int array_rva = *reinterpret_cast<const int*>(throw_info + 12);
    if (!array_rva) return "?";
    const BYTE* array = image + array_rva;
    if (*reinterpret_cast<const int*>(array) < 1) return "?";
    const BYTE* catchable = image + *reinterpret_cast<const int*>(array + 4);
    const BYTE* type_desc = image + *reinterpret_cast<const int*>(catchable + 4);
    return reinterpret_cast<const char*>(type_desc + 16);
#else
    (void)rec;
    return "?";
#endif
}

void Report(const char* stage, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    const DWORD code = rec->ExceptionCode;
    char where[256];
    char line[768];
    Describe(rec->ExceptionAddress, where, sizeof(where));
    std::snprintf(line, sizeof(line), "%s: %s (0x%08lX) at %s, thread %lu", stage, CodeName(code),
                  code, where, GetCurrentThreadId());
    LogRaw(line);

    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        Describe(reinterpret_cast<const void*>(rec->ExceptionInformation[1]), where, sizeof(where));
        std::snprintf(line, sizeof(line), "  tried to %s %s",
                      kind == 0 ? "read" : kind == 1 ? "write" : "execute", where);
        LogRaw(line);
    } else if (code == 0xE06D7363) {
        std::snprintf(line, sizeof(line), "  type %s", CppTypeName(rec));
        LogRaw(line);
    }

#if defined(_M_X64)
    const CONTEXT* ctx = ep->ContextRecord;
    std::snprintf(line, sizeof(line),
                  "  rip=%016llx rsp=%016llx rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx",
                  ctx->Rip, ctx->Rsp, ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    LogRaw(line);
    if (code == EXCEPTION_STACK_OVERFLOW) return; // no stack left to scan with

    // Stack scan: every value on the stack that points into executable code.
    // Not a precise unwind, but the real call chain is in here, newest first.
    const auto* sp = reinterpret_cast<const ULONG_PTR*>(ctx->Rsp);
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto* top = static_cast<const ULONG_PTR*>(tib->StackBase);
    int found = 0;
    for (const ULONG_PTR* p = sp; p && p < top && p < sp + 4096 && found < 24; ++p) {
        const void* value = reinterpret_cast<const void*>(*p);
        MEMORY_BASIC_INFORMATION mbi{};
        if (!value || VirtualQuery(value, &mbi, sizeof(mbi)) == 0) continue;
        if (mbi.State != MEM_COMMIT || !IsExecutable(mbi.Protect)) continue;
        Describe(value, where, sizeof(where));
        std::snprintf(line, sizeof(line), "  stack: %s", where);
        LogRaw(line);
        ++found;
    }
#endif
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep) {
    if (t_in_handler) return EXCEPTION_CONTINUE_SEARCH;
    t_in_handler = true;
    Report("UNHANDLED, the app is closing", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
    std::string what = "no active exception";
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            what = ex.what();
        } catch (...) {
            what = "a non-std exception";
        }
    }
    char line[768];
    std::snprintf(line, sizeof(line), "std::terminate on thread %lu: uncaught %s",
                  GetCurrentThreadId(), what.c_str());
    LogRaw(line);
    std::abort();
}

void OnAbort(int) {
    char line[128];
    std::snprintf(line, sizeof(line), "abort() called on thread %lu", GetCurrentThreadId());
    LogRaw(line);
}

} // namespace

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(&OnUnhandled);
    std::signal(SIGABRT, &OnAbort);
    InstallThreadCrashHooks();
}

void InstallThreadCrashHooks() {
    std::set_terminate(&OnTerminate);
}

void LogMemoryUsage(const char* when) {
    try {
        using winrt::Windows::System::MemoryManager;
        const unsigned long long mb = 1024ull * 1024ull;
        ONYX_INFO("Memory %s: %llu MB used of %llu MB allowed", when,
                  MemoryManager::AppMemoryUsage() / mb, MemoryManager::AppMemoryUsageLimit() / mb);
    } catch (...) {
        ONYX_WARN("Memory %s: usage not available", when);
    }
}

void WatchMemoryFor(int seconds) {
    std::thread([seconds] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (int i = 0; i < seconds * 2; ++i) {
            Sleep(500);
            LogMemoryUsage("(watch)");
        }
    }).detach();
}

} // namespace onyx::app
