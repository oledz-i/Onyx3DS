// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include <mutex>
#include "Platform/SaveBackup.h"

#include "Platform/Log.h"
#include "Platform/UwpPlatform.h"
#include "Ui/AppServices.h"

namespace onyx::app {
namespace {

std::mutex g_sync; // restore and backup never overlap each other or a game start


struct Item { const char* rel_local; const char* backup_name; };
// Relative to LocalState.
const Item kItems[] = {{"system/Azahar/sdmc", "sdmc"}, {"system/Azahar/nand", "nand"},
                       {"states", "states"}, {"nes", "nes"}}; // nes: battery saves (.srm)

std::string Parent(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
    while (!p.empty() && p.back() == '/') p.pop_back();
    auto i = p.find_last_of('/');
    return i == std::string::npos ? std::string() : p.substr(0, i);
}

// Copies new/changed files from `from` to `to`; returns files copied.
int Mirror(UwpFileSystem& fs, const std::string& from, const std::string& to) {
    int n = 0;
    if (!fs.IsDirectory(from)) return 0;
    fs.CreateDirs(to);
    for (const auto& e : fs.List(from)) {
        const std::string a = JoinPath(from, e.name), b = JoinPath(to, e.name);
        if (e.is_dir) { n += Mirror(fs, a, b); continue; }
        // Saves and states are always refreshed (no mtime to compare); installed titles
        // are big and never change, so those are copied only when the size differs.
        if (a.find("/title/") != std::string::npos || a.find("\\title\\") != std::string::npos) {
            if (auto sz = fs.Size(b); sz && *sz == e.size) continue;
        }
        if (fs.Copy(a, b)) ++n;
    }
    return n;
}

bool HasFiles(UwpFileSystem& fs, const std::string& dir) {
    if (!fs.IsDirectory(dir)) return false;
    for (const auto& e : fs.List(dir)) {
        if (!e.is_dir || HasFiles(fs, JoinPath(dir, e.name))) return true;
    }
    return false;
}

} // namespace

std::string SaveBackupDir(const FolderConfig& folders) {
    if (!folders.saves.empty() || folders.roms.empty()) return {};
    const std::string root = Parent(folders.roms.front());
    return root.empty() ? std::string() : JoinPath(root, "SaveBackup");
}

void BackupSaves(const FolderConfig& folders) {
    const std::string dest = SaveBackupDir(folders);
    if (dest.empty()) return;
    std::lock_guard lock(g_sync);
    auto& fs = AppServices::Get().Fs();
    int n = 0;
    for (const auto& it : kItems)
        n += Mirror(fs, JoinPath(Paths().local_state, it.rel_local), JoinPath(dest, it.backup_name));
    ONYX_INFO("Save backup: %d files refreshed in %s", n, dest.c_str());
}

void RestoreSavesIfFresh(const FolderConfig& folders) {
    const std::string src = SaveBackupDir(folders);
    if (src.empty()) return;
    std::lock_guard lock(g_sync);
    auto& fs = AppServices::Get().Fs();
    int n = 0;
    for (const auto& it : kItems) {
        const std::string local = JoinPath(Paths().local_state, it.rel_local);
        const std::string backup = JoinPath(src, it.backup_name);
        if (HasFiles(fs, local) || !HasFiles(fs, backup)) continue;
        n += Mirror(fs, backup, local);
    }
    if (n) ONYX_INFO("Restored %d save files from %s", n, src.c_str());
}

} // namespace onyx::app

namespace onyx::app {
void WaitForSaveSync() { std::lock_guard lock(g_sync); }
} // namespace onyx::app
