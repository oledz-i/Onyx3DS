// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/game_info.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

namespace {
constexpr u64 kRetryMissSeconds = 14ull * 24 * 3600;
const char* kApi = "https://en.wikipedia.org/w/api.php";
const char* kThumbs = "https://thumbnails.libretro.com/";

u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

std::string Canonical(std::string_view s) {
    std::string out;
    for (unsigned char c : ToLower(s))
        if (std::isalnum(c)) out += static_cast<char>(c);
    return out;
}

// "Name (USA) (Rev 1)" -> "Name"
std::string StripTags(std::string s) {
    for (;;) {
        const auto open = s.find('(');
        if (open == std::string::npos) break;
        const auto close = s.find(')', open);
        if (close == std::string::npos) break;
        s.erase(open, close - open + 1);
    }
    return Trim(s);
}

std::string UrlDecode(std::string_view in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() + 0 && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else if (in[i] == '&' && in.substr(i, 5) == "&amp;") {
            out += '&';
            i += 4;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string InfoboxField(const std::string& wikitext, const char* name) {
    // "| name = value" up to the next line starting with "|" or the end of the box.
    const std::regex re(std::string("\\n\\s*\\|\\s*") + name + "\\s*=\\s*", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(wikitext, m, re)) return {};
    size_t pos = m.position(0) + m.length(0);
    int depth = 0;
    size_t end = pos;
    while (end < wikitext.size()) {
        if (wikitext.compare(end, 2, "{{") == 0 || wikitext.compare(end, 2, "[[") == 0) {
            ++depth;
            end += 2;
            continue;
        }
        if (wikitext.compare(end, 2, "}}") == 0 || wikitext.compare(end, 2, "]]") == 0) {
            if (depth == 0) break;
            --depth;
            end += 2;
            continue;
        }
        if (depth == 0 && wikitext[end] == '\n' && wikitext.find_first_not_of(" \t", end + 1) != std::string::npos &&
            wikitext[wikitext.find_first_not_of(" \t", end + 1)] == '|')
            break;
        ++end;
    }
    return wikitext.substr(pos, end - pos);
}
} // namespace

std::string GameInfoService::CleanWikiText(std::string t) {
    auto erase_between = [&](const std::string& open, const std::string& close) {
        for (size_t a; (a = t.find(open)) != std::string::npos;) {
            const size_t b = t.find(close, a + open.size());
            t.erase(a, b == std::string::npos ? std::string::npos : b + close.size() - a);
        }
    };
    erase_between("<!--", "-->");
    erase_between("<ref", "</ref>");
    // List templates keep their entries; everything else in {{ }} goes.
    for (const char* name : {"{{plainlist|", "{{Plainlist|", "{{ubl|", "{{unbulleted list|", "{{Unbulleted list|",
                             "{{flatlist|", "{{Flatlist|", "{{hlist|"}) {
        for (size_t a; (a = t.find(name)) != std::string::npos;) {
            t.erase(a, std::string(name).size());
            const size_t b = t.find("}}", a);
            if (b != std::string::npos) t.replace(b, 2, "");
            if (std::string(name).find("ubl") != std::string::npos || std::string(name).find("nbulleted") != std::string::npos ||
                std::string(name).find("hlist") != std::string::npos)
                for (size_t bar; (bar = t.find('|', a)) != std::string::npos && (b == std::string::npos || bar < b);)
                    t.replace(bar, 1, ", ");
        }
    }
    for (int guard = 0; guard < 8; ++guard) {
        const size_t close = t.find("}}");
        if (close == std::string::npos) break;
        const size_t open = t.rfind("{{", close);
        if (open == std::string::npos) {
            t.erase(close, 2);
            continue;
        }
        t.erase(open, close + 2 - open);
    }
    // [[target|label]] -> label, [[target]] -> target
    for (size_t a; (a = t.find("[[")) != std::string::npos;) {
        const size_t b = t.find("]]", a);
        if (b == std::string::npos) break;
        std::string inner = t.substr(a + 2, b - a - 2);
        const auto bar = inner.rfind('|');
        if (bar != std::string::npos) inner = inner.substr(bar + 1);
        t.replace(a, b + 2 - a, inner);
    }
    std::string out;
    bool prev_space = true;
    for (char c : t) {
        if (c == '\'' || c == '\r') continue;
        if (c == '*' || c == '\n') c = ',';
        if (c == '<') c = ' ';
        const bool space = c == ' ' || c == '\t';
        if (space && prev_space) continue;
        prev_space = space;
        out += c;
    }
    // Collapse the commas the bullets left behind.
    std::string tidy;
    for (char c : out) {
        if (c == ',' && (tidy.empty() || tidy.back() == ',' || tidy.back() == ' ')) {
            if (!tidy.empty() && tidy.back() == ' ' && tidy.size() > 1 && tidy[tidy.size() - 2] != ',') {
                tidy.pop_back();
                tidy += ',';
            }
            continue;
        }
        if (c == ',') {
            tidy += ", ";
            continue;
        }
        if (c == ' ' && !tidy.empty() && tidy.back() == ' ') continue;
        tidy += c;
    }
    while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == ',')) tidy.pop_back();
    return Trim(tidy);
}

