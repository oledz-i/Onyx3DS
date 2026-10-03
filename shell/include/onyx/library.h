// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game library behind the home menu: scans the ROM folders, reads titles
// and icons out of the files, remembers favourites and play time, and keeps a
// JSON cache so a 300-game USB drive does not get re-read on every launch.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "onyx/n3ds_formats.h"
#include "onyx/platform.h"
#include "onyx/settings.h"

namespace onyx {

struct GameEntry {
    std::string path;
    std::string title;          // best SMDH name, or a cleaned-up file name
    std::string long_title;
    std::string publisher;
    std::string region;         // "USA", "EUR", "Region free" ...
    std::string product_code;
    u64 title_id = 0;
    n3ds::TitleKind kind = n3ds::TitleKind::Unknown;
    n3ds::RomFormat format = n3ds::RomFormat::Unknown;
    bool encrypted = false;
    u64 file_size = 0;
    std::string note;           // parser notes, e.g. "encrypted dump"

    // Cached artwork, all inside LocalState. Empty when not downloaded yet.
    std::string icon_path;      // 48x48 RGBA from the SMDH, raw .rgba file
    std::string grid_art;       // SteamGridDB grid (channel tile)
    std::string hero_art;       // SteamGridDB hero (details banner)
    std::string logo_art;       // SteamGridDB logo (drawn over the hero)
    std::string user_title;     // rename from the details page

    bool favorite = false;
    bool hidden = false;
    u64 play_seconds = 0;
    u64 last_played = 0;        // unix seconds
    u32 launch_count = 0;

    std::string DisplayTitle() const { return user_title.empty() ? title : user_title; }
    std::string TitleIdHex() const { return TitleIdToHex(title_id); }
    bool IsLaunchable() const {
        return kind == n3ds::TitleKind::Application || kind == n3ds::TitleKind::Demo ||
               kind == n3ds::TitleKind::System || kind == n3ds::TitleKind::Unknown;
    }
};

// Turns "Mario Kart 7 (USA) (En,Fr,Es) [!].3ds" into "Mario Kart 7".
std::string CleanFileTitle(std::string_view file_name);

struct ScanProgress {
    std::size_t files_seen = 0;
    std::size_t files_parsed = 0;
    std::string current;
};

class GameLibrary {
public:
    GameLibrary(IFileSystem& fs, std::string cache_dir);

    // Rebuilds the list from the ROM folders. Entries whose path and size match
    // the cache are reused without opening the file. Safe to call off-thread.
    void Scan(const FolderConfig& folders,
              const std::function<void(const ScanProgress&)>& progress = {},
              const std::atomic<bool>* cancel = nullptr);

    // Update/DLC packages found in the Updates & DLC folder and in ROM folders.
    std::vector<GameEntry> Installables() const;

    // Games for the grid, after hiding/sorting. Favourites always come first.
    std::vector<GameEntry> GridView(const QolSettings& qol, const std::string& filter = {}) const;

    std::vector<GameEntry> All() const;
    std::optional<GameEntry> Find(const std::string& path) const;
    std::optional<GameEntry> FindByTitleId(u64 title_id) const;

    // Mutations used by the UI; each one persists the cache.
    void Update(const GameEntry& entry);
    void RecordSession(const std::string& path, u64 seconds_played, u64 now_unix);

    bool Load();
    bool Save() const;

    std::string IconCachePath(u64 title_id, const std::string& path) const;

private:
    GameEntry Build(const std::string& path, const DirEntry& file);
    void Walk(const std::string& dir, int depth, std::vector<std::pair<std::string, DirEntry>>& out,
              const std::atomic<bool>* cancel);

    IFileSystem& fs_;
    std::string cache_dir_;
    mutable std::mutex mutex_;
    std::vector<GameEntry> games_;
};

} // namespace onyx
