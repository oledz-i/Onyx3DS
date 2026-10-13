// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/steamgriddb.h"

#include <algorithm>
#include <functional>

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
        if (g.contains("release_date") && g["release_date"].is_number_integer()) {
            const long long secs = g["release_date"].get<long long>();
            if (secs > 0) game.year = 1970 + static_cast<int>(secs / 31556952LL);
        }
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
    auto images = body ? ParseImages(*body) : std::vector<SgdbImage>{};
    if (images.empty() && body && kind == SgdbArt::Grid) {
        // The two exact capsule sizes are rare; any static grid still fills a tile.
        const auto any = GetJson("/grids/game/" + std::to_string(game_id) +
                                 "?mimes=image/png,image/jpeg&types=static&nsfw=false&humor=false");
        if (any) images = ParseImages(*any);
    }
    return images;
}

std::optional<std::string> SteamGridDb::Download(const std::string& url) {
    // Image CDN URLs are public; no API key needed (and none should be sent).
    const HttpResponse r = http_.Get(url, {{"User-Agent", "ONYX3DS"}});
    last_status_ = r.status;
    if (!r.ok() || r.body.empty()) return std::nullopt;
    return r.body;
}

std::vector<SgdbGame> SteamGridDb::Rank(std::vector<SgdbGame> results, const std::string& title) {
    const std::string want = Canonical(title);
    auto score = [&](const SgdbGame& g) {
        const std::string name = Canonical(g.name);
        int s = 0;
        if (!want.empty() && name == want) s += 100;
        else if (!want.empty() && name.starts_with(want)) s += 40;
        if (name.find("3ds") != std::string::npos) s += 60;
        if (g.year >= 2011 && g.year <= 2020) s += 30;
        else if (g.year > 0 && g.year < 2010) s -= 20;
        if (g.verified) s += 10;
        return s;
    };
    std::stable_sort(results.begin(), results.end(),
                     [&](const SgdbGame& a, const SgdbGame& b) { return score(a) > score(b); });
    return results;
}

std::optional<SgdbGame> SteamGridDb::BestMatch(const std::vector<SgdbGame>& results,
                                               const std::string& title) {
    if (results.empty()) return std::nullopt;
    return Rank(results, title).front();
}

ArtScraper::ArtScraper(SteamGridDb& sgdb, IFileSystem& fs, std::string art_dir)
    : sgdb_(sgdb), fs_(fs), art_dir_(std::move(art_dir)) {}

void ArtMissCache::Load(IFileSystem& fs, const std::string& path) {
    std::scoped_lock lock(mutex_);
    missed_.clear();
    const std::string text = fs.ReadText(path);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        try {
            missed_[line.substr(0, tab)] = std::stoull(line.substr(tab + 1));
        } catch (...) {
        }
    }
}

void ArtMissCache::Save(IFileSystem& fs, const std::string& path) const {
    std::string text;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [k, t] : missed_) text += k + "\t" + std::to_string(t) + "\n";
    }
    fs.WriteText(path, text);
}

bool ArtMissCache::RecentlyMissed(const std::string& key, u64 now) const {
    std::scoped_lock lock(mutex_);
    const auto it = missed_.find(key);
    return it != missed_.end() && now >= it->second && now - it->second < kRetrySeconds;
}

void ArtMissCache::Add(const std::string& key, u64 now) {
    std::scoped_lock lock(mutex_);
    missed_[key] = now;
}

void ArtMissCache::Forget(const std::string& key) {
    std::scoped_lock lock(mutex_);
    missed_.erase(key);
}

std::optional<std::string> ArtScraper::SaveFirst(SgdbArt kind, u64 game_id,
                                                 const std::string& base) {
    int tried = 0;
    for (const auto& img : sgdb_.Images(kind, game_id)) {
        if (++tried > 3) break; // a few dead links are fine, a whole list of them is not
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
    return ScrapeGame(game, match->id, overwrite);
}

bool ArtScraper::ScrapeGame(GameEntry& game, u64 sgdb_game_id, bool overwrite) {
    if (!sgdb_.HasKey() || !sgdb_game_id) return false;
    const std::string key =
        game.title_id ? game.TitleIdHex() : "game-" + std::to_string(sgdb_game_id);
    const std::string base = JoinPath(art_dir_, key);
    bool any = false;
    auto fetch = [&](std::string& slot, SgdbArt kind, const char* suffix) {
        if (!overwrite && !slot.empty() && fs_.Exists(slot)) return;
        if (auto p = SaveFirst(kind, sgdb_game_id, base + suffix)) {
            slot = *p;
            any = true;
            if (on_saved) on_saved(game);
        }
    };
    fetch(game.grid_art, SgdbArt::Grid, "_grid");
    fetch(game.hero_art, SgdbArt::Hero, "_hero");
    fetch(game.logo_art, SgdbArt::Logo, "_logo");
    return any;
}

bool ArtScraper::SetGridFromUrl(GameEntry& game, const std::string& url) {
    const std::string u = Trim(url);
    if (!u.starts_with("https://") && !u.starts_with("http://")) return false;
    const auto data = sgdb_.Download(u);
    if (!data || data->size() < 8) return false;
    const std::string& d = *data;
    const bool png = d.compare(0, 4, "\x89PNG") == 0;
    const bool jpg = static_cast<unsigned char>(d[0]) == 0xFF && static_cast<unsigned char>(d[1]) == 0xD8;
    if (!png && !jpg) return false;
    const std::string key = game.title_id
                                ? game.TitleIdHex()
                                : "custom-" + std::to_string(std::hash<std::string>{}(game.path));
    const std::string path = JoinPath(art_dir_, key + "_grid" + (png ? ".png" : ".jpg"));
    fs_.CreateDirs(art_dir_);
    if (!fs_.WriteText(path, d)) return false;
    game.grid_art = path;
    return true;
}

} // namespace onyx