std::string GameInfoService::ReleaseDate(std::string_view value) {
    // "{{Video game release|JP|November 3, 2011|NA|...}}", "{{vgrelease|WW|2011-11-03}}", "2011"
    const std::string v(value);
    static const std::regex long_date(R"((January|February|March|April|May|June|July|August|September|October|November|December) \d{1,2}, \d{4})");
    static const std::regex iso(R"((\d{4})-(\d{2})-(\d{2}))");
    static const std::regex year(R"((19|20)\d{2})");
    std::smatch m;
    if (std::regex_search(v, m, long_date)) return m.str(0);
    static const char* months[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
    if (std::regex_search(v, m, iso)) {
        const int mo = std::stoi(m.str(2));
        if (mo >= 1 && mo <= 12) return std::string(months[mo - 1]) + " " + std::to_string(std::stoi(m.str(3))) + ", " + m.str(1);
    }
    if (std::regex_search(v, m, year)) return m.str(0);
    return {};
}

std::vector<std::string> GameInfoService::ParseSearchTitles(std::string_view text) {
    std::vector<std::string> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("query") || !j["query"].contains("search")) return out;
    for (const auto& r : j["query"]["search"])
        if (r.contains("title")) out.push_back(r["title"].get<std::string>());
    return out;
}

std::vector<GameInfoService::Article> GameInfoService::ParseArticles(std::string_view text) {
    std::vector<Article> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("query") || !j["query"].contains("pages") || !j["query"]["pages"].is_array())
        return out;
    for (const auto& p : j["query"]["pages"]) {
        Article a;
        a.title = p.value("title", "");
        a.extract = p.value("extract", "");
        if (p.contains("revisions") && p["revisions"].is_array() && !p["revisions"].empty()) {
            const auto& rev = p["revisions"][0];
            if (rev.contains("slots") && rev["slots"].contains("main")) a.wikitext = rev["slots"]["main"].value("content", "");
        }
        if (!a.title.empty()) out.push_back(a);
    }
    return out;
}

GameInfo GameInfoService::FromArticle(const Article& a) {
    GameInfo info;
    info.found = true;
    info.source = a.title;
    info.summary = Trim(a.extract);
    info.developer = CleanWikiText(InfoboxField(a.wikitext, "developer"));
    info.publisher = CleanWikiText(InfoboxField(a.wikitext, "publisher"));
    info.genre = CleanWikiText(InfoboxField(a.wikitext, "genre"));
    info.released = ReleaseDate(InfoboxField(a.wikitext, "released"));
    if (info.summary.size() > 700) {
        const auto cut = info.summary.rfind(". ", 700);
        info.summary = cut != std::string::npos && cut > 200 ? info.summary.substr(0, cut + 1) : info.summary.substr(0, 700) + "...";
    }
    return info;
}

