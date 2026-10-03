// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/theme.h"

#include <nlohmann/json.hpp>

namespace onyx {

using nlohmann::json;

namespace {

bool IsColor(const std::string& s) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

void Str(const json& j, const char* k, std::string& out) {
    if (auto it = j.find(k); it != j.end() && it->is_string()) out = it->get<std::string>();
}
void Color(const json& j, const char* k, std::string& out) {
    std::string v;
    Str(j, k, v);
    if (IsColor(v)) out = v; // a typo keeps the default instead of a broken UI
}
void Num(const json& j, const char* k, double& out) {
    if (auto it = j.find(k); it != j.end() && it->is_number()) out = it->get<double>();
}

} // namespace

std::string Theme::Resolve(const std::string& relative) const {
    if (relative.empty()) return {};
    // Absolute already ("E:/..." or "/...")?
    if (relative.size() > 1 && (relative[1] == ':' || relative[0] == '/')) return relative;
    return JoinPath(folder, relative);
}

std::optional<Theme> Theme::Parse(std::string_view text, const std::string& folder) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    Theme t;
    t.folder = NormalizeSlashes(folder);
    Str(j, "id", t.id);
    Str(j, "name", t.name);
    Str(j, "author", t.author);
    Str(j, "description", t.description);
    if (t.id.empty()) t.id = ToLower(FileName(folder));
    if (t.name.empty()) t.name = t.id;

    if (auto c = j.find("colors"); c != j.end() && c->is_object()) {
        auto& o = t.colors;
        Color(*c, "background_top", o.background_top);
        Color(*c, "background_bottom", o.background_bottom);
        Color(*c, "accent", o.accent);
        Color(*c, "accent_text", o.accent_text);
        Color(*c, "text", o.text);
        Color(*c, "text_muted", o.text_muted);
        Color(*c, "tile_face", o.tile_face);
        Color(*c, "tile_edge", o.tile_edge);
        Color(*c, "tile_glow", o.tile_glow);
        Color(*c, "bar", o.bar);
        Color(*c, "bar_text", o.bar_text);
        Color(*c, "panel", o.panel);
    }
    if (auto x = j.find("textures"); x != j.end() && x->is_object()) {
        auto& o = t.textures;
        Str(*x, "background", o.background);
        Str(*x, "background_far", o.background_far);
        Str(*x, "background_near", o.background_near);
        Str(*x, "tile_frame", o.tile_frame);
        Str(*x, "tile_gloss", o.tile_gloss);
        Str(*x, "tile_empty", o.tile_empty);
        Str(*x, "bar", o.bar);
        Str(*x, "button", o.button);
        Str(*x, "cursor", o.cursor);
    }
    if (auto a = j.find("audio"); a != j.end() && a->is_object()) {
        auto& o = t.audio;
        Str(*a, "music", o.music);
        Str(*a, "move", o.move);
        Str(*a, "select", o.select);
        Str(*a, "back", o.back);
        Str(*a, "launch", o.launch);
    }
    if (auto s = j.find("style"); s != j.end() && s->is_object()) {
        auto& o = t.style;
        Num(*s, "tile_corner_radius", o.tile_corner_radius);
        Num(*s, "tile_depth", o.tile_depth);
        Num(*s, "tile_tilt", o.tile_tilt);
        Num(*s, "parallax", o.parallax);
        Num(*s, "hover_scale", o.hover_scale);
        if (auto it = s->find("empty_slots"); it != s->end() && it->is_number_integer())
            o.empty_slots = std::clamp(it->get<int>(), 0, 48);
        if (auto it = s->find("show_clock"); it != s->end() && it->is_boolean())
            o.show_clock = it->get<bool>();
        Str(*s, "font", o.font);
    }
    return t;
}

std::string Theme::ToJson() const {
    json j = {
        {"id", id}, {"name", name}, {"author", author}, {"description", description},
        {"colors",
         {{"background_top", colors.background_top}, {"background_bottom", colors.background_bottom},
          {"accent", colors.accent}, {"accent_text", colors.accent_text}, {"text", colors.text},
          {"text_muted", colors.text_muted}, {"tile_face", colors.tile_face},
          {"tile_edge", colors.tile_edge}, {"tile_glow", colors.tile_glow}, {"bar", colors.bar},
          {"bar_text", colors.bar_text}, {"panel", colors.panel}}},
        {"textures",
         {{"background", textures.background}, {"background_far", textures.background_far},
          {"background_near", textures.background_near}, {"tile_frame", textures.tile_frame},
          {"tile_gloss", textures.tile_gloss}, {"tile_empty", textures.tile_empty},
          {"bar", textures.bar}, {"button", textures.button}, {"cursor", textures.cursor}}},
        {"audio",
         {{"music", audio.music}, {"move", audio.move}, {"select", audio.select},
          {"back", audio.back}, {"launch", audio.launch}}},
        {"style",
         {{"tile_corner_radius", style.tile_corner_radius}, {"tile_depth", style.tile_depth},
          {"tile_tilt", style.tile_tilt}, {"parallax", style.parallax},
          {"hover_scale", style.hover_scale}, {"empty_slots", style.empty_slots},
          {"show_clock", style.show_clock}, {"font", style.font}}},
    };
    return j.dump(2);
}

std::vector<Theme> LoadThemes(IFileSystem& fs, const std::string& builtin_root,
                              const std::string& user_root) {
    std::vector<Theme> out;
    auto scan = [&](const std::string& root, bool built_in) {
        if (root.empty() || !fs.IsDirectory(root)) return;
        for (const auto& e : fs.List(root)) {
            if (!e.is_dir) continue;
            const std::string dir = JoinPath(root, e.name);
            const std::string text = fs.ReadText(JoinPath(dir, "theme.json"));
            auto theme = Theme::Parse(text, dir);
            if (!theme) continue;
            theme->built_in = built_in;
            std::erase_if(out, [&](const Theme& t) { return t.id == theme->id; });
            out.push_back(std::move(*theme));
        }
    };
    scan(builtin_root, true);
    scan(user_root, false);
    std::stable_sort(out.begin(), out.end(), [](const Theme& a, const Theme& b) {
        if (a.built_in != b.built_in) return a.built_in; // built-ins first
        return ToLower(a.name) < ToLower(b.name);
    });
    return out;
}

const Theme* FindTheme(const std::vector<Theme>& themes, const std::string& id) {
    for (const auto& t : themes)
        if (t.id == id) return &t;
    return themes.empty() ? nullptr : &themes.front();
}

} // namespace onyx
