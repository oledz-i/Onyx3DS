// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/library.h"

#include <algorithm>
#include <functional>
#include <map>

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

std::string CleanFileTitle(std::string_view file_name) {
    std::string s = Stem(file_name);
    // Drop "(USA)", "[!]", "(Rev 1)" style tags, then tidy separators.
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') {
            ++depth;
            continue;
        }
        if ((c == ')' || c == ']') && depth > 0) {
            --depth;
            continue;
        }
        if (depth == 0) out += (c == '_' ? ' ' : c);
    }
    // Collapse double spaces left behind by removed tags.
    std::string tidy;
    for (char c : out) {
        if (c == ' ' && !tidy.empty() && tidy.back() == ' ') continue;
        tidy += c;
    }
    tidy = Trim(tidy);
    while (!tidy.empty() && (tidy.back() == '-' || tidy.back() == ' ')) tidy.pop_back();
    return tidy.empty() ? Stem(file_name) : tidy;
}

GameLibrary::GameLibrary(IFileSystem& fs, std::string cache_dir)
    : fs_(fs), cache_dir_(NormalizeSlashes(cache_dir)) {}

std::string GameLibrary::IconCachePath(u64 title_id, const std::string& path) const {
    // Homebrew has no title ID; hash the path instead so icons don't collide.
    const std::string key = title_id ? TitleIdToHex(title_id)
                                     : "path-" + std::to_string(std::hash<std::string>{}(path));
    return JoinPath(cache_dir_, "icons/" + key + ".rgba");
}

void GameLibrary::Walk(const std::string& dir, int depth,
                       std::vector<std::pair<std::string, DirEntry>>& out,
                       const std::atomic<bool>* cancel) {
    if (depth > 4 || (cancel && cancel->load())) return;
    for (const auto& e : fs_.List(dir)) {
        if (e.name.empty() || e.name[0] == '.') continue;
        const std::string full = JoinPath(dir, e.name);
        if (e.is_dir) {
            // A texture pack accidentally dropped in the ROM folder is huge and
            // has no ROMs in it; skip folders named like a title ID.
            if (e.name.size() == 16 && HexToTitleId(e.name)) continue;
            Walk(full, depth + 1, out, cancel);
        } else if (n3ds::IsRomExtension(Extension(e.name))) {
            out.emplace_back(full, e);
        }
    }
}

GameEntry GameLibrary::Build(const std::string& path, const DirEntry& file) {
    GameEntry g;
    g.path = path;
    g.file_size = file.size;
    const n3ds::RomInfo info = n3ds::Inspect(fs_, path);
    g.format = info.format;
    g.title_id = info.title_id;
    g.kind = info.kind;
    g.product_code = info.product_code;
    g.encrypted = info.encrypted;
    g.note = info.error;
    if (info.smdh) {
        const auto& t = info.smdh->Best(n3ds::Language::English);
        g.title = t.short_name;
        g.long_title = t.long_name;
        g.publisher = t.publisher;
        g.region = info.smdh->RegionString();
        if (!info.smdh->icon_rgba.empty()) {
            g.icon_path = IconCachePath(g.title_id, path);
            fs_.CreateDirs(JoinPath(cache_dir_, "icons"));
            fs_.WriteAll(g.icon_path, info.smdh->icon_rgba);
        }
    }
    if (g.title.empty()) g.title = CleanFileTitle(FileName(path));
    if (g.region.empty()) {
        const std::string lower = ToLower(FileName(path));
        if (lower.find("(usa") != std::string::npos) g.region = "USA";
        else if (lower.find("(eur") != std::string::npos || lower.find("(europe") != std::string::npos)
            g.region = "EUR";
        else if (lower.find("(jap") != std::string::npos || lower.find("(jpn") != std::string::npos)
            g.region = "JPN";
    }
    return g;
}