std::vector<std::string> GameInfoService::ParseListing(std::string_view html) {
    std::vector<std::string> out;
    const std::string h(html);
    static const std::regex href(R"(href="([^"?/][^"]*)\.png")", std::regex::icase);
    for (auto it = std::sregex_iterator(h.begin(), h.end(), href); it != std::sregex_iterator(); ++it)
        out.push_back(UrlDecode((*it)[1].str()));
    return out;
}

std::string GameInfoService::BestSnapName(const std::vector<std::string>& names, const std::string& title) {
    const std::string want = Canonical(title);
    if (want.empty()) return {};
    auto region_rank = [](const std::string& n) {
        if (n.find("(USA") != std::string::npos) return 0;
        if (n.find("(World") != std::string::npos) return 1;
        if (n.find("(Europe") != std::string::npos) return 2;
        return 3;
    };
    std::string best;
    int best_score = -1;
    for (const auto& n : names) {
        const std::string c = Canonical(StripTags(n));
        int score = 0;
        if (c == want) score = 100;
        else if (c.size() > want.size() && c.starts_with(want) && c.size() - want.size() <= 6) score = 60;
        else continue;
        score -= region_rank(n);
        if (n.find("(Beta") != std::string::npos || n.find("(Demo") != std::string::npos || n.find("(Proto") != std::string::npos) score -= 20;
        if (score > best_score) {
            best_score = score;
            best = n;
        }
    }
    return best;
}

const char* GameInfoService::LibretroFolder(GameSystem system) {
    switch (system) {
    case GameSystem::Gba: return "Nintendo - Game Boy Advance";
    case GameSystem::Nes: return "Nintendo - Nintendo Entertainment System";
    default: return "Nintendo - Nintendo 3DS";
    }
}

std::string GameInfoService::Key(const GameEntry& g) {
    if (g.title_id) return g.TitleIdHex();
    return g.SaveKey();
}

GameInfoService::GameInfoService(IHttpClient& http, IFileSystem& fs, std::string cache_dir)
    : http_(http), fs_(fs), dir_(std::move(cache_dir)) {}

std::string GameInfoService::InfoPath(const GameEntry& g) const { return JoinPath(dir_, Key(g) + "_info.json"); }

GameInfo GameInfoService::Cached(const GameEntry& g) const {
    GameInfo info;
    const std::string text = const_cast<IFileSystem&>(fs_).ReadText(InfoPath(g));
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return info;
    info.found = j.value("found", false);
    info.summary = j.value("summary", "");
    info.developer = j.value("developer", "");
    info.publisher = j.value("publisher", "");
    info.released = j.value("released", "");
    info.genre = j.value("genre", "");
    info.source = j.value("source", "");
    if (j.contains("shots"))
        for (const auto& s : j["shots"]) {
            const std::string p = s.get<std::string>();
            if (const_cast<IFileSystem&>(fs_).Exists(p)) info.shots.push_back(p);
        }
    return info;
}

std::vector<std::string> GameInfoService::SnapNames(GameSystem system) {
    {
        std::scoped_lock lock(mutex_);
        if (auto it = listings_.find(static_cast<int>(system)); it != listings_.end()) return it->second;
    }
    const std::string file = JoinPath(dir_, std::string("snaps_") + std::to_string(static_cast<int>(system)) + ".txt");
    std::vector<std::string> names;
    const std::string cached = fs_.ReadText(file);
    if (!cached.empty()) {
        names = Split(cached, '\n');
    } else {
        const HttpResponse r = http_.Get(std::string(kThumbs) + UrlEncode(LibretroFolder(system)) + "/Named_Snaps/",
                                         {{"User-Agent", "ONYX3DS"}});
        if (r.ok()) {
            names = ParseListing(r.body);
            std::string text;
            for (const auto& n : names) text += n + "\n";
            if (!names.empty()) {
                fs_.CreateDirs(dir_);
                fs_.WriteText(file, text);
            }
        }
    }
    std::scoped_lock lock(mutex_);
    listings_[static_cast<int>(system)] = names;
    return names;
}

