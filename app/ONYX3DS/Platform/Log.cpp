// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Log.h"

#include <cstdarg>
#include <cstdio>

namespace onyx::app {
namespace {
std::mutex g_mutex;
std::deque<std::string> g_ring;
HANDLE g_file = INVALID_HANDLE_VALUE;
constexpr std::size_t kRingSize = 1000;
} // namespace

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::string Utf8(winrt::hstring const& h) {
    return Utf8(std::wstring(h.c_str(), h.size()));
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void LogInit(const std::wstring& log_file) {
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) return;
    CREATEFILE2_EXTENDED_PARAMETERS p{sizeof(p)};
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    // Keep only the latest session: the previous one is renamed to onyx.prev.log.
    const std::wstring prev = log_file.substr(0, log_file.find_last_of(L'.')) + L".prev.log";
    DeleteFileFromAppW(prev.c_str());
    MoveFileFromAppW(log_file.c_str(), prev.c_str());
    g_file = CreateFile2FromAppW(log_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, &p);
}

void Log(LogLevel level, const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    static const char* names[] = {"debug", "info", "warn", "ERROR"};
    char line[2200];
    const int n = std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour,
                                t.wMinute, t.wSecond, t.wMilliseconds,
                                names[static_cast<int>(level)], msg);
    OutputDebugStringA(line);

    std::lock_guard lock(g_mutex);
    g_ring.emplace_back(line, line + std::max(0, n - 1));
    if (g_ring.size() > kRingSize) g_ring.pop_front();
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(std::max(0, n)), &written, nullptr);
        if (level >= LogLevel::Warning) FlushFileBuffers(g_file);
    }
}

std::vector<std::string> RecentLog(std::size_t max_lines) {
    std::lock_guard lock(g_mutex);
    const std::size_t start = g_ring.size() > max_lines ? g_ring.size() - max_lines : 0;
    return {g_ring.begin() + static_cast<std::ptrdiff_t>(start), g_ring.end()};
}

} // namespace onyx::app
