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

private:
    std::string InfoPath(const GameEntry& game) const;
    std::vector<std::string> SnapNames(GameSystem system);
    void FetchShots(const GameEntry& game, GameInfo& info);

    IHttpClient& http_;
    IFileSystem& fs_;
    std::string dir_;
    std::mutex mutex_;
    std::map<int, std::vector<std::string>> listings_;
};

} // namespace onyx