void GameInfoService::FetchShots(const GameEntry& g, GameInfo& info) {
    const auto names = SnapNames(g.system);
    const std::string name = BestSnapName(names, g.DisplayTitle());
    if (name.empty()) return;
    const std::string base = std::string(kThumbs) + UrlEncode(LibretroFolder(g.system)) + "/";
    for (const char* kind : {"Named_Snaps", "Named_Titles"}) {
        const HttpResponse r = http_.Get(base + kind + "/" + UrlEncode(name) + ".png", {{"User-Agent", "ONYX3DS"}});
        if (!r.ok() || r.body.size() < 8 || r.body.compare(0, 4, "\x89PNG") != 0) continue;
        const std::string path = JoinPath(dir_, Key(g) + "_" + (kind[6] == 'S' ? "snap" : "title") + ".png");
        fs_.CreateDirs(dir_);
        if (fs_.WriteText(path, r.body)) info.shots.push_back(path);
    }
}

GameInfo GameInfoService::Get(const GameEntry& g) {
    {
        const std::string text = fs_.ReadText(InfoPath(g));
        const json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            const u64 when = j.value("fetched", u64{0});
            const bool miss = !j.value("found", false);
            if (!miss || Now() - when < kRetryMissSeconds) return Cached(g);
        }
    }
    GameInfo info;
    const char* hint = g.system == GameSystem::Gba ? "Game Boy Advance video game"
                       : g.system == GameSystem::Nes ? "Nintendo Entertainment System video game"
                                                     : "Nintendo 3DS video game";
    const std::string title = StripTags(g.DisplayTitle());
    bool reachable = false;
    const HttpResponse search = http_.Get(std::string(kApi) + "?action=query&list=search&srlimit=4&format=json&srsearch=" +
                                              UrlEncode(title + " " + hint),
                                          {{"User-Agent", "ONYX3DS"}});
    reachable = search.ok();
    if (search.ok()) {
        const auto titles = ParseSearchTitles(search.body);
        if (!titles.empty()) {
            std::string joined;
            for (const auto& t : titles) joined += (joined.empty() ? "" : "|") + t;
            const HttpResponse art = http_.Get(
                std::string(kApi) + "?action=query&prop=extracts|revisions&exintro=1&explaintext=1&rvprop=content&rvslots=main"
                                    "&redirects=1&format=json&formatversion=2&titles=" + UrlEncode(joined),
                {{"User-Agent", "ONYX3DS"}});
            if (art.ok()) {
                const auto articles = ParseArticles(art.body);
                const std::string want = Canonical(title);
                // In search order, the first article that is a video game and shares the name.
                for (const auto& t : titles)
                    for (const auto& a : articles) {
                        if (a.title != t || a.wikitext.find("nfobox video game") == std::string::npos) continue;
                        const std::string have = Canonical(StripTags(a.title));
                        if (have.find(want) == std::string::npos && want.find(have) == std::string::npos) continue;
                        info = FromArticle(a);
                        goto found;
                    }
            }
        }
    }
found:
    FetchShots(g, info);
    if (!info.shots.empty()) info.found = true;
    if (!reachable) return info; // offline: do not remember the miss
    json j = {{"found", info.found}, {"summary", info.summary}, {"developer", info.developer},
              {"publisher", info.publisher}, {"released", info.released}, {"genre", info.genre},
              {"source", info.source}, {"shots", info.shots}, {"fetched", Now()}};
    fs_.CreateDirs(dir_);
    fs_.WriteText(InfoPath(g), j.dump());
    return info;
}

} // namespace onyx
