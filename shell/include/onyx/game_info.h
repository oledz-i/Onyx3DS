// SPDX-License-Identifier: GPL-3.0-or-later
//
// Descriptions, release details and screenshots for the crossbar's hover panel.
// Text comes from the game's Wikipedia article (public API, no key); screenshots come
// from the community-made libretro thumbnail server. Nothing is bundled: everything is
// fetched on the player's console, cached on disk, and silently absent when not found.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "onyx/library.h"
#include "onyx/platform.h"

namespace onyx {

struct GameInfo {
    bool found = false;
    std::string summary;
    std::string developer;
    std::string publisher;
    std::string released;
    std::string genre;
    std::string source; // article title
    std::vector<std::string> shots; // local picture files: gameplay first, then title screen
    int score = 0;                  // critics' average out of 100 (0 = none found)
    std::string score_source;       // where the score came from (Metacritic, GameRankings)
    std::string boxart;             // local picture file of the box art ("" = none)
    // The critics' score as stars, in halves: 87 -> 4.5 (0 when there is no score).
    double Stars() const { return score <= 0 ? 0.0 : static_cast<int>(score / 10.0 + 0.5) / 2.0; }
};

class GameInfoService {
public:
    GameInfoService(IHttpClient& http, IFileSystem& fs, std::string cache_dir);

    // Disk only; never touches the network. found == false when nothing is cached.
    GameInfo Cached(const GameEntry& game) const;
    // Cached, or fetched now (call off the UI thread). Misses are remembered for two weeks.
    GameInfo Get(const GameEntry& game);

    // Parsers, exposed for tests.
    static std::vector<std::string> ParseSearchTitles(std::string_view json);
    struct Article {
        std::string title, extract, wikitext;
    };
    static std::vector<Article> ParseArticles(std::string_view json);
    static GameInfo FromArticle(const Article& article);
    static std::string CleanWikiText(std::string text);
    static std::string ReleaseDate(std::string_view value);
    static std::vector<std::string> ParseListing(std::string_view html);
    static std::string BestSnapName(const std::vector<std::string>& names, const std::string& title);
    static const char* LibretroFolder(GameSystem system);
    static std::string Key(const GameEntry& game);
    // "85/100" style critics' score from an article's review box. Returns 0 when there is none.
    static int ParseScore(std::string_view wikitext, std::string* source = nullptr);
    // Box art on disk for the game, downloading it if it is not there yet (call off the UI thread).
    // Returns "" when the thumbnail project has none.
    std::string EnsureBoxArt(const GameEntry& game);
    // Disk only.
    std::string CachedBoxArt(const GameEntry& game) const;

private:
    std::string InfoPath(const GameEntry& game) const;
    std::vector<std::string> SnapNames(GameSystem system, const char* kind = "Named_Snaps");
    void FetchShots(const GameEntry& game, GameInfo& info);

    IHttpClient& http_;
    IFileSystem& fs_;
    std::string dir_;
    std::mutex mutex_;
    std::map<std::string, std::vector<std::string>> listings_;
};

} // namespace onyx
