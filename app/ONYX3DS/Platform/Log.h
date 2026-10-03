// SPDX-License-Identifier: GPL-3.0-or-later
//
// App log: kept in a ring buffer for the System Check page and appended to
// LocalState/onyx.log so a crash on the console can be read afterwards from
// the Xbox Device Portal file explorer.
#pragma once

#include <string>
#include <vector>

namespace onyx::app {

enum class LogLevel { Debug, Info, Warning, Error };

void LogInit(const std::wstring& log_file);
void Log(LogLevel level, const char* fmt, ...);
std::vector<std::string> RecentLog(std::size_t max_lines = 200);

#define ONYX_INFO(...) ::onyx::app::Log(::onyx::app::LogLevel::Info, __VA_ARGS__)
#define ONYX_WARN(...) ::onyx::app::Log(::onyx::app::LogLevel::Warning, __VA_ARGS__)
#define ONYX_ERROR(...) ::onyx::app::Log(::onyx::app::LogLevel::Error, __VA_ARGS__)
#define ONYX_DEBUG(...) ::onyx::app::Log(::onyx::app::LogLevel::Debug, __VA_ARGS__)

std::string Utf8(const std::wstring& w);
std::string Utf8(winrt::hstring const& h);
std::wstring Wide(const std::string& s);

} // namespace onyx::app