void GameLibrary::Scan(const FolderConfig& folders,
                       const std::function<void(const ScanProgress&)>& progress,
                       const std::atomic<bool>* cancel) {
    std::vector<std::pair<std::string, DirEntry>> files;
    for (const auto& dir : folders.roms) Walk(dir, 0, files, cancel);
    if (!folders.updates_dlc.empty()) Walk(folders.updates_dlc, 0, files, cancel);

    std::map<std::string, GameEntry> previous;
    {
        std::lock_guard lock(mutex_);
        for (auto& g : games_) previous.emplace(ToLower(g.path), g);
    }

    std::vector<GameEntry> result;
    ScanProgress p;
    // The same file can be reachable from two configured folders.
    std::map<std::string, bool> seen;
    for (const auto& [path, entry] : files) {
        if (cancel && cancel->load()) return;
        const std::string key = ToLower(path);
        if (seen[key]) continue;
        seen[key] = true;
        ++p.files_seen;
        p.current = entry.name;
        if (auto it = previous.find(key); it != previous.end() && it->second.file_size == entry.size) {
            result.push_back(it->second); // unchanged: keep art, play time, favourites
        } else {
            GameEntry g = Build(path, entry);
            if (it != previous.end()) { // file replaced (e.g. decrypted copy): keep user data
                g.favorite = it->second.favorite;
                g.play_seconds = it->second.play_seconds;
                g.last_played = it->second.last_played;
                g.launch_count = it->second.launch_count;
                g.user_title = it->second.user_title;
                g.grid_art = it->second.grid_art;
                g.hero_art = it->second.hero_art;
                g.logo_art = it->second.logo_art;
            }
            result.push_back(std::move(g));
            ++p.files_parsed;
        }
        if (progress) progress(p);
    }
    {
        std::lock_guard lock(mutex_);
        games_ = std::move(result);
    }
    Save();
}

std::vector<GameEntry> GameLibrary::Installables() const {
    std::lock_guard lock(mutex_);
    std::vector<GameEntry> out;
    for (const auto& g : games_)
        if (g.kind == n3ds::TitleKind::Update || g.kind == n3ds::TitleKind::DLC) out.push_back(g);
    std::sort(out.begin(), out.end(), [](const GameEntry& a, const GameEntry& b) {
        return a.title_id < b.title_id;
    });
    return out;
}

std::vector<GameEntry> GameLibrary::GridView(const QolSettings& qol,
                                             const std::string& filter) const {
    std::vector<GameEntry> out;
    {
        std::lock_guard lock(mutex_);
        const std::string f = ToLower(filter);
        for (const auto& g : games_) {
            if (g.hidden) continue;
            if (qol.hide_updates_dlc_from_grid && !g.IsLaunchable()) continue;
            if (!f.empty() && ToLower(g.DisplayTitle()).find(f) == std::string::npos &&
                ToLower(g.publisher).find(f) == std::string::npos)
                continue;
            out.push_back(g);
        }
    }
    auto by_title = [](const GameEntry& a, const GameEntry& b) {
        return ToLower(a.DisplayTitle()) < ToLower(b.DisplayTitle());
    };
    std::function<bool(const GameEntry&, const GameEntry&)> cmp = by_title;
    switch (qol.sort) {
    case SortMode::RecentlyPlayed:
        cmp = [&](const GameEntry& a, const GameEntry& b) {
            if (a.last_played != b.last_played) return a.last_played > b.last_played;
            return by_title(a, b);
        };
        break;
    case SortMode::MostPlayed:
        cmp = [&](const GameEntry& a, const GameEntry& b) {
            if (a.play_seconds != b.play_seconds) return a.play_seconds > b.play_seconds;
            return by_title(a, b);
        };
        break;
    case SortMode::Publisher:
        cmp = [&](const GameEntry& a, const GameEntry& b) {
            if (a.publisher != b.publisher) return ToLower(a.publisher) < ToLower(b.publisher);
            return by_title(a, b);
        };
        break;
    case SortMode::Region:
        cmp = [&](const GameEntry& a, const GameEntry& b) {
            if (a.region != b.region) return a.region < b.region;
            return by_title(a, b);
        };
        break;
    case SortMode::Title: break;
    }
    std::stable_sort(out.begin(), out.end(), [&](const GameEntry& a, const GameEntry& b) {
        if (a.favorite != b.favorite) return a.favorite;
        return cmp(a, b);
    });
    return out;
}

