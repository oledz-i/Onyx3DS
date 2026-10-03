// SPDX-License-Identifier: GPL-3.0-or-later
//
// SteamGridDB (https://www.steamgriddb.com) artwork for the channel tiles and
// the game details banner. Needs a free personal API key from
// steamgriddb.com/profile/preferences/api, entered in Settings > Services.
#pragma once

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
    static std::vector<SgdbGame> ParseSearch(std::string_view json);
    static std::vector<SgdbImage> ParseImages(std::string_view json);
    static std::string QueryFor(SgdbArt kind);

private:
    std::optional<std::string> GetJson(const std::string& path);

    IHttpClient& http_;
    std::string key_;
    int last_status_ = 0;
};

// Downloads grid/hero/logo for a game into <art_dir> and fills in the
// GameEntry paths. Returns true when at least one image was saved.
class ArtScraper {
public:
    ArtScraper(SteamGridDb& sgdb, IFileSystem& fs, std::string art_dir);
    bool Scrape(GameEntry& game, bool overwrite = false);

private:
    std::optional<std::string> SaveFirst(SgdbArt kind, u64 game_id, const std::string& base);

    SteamGridDb& sgdb_;
    IFileSystem& fs_;
    std::string art_dir_;
};

} // namespace onyx
