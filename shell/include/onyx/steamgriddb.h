// SPDX-License-Identifier: GPL-3.0-or-later
//
// SteamGridDB (https://www.steamgriddb.com) artwork for the channel tiles and
// the game details banner. Needs a free personal API key from
// steamgriddb.com/profile/preferences/api, entered in Settings > Services.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "onyx/library.h"
#include "onyx/platform.h"

namespace onyx {

struct SgdbGame {
    u64 id = 0;
    std::string name;
    bool verified = false;
    int year = 0; // release year when SteamGridDB reports one, else 0
};

enum class SgdbArt { Grid, Hero, Logo, Icon };

struct SgdbImage {
    u64 id = 0;
    std::string url;
    std::string thumb;
    std::string mime;
    std::string style;
    int width = 0;
    int height = 0;
    int score = 0;
};

class SteamGridDb {
public:
    SteamGridDb(IHttpClient& http, std::string api_key);

    bool HasKey() const { return !key_.empty(); }
    // Last HTTP status, so Settings can say "key rejected" vs "offline".
    int LastStatus() const { return last_status_; }

    std::vector<SgdbGame> Search(const std::string& term);
    std::vector<SgdbImage> Images(SgdbArt kind, u64 game_id);
    std::optional<std::string> Download(const std::string& url);

    // Picks the closest match for a 3DS title: exact name first, then the
    // first verified result, then the first result.
    static std::optional<SgdbGame> BestMatch(const std::vector<SgdbGame>& results,
                                             const std::string& title);
    // All results, best match for a 3DS title first: exact name, then names that
    // start with it, 3DS-era releases (2011-2020) and "3DS" in the name above
    // old or unrelated releases (so "Smash Bros." finds the 3DS game, not the
    // N64 one), then verified entries.
    static std::vector<SgdbGame> Rank(std::vector<SgdbGame> results, const std::string& title);
    static std::vector<SgdbGame> ParseSearch(std::string_view json);
    static std::vector<SgdbImage> ParseImages(std::string_view json);
    static std::string QueryFor(SgdbArt kind);

private:
    std::optional<std::string> GetJson(const std::string& path);

    IHttpClient& http_;
    std::string key_;
    int last_status_ = 0;
};

// Games SteamGridDB had nothing for, remembered between launches so an unmatched game
// (a ROM hack, homebrew) is not searched for again on every start. Thread-safe.
class ArtMissCache {
public:
    static constexpr u64 kRetrySeconds = 7ull * 24 * 3600;
    void Load(IFileSystem& fs, const std::string& path);
    void Save(IFileSystem& fs, const std::string& path) const;
    bool RecentlyMissed(const std::string& key, u64 now) const;
    void Add(const std::string& key, u64 now);
    void Forget(const std::string& key);

private:
    mutable std::mutex mutex_;
    std::map<std::string, u64> missed_;
};

// Downloads grid/hero/logo for a game into <art_dir> and fills in the
// GameEntry paths. Returns true when at least one image was saved.
class ArtScraper {
public:
    // Called (on the scraping thread) after each picture is saved, with the game as it is so far,
    // so a tile can show its cover before the hero and logo have finished downloading.
    std::function<void(const GameEntry&)> on_saved;

    ArtScraper(SteamGridDb& sgdb, IFileSystem& fs, std::string art_dir);
    bool Scrape(GameEntry& game, bool overwrite = false);
    // Same, for a game the user picked themselves.
    bool ScrapeGame(GameEntry& game, u64 sgdb_game_id, bool overwrite = true);
    // Uses an image the user pasted a link to as the game's grid art. PNG or
    // JPEG only; returns false when the download isn't a usable image.
    bool SetGridFromUrl(GameEntry& game, const std::string& url);

private:
    std::optional<std::string> SaveFirst(SgdbArt kind, u64 game_id, const std::string& base);

    SteamGridDb& sgdb_;
    IFileSystem& fs_;
    std::string art_dir_;
};

} // namespace onyx