std::vector<GameEntry> GameLibrary::All() const {
    std::lock_guard lock(mutex_);
    return games_;
}

std::optional<GameEntry> GameLibrary::Find(const std::string& path) const {
    std::lock_guard lock(mutex_);
    const std::string key = ToLower(NormalizeSlashes(path));
    for (const auto& g : games_)
        if (ToLower(g.path) == key) return g;
    return std::nullopt;
}

std::optional<GameEntry> GameLibrary::FindByTitleId(u64 title_id) const {
    std::lock_guard lock(mutex_);
    for (const auto& g : games_)
        if (g.title_id == title_id && g.IsLaunchable()) return g;
    return std::nullopt;
}

void GameLibrary::Update(const GameEntry& entry) {
    {
        std::lock_guard lock(mutex_);
        for (auto& g : games_)
            if (ToLower(g.path) == ToLower(entry.path)) g = entry;
    }
    Save();
}

void GameLibrary::RecordSession(const std::string& path, u64 seconds, u64 now) {
    {
        std::lock_guard lock(mutex_);
        for (auto& g : games_) {
            if (ToLower(g.path) != ToLower(path)) continue;
            g.play_seconds += seconds;
            g.last_played = now;
            ++g.launch_count;
        }
    }
    Save();
}

namespace {

json ToJson(const GameEntry& g) {
    return {
        {"path", g.path}, {"title", g.title}, {"long_title", g.long_title},
        {"publisher", g.publisher}, {"region", g.region}, {"product_code", g.product_code},
        {"title_id", TitleIdToHex(g.title_id)}, {"kind", static_cast<int>(g.kind)},
        {"format", static_cast<int>(g.format)}, {"encrypted", g.encrypted},
        {"file_size", g.file_size}, {"note", g.note}, {"icon_path", g.icon_path},
        {"grid_art", g.grid_art}, {"hero_art", g.hero_art}, {"logo_art", g.logo_art},
        {"user_title", g.user_title}, {"favorite", g.favorite}, {"hidden", g.hidden},
        {"play_seconds", g.play_seconds}, {"last_played", g.last_played},
        {"launch_count", g.launch_count},
    };
}

GameEntry FromJson(const json& j) {
    GameEntry g;
    auto str = [&](const char* k) { return j.value(k, std::string{}); };
    g.path = str("path");
    g.title = str("title");
    g.long_title = str("long_title");
    g.publisher = str("publisher");
    g.region = str("region");
    g.product_code = str("product_code");
    g.title_id = HexToTitleId(str("title_id")).value_or(0);
    g.kind = static_cast<n3ds::TitleKind>(j.value("kind", 0));
    g.format = static_cast<n3ds::RomFormat>(j.value("format", 0));
    g.encrypted = j.value("encrypted", false);
    g.file_size = j.value("file_size", u64{0});
    g.note = str("note");
    g.icon_path = str("icon_path");
    g.grid_art = str("grid_art");
    g.hero_art = str("hero_art");
    g.logo_art = str("logo_art");
    g.user_title = str("user_title");
    g.favorite = j.value("favorite", false);
    g.hidden = j.value("hidden", false);
    g.play_seconds = j.value("play_seconds", u64{0});
    g.last_played = j.value("last_played", u64{0});
    g.launch_count = j.value("launch_count", u32{0});
    return g;
}

} // namespace

bool GameLibrary::Save() const {
    json arr = json::array();
    {
        std::lock_guard lock(mutex_);
        for (const auto& g : games_) arr.push_back(ToJson(g));
    }
    fs_.CreateDirs(cache_dir_);
    const json root = {{"version", 1}, {"games", arr}};
    return fs_.WriteText(JoinPath(cache_dir_, "library.json"), root.dump());
}

bool GameLibrary::Load() {
    const std::string text = fs_.ReadText(JoinPath(cache_dir_, "library.json"));
    if (text.empty()) return false;
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("games")) return false;
    std::vector<GameEntry> loaded;
    for (const auto& j : root["games"])
        if (j.is_object()) loaded.push_back(FromJson(j));
    std::lock_guard lock(mutex_);
    games_ = std::move(loaded);
    return true;
}

} // namespace onyx
