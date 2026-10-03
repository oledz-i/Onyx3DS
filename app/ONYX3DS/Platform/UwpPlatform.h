// SPDX-License-Identifier: GPL-3.0-or-later
//
// Xbox implementations of the shell's platform interfaces, plus console
// detection and the app's well-known folders.
#pragma once

#include "onyx/platform.h"
#include "onyx/settings.h"

namespace onyx::app {

// Every call uses the *FromApp Win32 APIs, which work for LocalState, the
// install folder and anything reachable through broadFileSystemAccess /
// removable storage (E:\ on the console when a USB drive is plugged in).
class UwpFileSystem final : public IFileSystem {
public:
    bool Exists(const std::string& path) override;
    bool IsDirectory(const std::string& path) override;
    std::vector<DirEntry> List(const std::string& dir) override;
    Bytes ReadRange(const std::string& path, u64 offset, std::size_t length) override;
    std::optional<u64> Size(const std::string& path) override;
    bool WriteAll(const std::string& path, std::span<const u8> data) override;
    bool CreateDirs(const std::string& path) override;
    bool Remove(const std::string& path) override;
    bool Copy(const std::string& from, const std::string& to) override;
};

// Windows.Web.Http. Blocking; call from worker threads only.
class UwpHttpClient final : public IHttpClient {
public:
    UwpHttpClient();
    HttpResponse Send(const HttpRequest& request) override;

private:
    winrt::Windows::Web::Http::HttpClient client_{nullptr};
};

struct AppPaths {
    std::string local_state;   // LocalState (always writable, backed up nowhere)
    std::string install;       // package install folder (read-only)
    std::string azahar_root;   // libretro "system/save" dir handed to the core
    std::string cache;         // library.json, icons, art, cheat index
    std::string art;           // SteamGridDB downloads
    std::string builtin_themes;
    std::string settings_file;
    std::string log_file;
};

const AppPaths& Paths();
ConsoleModel DetectConsole();
std::string ConsoleDescription(); // "Xbox Series S (Dev Mode, Game resources)"

// Lists drives that look like USB storage (D:..Z: with a root that exists),
// for the folder browser and "Set up this USB drive".
std::vector<std::string> RemovableDriveRoots();

// Runs `work` on the thread pool.
void RunAsync(std::function<void()> work);
// Runs `work` on the UI thread.
void RunOnUi(std::function<void()> work);

u64 NowUnix();

} // namespace onyx::app
