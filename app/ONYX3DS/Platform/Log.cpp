// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Log.h"
#include <sys/stat.h>
#include <share.h>
#include <io.h>
#include <fcntl.h>

#include <cstdarg>
#include <cstdio>

namespace onyx::app {
namespace {
void MesaLogHook(int level, const char* msg);
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

void LogRaw(const char* text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1024];
    const int n = std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [CRASH] %s\n", t.wHour,
                                t.wMinute, t.wSecond, t.wMilliseconds, text);
    if (n <= 0) return;
    const DWORD len = static_cast<DWORD>(std::min<int>(n, sizeof(line) - 1));
    OutputDebugStringA(line);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, len, &written, nullptr);
        FlushFileBuffers(g_file);
    }
}

namespace {
std::wstring g_stderr_path;
long long g_stderr_read = 0;
} // namespace

namespace {
// Receives every Mesa log message (mesa_log_level: 0 error, 1 warning, 2 info, 3 debug)
// from vulkan_dzn.dll; installed by InstallDriverLogHook. May run on any thread.
void MesaLogHook(int level, const char* msg) {
    std::string text(msg ? msg : "");
    // Multi-line messages (e.g. DXIL validation errors) become one log line each.
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            const LogLevel lvl = level <= 0   ? LogLevel::Error
                                 : level == 1 ? LogLevel::Warning
                                              : LogLevel::Info;
            Log(lvl, "[driver] %s", line.c_str());
        }
        start = end + 1;
    }
}
} // namespace

bool InstallDriverLogHook(HMODULE dzn) {
    using SetHookFn = void(__cdecl*)(void (*)(int, const char*));
    const auto set_hook =
        dzn ? reinterpret_cast<SetHookFn>(GetProcAddress(dzn, "onyx_set_log_hook")) : nullptr;
    if (!set_hook) {
        ONYX_WARN("Driver log hook not available (vulkan_dzn.dll has no onyx_set_log_hook "
                  "export, error %lu); driver errors only reach driver.log",
                  dzn ? GetLastError() : 0ul);
        return false;
    }
    set_hook(&MesaLogHook); // Dozen answers with a "[driver] dzn: log hook installed" line
    ONYX_INFO("Driver log hook installed");
    return true;
}

void CaptureStderr(const std::wstring& path) {
    g_stderr_path = path;
    g_stderr_read = 0;
    FILE* f = nullptr;
    const errno_t e = _wfreopen_s(&f, path.c_str(), L"w", stderr);
    if (e == 0 && f) {
        setvbuf(stderr, nullptr, _IONBF, 0); // unbuffered: survives a crash
        ONYX_INFO("Driver messages go to driver.log");
        return;
    }
    // The CRT could not reopen stderr (no console to replace). Open the file ourselves
    // and put it on file descriptor 2, which is what the driver's stderr writes use.
    int fd = -1;
    const errno_t e2 = _wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                                 _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (e2 == 0 && fd >= 0 && _dup2(fd, 2) == 0) {
        ONYX_INFO("Driver messages go to driver.log (reopen failed with %d, used fd 2)",
                  static_cast<int>(e));
        return;
    }
    ONYX_WARN("Could not capture driver messages (reopen error %d, open error %d)",
              static_cast<int>(e), static_cast<int>(e2));
    g_stderr_path.clear();
}

void DrainStderrToLog() {
    if (g_stderr_path.empty()) return;
    fflush(stderr);
    // Share every access: the CRT keeps driver.log open for writing as stderr.
    CREATEFILE2_EXTENDED_PARAMETERS p{sizeof(p)};
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    const HANDLE f = CreateFile2FromAppW(g_stderr_path.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         OPEN_EXISTING, &p);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart < g_stderr_read)
        g_stderr_read = 0; // truncated or recreated since the last drain
    LARGE_INTEGER offset{};
    offset.QuadPart = g_stderr_read;
    if (SetFilePointerEx(f, offset, nullptr, FILE_BEGIN)) {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n > 0) {
            pending.append(buf, n);
            g_stderr_read += static_cast<long long>(n);
            if (pending.size() > 64 * 1024) break; // a runaway driver log stays readable
        }
        size_t start = 0;
        while (start < pending.size()) {
            size_t end = pending.find('\n', start);
            if (end == std::string::npos) end = pending.size();
            std::string line = pending.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) Log(LogLevel::Warning, "[driver] %s", line.c_str());
            start = end + 1;
        }
    }
    CloseHandle(f);
}

std::vector<std::string> RecentLog(std::size_t max_lines) {
    std::lock_guard lock(g_mutex);
    const std::size_t start = g_ring.size() > max_lines ? g_ring.size() - max_lines : 0;
    return {g_ring.begin() + static_cast<std::ptrdiff_t>(start), g_ring.end()};
}

} // namespace onyx::app
