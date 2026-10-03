// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/cheats.h"

#include <cctype>

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

namespace {
constexpr std::string_view kEnabledMarker = "*citra_enabled";
constexpr u64 kIndexMaxAge = 7ull * 24 * 3600;

bool IsHexWord(std::string_view w) {
    if (w.size() != 8) return false;
    for (char c : w)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
} // namespace

bool Cheat::IsValid() const {
    if (lines.empty()) return false;
    for (const auto& l : lines) {
        const auto parts = Split(Trim(l), ' ');
        std::vector<std::string> words;
        for (const auto& p : parts)
            if (!p.empty()) words.push_back(p);
        if (words.size() != 2 || !IsHexWord(words[0]) || !IsHexWord(words[1])) return false;
    }
    return true;
}

CheatFile CheatFile::Parse(std::string_view text) {
    CheatFile f;
    Cheat cur;
    bool have = false;
    auto flush = [&] {
        if (have && !cur.lines.empty()) f.cheats.push_back(cur);
        cur = Cheat{};
        have = false;
    };
    for (std::string line : Split(text, '\n')) {
        line.erase(std::remove(line.begin(), line.end(), '\0'), line.end());
        line = Trim(line);
        if (line.empty()) continue;
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            flush();
            cur.name = line.substr(1, line.size() - 2);
            have = true;
        } else if (line.front() == '*') {
            if (line == kEnabledMarker) cur.enabled = true;
            else cur.comments.push_back(line.substr(1));
        } else if (have) {
            cur.lines.push_back(line);
        }
    }
    flush();
    return f;
}

std::string CheatFile::Serialize() const {
    std::string out;
    for (const auto& c : cheats) {
        out += "[" + c.name + "]\n";
        if (c.enabled) {
            out += kEnabledMarker;
            out += '\n';
        }
        for (const auto& cm : c.comments) out += "*" + cm + "\n";
        for (const auto& l : c.lines) out += l + "\n";
        out += "\n";
    }
    return out;
}

Cheat* CheatFile::Find(std::string_view name) {
    for (auto& c : cheats)
        if (ToLower(c.name) == ToLower(name)) return &c;
    return nullptr;
}

int CheatFile::MergeFrom(const CheatFile& incoming) {
    int added = 0;
    for (const auto& c : incoming.cheats) {
        if (Find(c.name)) continue;
        Cheat copy = c;
        copy.enabled = false; // never switch on a downloaded cheat by surprise
        cheats.push_back(std::move(copy));
        ++added;
    }
    return added;
}

CheatDatabase::CheatDatabase(IHttpClient& http, IFileSystem& fs, std::string cache_dir,
                             CheatSource source)
    : http_(http), fs_(fs), cache_dir_(std::move(cache_dir)), source_(std::move(source)) {}

std::map<u64, std::string> CheatDatabase::ParseTree(std::string_view text,
                                                    const std::string& prefix) {
    std::map<u64, std::string> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("tree") || !j["tree"].is_array()) return out;
    for (const auto& e : j["tree"]) {
        if (e.value("type", "") != "blob") continue;
        const std::string path = e.value("path", "");
        if (!path.starts_with(prefix) || !EndsWithNoCase(path, ".txt")) continue;
        if (const auto tid = HexToTitleId(Stem(path)); tid && Stem(path).size() == 16)
            out[*tid] = path;
    }
    return out;
}

std::string CheatDatabase::RawUrl(const std::string& repo_path) const {
    std::string encoded;
    for (const auto& seg : Split(repo_path, '/')) {
        if (!encoded.empty()) encoded += '/';
        encoded += UrlEncode(seg);
    }
    return "https://raw.githubusercontent.com/" + source_.owner + "/" + source_.repo + "/" +
           source_.branch + "/" + encoded;
}

bool CheatDatabase::LoadCachedIndex(u64 now) {
    const std::string text = fs_.ReadText(JoinPath(cache_dir_, "cheatdb_index.json"));
    if (text.empty()) return false;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    const u64 fetched = j.value("fetched", u64{0});
    if (now > fetched + kIndexMaxAge) return false;
    index_.clear();
    const json idx = j.value("index", json::object());
    for (const auto& [k, v] : idx.items())
        if (const auto tid = HexToTitleId(k); tid && v.is_string()) index_[*tid] = v.get<std::string>();
    return !index_.empty();
}

bool CheatDatabase::RefreshIndex(u64 now, bool force) {
    if (!force && LoadCachedIndex(now)) return true;
    const std::string url = "https://api.github.com/repos/" + source_.owner + "/" +
                            source_.repo + "/git/trees/" + source_.branch + "?recursive=1";
    const HttpResponse r = http_.Get(url, {{"Accept", "application/vnd.github+json"},
                                           {"User-Agent", "ONYX3DS"}});
    if (!r.ok()) {
        // Offline or rate limited: fall back to a stale cache rather than nothing.
        return LoadCachedIndex(0) || !index_.empty();
    }
    auto parsed = ParseTree(r.body, source_.prefix);
    if (parsed.empty()) return !index_.empty();
    index_ = std::move(parsed);
    json idx = json::object();
    for (const auto& [tid, path] : index_) idx[TitleIdToHex(tid)] = path;
    fs_.CreateDirs(cache_dir_);
    fs_.WriteText(JoinPath(cache_dir_, "cheatdb_index.json"),
                  json{{"fetched", now}, {"source", source_.label}, {"index", idx}}.dump());
    return true;
}

std::optional<std::string> CheatDatabase::PathFor(u64 title_id) const {
    if (auto it = index_.find(title_id); it != index_.end()) return it->second;
    return std::nullopt;
}

std::optional<CheatFile> CheatDatabase::Fetch(u64 title_id) {
    std::string path;
    if (auto p = PathFor(title_id)) {
        path = *p;
    } else {
        return std::nullopt;
    }
    const HttpResponse r = http_.Get(RawUrl(path), {{"User-Agent", "ONYX3DS"}});
    if (!r.ok()) return std::nullopt;
    CheatFile f = CheatFile::Parse(r.body);
    // Drop entries that are only headings or have malformed codes.
    std::erase_if(f.cheats, [](const Cheat& c) { return !c.IsValid(); });
    if (f.cheats.empty()) return std::nullopt;
    return f;
}

int CheatDatabase::DownloadInto(u64 title_id, const std::string& cheats_dir) {
    const auto remote = Fetch(title_id);
    if (!remote) return -1;
    const std::string file = JoinPath(cheats_dir, TitleIdToHex(title_id) + ".txt");
    CheatFile local = CheatFile::Parse(fs_.ReadText(file));
    const int added = local.MergeFrom(*remote);
    if (added > 0) {
        fs_.CreateDirs(cheats_dir);
        if (!fs_.WriteText(file, local.Serialize())) return -1;
    }
    return added;
}

} // namespace onyx
