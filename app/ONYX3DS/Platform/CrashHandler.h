// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native crash reporting. XAML's UnhandledException only sees WinRT errors on
// the UI thread; a native fault on the emulation thread (access violation,
// failed assert, uncaught C++ exception) used to close the app with nothing in
// the log. These hooks write the fault, where it happened and a stack scan
// straight to onyx.log before the process goes away.
#pragma once

namespace onyx::app {

// Process-wide hooks: unhandled exception filter and SIGABRT. (Xbox's SDK has
// no vectored exception handlers, so only faults nothing catches are logged.)
// Call once, right after LogInit.
void InstallCrashHandler();

// Per-thread hooks (std::terminate handler). Call at the top of every thread
// that runs emulator code.
void InstallThreadCrashHooks();

// What RunGuarded caught.
struct GuardedCrash {
    unsigned long code = 0;  // Windows exception code
    bool in_jit = false;     // faulted in, or writing to, JIT-generated code
    char where[256] = {};    // "ONYX3DS.exe+0x1234", "private executable memory", ...
    char what[96] = {};      // "access violation", ...
};

// Runs fn(ctx). If it crashes (access violation, failed assert, uncaught C++
// exception, ...), logs the crash, fills `out` and returns false instead of
// letting the process die. State touched by fn is left as it was at the crash.
bool RunGuarded(void (*fn)(void*), void* ctx, GuardedCrash* out);

// Logs the app's memory use against the limit Xbox gives it. `when` labels the line.
void LogMemoryUsage(const char* when);

// Logs memory use every 500 ms for `seconds`, on a background thread, so an
// out-of-memory kill shows up as usage climbing to the limit right before the log ends.
void WatchMemoryFor(int seconds);

} // namespace onyx::app
