// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/steamgriddb.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

namespace {
constexpr const char* kBase = "https://www.steamgriddb.com/api/v2";

// Strip trademark symbols and punctuation differences before comparing names.
std::string Canonical(std::string_view s) {
    std::string out;
    for (unsigned char c : ToLower(s))
        if (std::isalnum(c)) out += static_cast<char>(c);
    return out;
}
} // namespace

SteamGridDb::SteamGridDb(IHttpClient& http, std::string api_key)
    : http_(http), key_(Trim(api_key)) {}

std::optional<std::string> SteamGridDb::GetJson(const std::string& path) {
    if (key_.empty()) return std::nullopt;
    const HttpResponse r =
        http_.Get(std::string(kBase) + path,
                  {{"Authorization", "Bearer " + key_}, {"User-Agent", "ONYX3DS"}});
    last_status_ = r.status;
    if (!r.ok()) return std::nullopt;
    return r.body;
}

std::vector<SgdbGame> SteamGridDb::ParseSearch(std::string_view text) {
    std::vector<SgdbGame> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.value("success", false) || !j["data"].is_array()) return out;
    for (const auto& g : j["data"]) {
        SgdbGame game;
        game.id = g.value("id", u64{0});
        game.name = g.value("name", "");
        game.verified = g.value("verified", false);
        if (game.id) out.push_back(game);
    }
    return out;
}

std::vector<SgdbImage> SteamGridDb::ParseImages(std::string_view text) {
    std::vector<SgdbImage> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.value("success", false) || !j["data"].is_array()) return out;
    for (const auto& i : j["data"]) {
        SgdbImage img;
        img.id = i.value("id", u64{0});
        img.url = i.value("url", "");
        img.thumb = i.value("thumb", "");
        img.mime = i.value("mime", "");
        img.style = i.value("style", "");
        img.width = i.value("width", 0);
        img.height = i.value("height", 0);
        img.score = i.value("score", 0);
        if (!img.url.empty()) out.push_back(img);
    }
    // Highest community score first; the API already sorts, but be explicit.
    std::stable_sort(out.begin(), out.end(),
                     [](const SgdbImage& a, const SgdbImage& b) { return a.score > b.score; });
    return out;
}

std::string SteamGridDb::QueryFor(SgdbArt kind) {
    // PNG/JPEG only: the console's image decoders handle those everywhere.
    switch (kind) {
    case SgdbArt::Grid:
        // Horizontal capsules match the channel tile shape.
        return "/grids/game/{id}?dimensions=920x430,460x215&mimes=image/png,image/jpeg"
               "&types=static&nsfw=false&humor=false";
    case SgdbArt::Hero:
        return "/heroes/game/{id}?mimes=image/png,image/jpeg&types=static&nsfw=false&humor=false";
    case SgdbArt::Logo:
        return "/logos/game/{id}?mimes=image/png&types=static&nsfw=false&humor=false";
    case SgdbArt::Icon:
        return "/icons/game/{id}?mimes=image/png&types=static&nsfw=false&humor=false";
    }
    return {};
}

std::vector<SgdbGame> SteamGridDb::Search(const std::string& term) {
    const auto body = GetJson("/search/autocomplete/" + UrlEncode(term));
    return body ? ParseSearch(*body) : std::vector<SgdbGame>{};
}

std::vector<SgdbImage> SteamGridDb::Images(SgdbArt kind, u64 game_id) {
    std::string q = QueryFor(kind);
    q.replace(q.find("{id}"), 4, std::to_string(game_id));
    const auto body = GetJson(q);
    return body ? ParseImages(*body) : std::vector<SgdbImage>{};
}

std::optional<std::string> SteamGridDb::Download(const std::string& url) {
    // Image CDN URLs are public; no API key needed (and none should be sent).
    const HttpResponse r = http_.Get(url, {{"User-Agent", "ONYX3DS"}});
    last_status_ = r.status;
    if (!r.ok() || r.body.empty()) return std::nullopt;
    return r.body;
}

std::optional<SgdbGame> SteamGridDb::BestMatch(const std::vector<SgdbGame>& results,
                                               const std::string& title) {
    if (results.empty()) return std::nullopt;
    const std::string want = Canonical(title);
    for (const auto& g : results)
        if (Canonical(g.name) == want) return g;
    // "Pokemon X" vs "Pokémon X and Y": accept a result that starts with the title.
    for (const auto& g : results)
        if (!want.empty() && Canonical(g.name).starts_with(want)) return g;
    for (const auto& g : results)
        if (g.verified) return g;
    return results.front();
}

ArtScraper::ArtScraper(SteamGridDb& sgdb, IFileSystem& fs, std::string art_dir)
    : sgdb_(sgdb), fs_(fs), art_dir_(std::move(art_dir)) {}

std::optional<std::string> ArtScraper::SaveFirst(SgdbArt kind, u64 game_id,
                                                 const std::string& base) {
    for (const auto& img : sgdb_.Images(kind, game_id)) {
        const auto data = sgdb_.Download(img.url);
        if (!data) continue;
        const std::string ext = img.mime == "image/jpeg" ? ".jpg" : ".png";
        const std::string path = base + ext;
        fs_.CreateDirs(art_dir_);
        if (fs_.WriteText(path, *data)) return path;
    }
    return std::nullopt;
}

bool ArtScraper::Scrape(GameEntry& game, bool overwrite) {
    if (!sgdb_.HasKey()) return false;
    const auto match = SteamGridDb::BestMatch(sgdb_.Search(game.DisplayTitle()),
                                              game.DisplayTitle());
    if (!match) return false;
    const std::string key =
        game.title_id ? game.TitleIdHex() : "game-" + std::to_string(match->id);
    const std::string base = JoinPath(art_dir_, key);
    bool any = false;
    auto fetch = [&](std::string& slot, SgdbArt kind, const char* suffix) {
        if (!overwrite && !slot.empty() && fs_.Exists(slot)) return;
        if (auto p = SaveFirst(kind, match->id, base + suffix)) {
            slot = *p;
            any = true;
        }
    };
    fetch(game.grid_art, SgdbArt::Grid, "_grid");
    fetch(game.hero_art, SgdbArt::Hero, "_hero");
    fetch(game.logo_art, SgdbArt::Logo, "_logo");
    return any;
}

} // namespace onyx
