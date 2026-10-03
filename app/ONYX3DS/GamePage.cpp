// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "GamePage.h"
#if __has_include("GamePage.g.cpp")
#include "GamePage.g.cpp"
#endif

#include "Emu/EmulatorSession.h"
#include "Platform/Imaging.h"
#include "Platform/Log.h"
#include "Ui/AppServices.h"
#include "Ui/UiKit.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace onyx;
using namespace onyx::app;
namespace kit = onyx::app::ui;

namespace winrt::ONYX3DS::implementation {

namespace {
AppServices& Svc() {
    return AppServices::Get();
}
} // namespace

void GamePage::InitializeComponent() {
    GamePageT::InitializeComponent();
}

void GamePage::OnNavigatedTo(NavigationEventArgs const& e) {
    const std::string path = kit::S(unbox_value_or<hstring>(e.Parameter(), L""));
    if (auto g = Svc().Library().Find(path)) game_ = *g;
    auto weak = get_weak();
    Svc().SetToastSink([weak](const std::string& text) {
        if (auto self = weak.get()) kit::ShowToast(self->ToastHost(), text);
    });
    // B closes an open side panel before leaving the page.
    Svc().SetBackOverride([weak] {
        auto self = weak.get();
        if (self && self->SideScroll().Visibility() == Visibility::Visible) {
            self->CloseSide();
            return true;
        }
        return false;
    });
    Build();

    // First visit: grab cheats for this game in the background.
    if (Svc().Config().services.cheats_auto_download && game_.title_id &&
        !Svc().Fs().Exists(Svc().CheatFilePath(game_.title_id))) {
        Svc().DownloadCheats(game_.title_id, [](int) {});
    }
}

void GamePage::Build() {
    const Theme& t = Svc().CurrentTheme();
    // Backdrop: hero art if we have it, else the grid art, else the theme.
    std::string backdrop = game_.hero_art.empty() ? game_.grid_art : game_.hero_art;
    if (backdrop.empty() || !Svc().Fs().Exists(backdrop)) backdrop = Svc().ThemeAsset(t.textures.background);
    HeroImage().Source(ImageFromFile(backdrop, 1920));
    LinearGradientBrush shade;
    shade.StartPoint(Point{0, 0});
    shade.EndPoint(Point{0, 1});
    GradientStop a, b, c;
    a.Color({0x10, 0, 0, 0});
    a.Offset(0);
    b.Color({0x70, 0, 0, 0});
    b.Offset(0.55);
    c.Color({0xE6, 0, 0, 0});
    c.Offset(1);
    shade.GradientStops().Append(a);
    shade.GradientStops().Append(b);
    shade.GradientStops().Append(c);
    Shade().Fill(shade);
    Root().RequestedTheme(ElementTheme::Dark);

    auto info = InfoPanel();
    info.Children().Clear();
    const std::string white = "#FFFFFF", soft = "#D8DEE6";

    if (!game_.logo_art.empty() && Svc().Fs().Exists(game_.logo_art)) {
        Image logo;
        logo.Source(ImageFromFile(game_.logo_art, 800));
        logo.MaxHeight(220);
        logo.MaxWidth(760);
        logo.HorizontalAlignment(HorizontalAlignment::Left);
        info.Children().Append(logo);
    } else {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(24);
        if (!game_.icon_path.empty()) {
            Border frame;
            frame.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{16, 16, 16, 16});
            frame.Width(120);
            frame.Height(120);
            Image icon;
            icon.Source(IconFromRgba(Svc().Fs().ReadAll(game_.icon_path), 48, 48));
            frame.Child(icon);
            row.Children().Append(frame);
        }
        auto title = kit::Text(game_.DisplayTitle(), 60, true, white);
        title.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(title);
        info.Children().Append(row);
    }
    if (!game_.long_title.empty() && game_.long_title != game_.title)
        info.Children().Append(kit::Text(game_.long_title, 24, false, soft));

    std::string meta;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!meta.empty()) meta += "   ·   ";
        meta += s;
    };
    add(game_.publisher);
    add(game_.region);
    add(game_.product_code);
    add(FormatSize(game_.file_size));
    info.Children().Append(kit::Text(meta, 22, false, soft));
    info.Children().Append(kit::Text(FormatLastPlayed(game_.last_played) + "   ·   Play time " +
                                         FormatPlayTime(game_.play_seconds) + "   ·   Title ID " +
                                         game_.TitleIdHex(),
                                     20, false, soft));
    if (!game_.note.empty()) {
        StackPanel warn;
        warn.Orientation(Orientation::Horizontal);
        warn.Spacing(10);
        warn.Children().Append(kit::Glyph(kit::glyph::Warning, 20, "#FFC857"));
        warn.Children().Append(kit::Text(game_.note, 20, false, "#FFC857"));
        info.Children().Append(warn);
    }
    if (Svc().RetroAchievements().LoggedIn()) {
        info.Children().Append(kit::Text(
            "RetroAchievements: signed in as " + Svc().RetroAchievements().UserName() +
                ". Sets for 3DS games appear here once the site publishes them.",
            18, false, soft));
    }

    // Actions
    StackPanel row1;
    row1.Orientation(Orientation::Horizontal);
    row1.Spacing(16);
    row1.Margin(Thickness{0, 18, 0, 0});
    auto weak = get_weak();
    auto start = kit::ActionButton("Start", kit::glyph::Play, [weak] {
        if (auto self = weak.get()) self->Launch("");
    }, true);
    row1.Children().Append(start);
    EmulatorSession& session = EmulatorSession::Get();
    // Resume is offered when an auto-save exists for this title.
    const std::string auto_state = JoinPath(Paths().local_state, "states/" + game_.TitleIdHex() + "/auto.state");
    if (Svc().Fs().Exists(auto_state)) {
        row1.Children().Append(kit::ActionButton("Resume", kit::glyph::Load, [weak] {
            if (auto self = weak.get()) self->Launch("resume");
        }));
    }
    row1.Children().Append(kit::ActionButton("Save states", kit::glyph::Save, [weak] {
        if (auto self = weak.get()) self->ShowStates();
    }));
    row1.Children().Append(kit::ActionButton("Cheats", kit::glyph::Cheat, [weak] {
        if (auto self = weak.get()) self->ShowCheats();
    }));
    (void)session;
    info.Children().Append(row1);

    StackPanel row2;
    row2.Orientation(Orientation::Horizontal);
    row2.Spacing(16);
    row2.Children().Append(kit::ActionButton("Game settings", kit::glyph::Settings, [weak] {
        if (auto self = weak.get()) self->ShowGameSettings();
    }));
    row2.Children().Append(kit::ActionButton("Get artwork", kit::glyph::Picture, [weak] {
        auto self = weak.get();
        if (!self) return;
        Svc().Toast("Searching SteamGridDB...");
        Svc().ScrapeArt(self->game_, true, [weak](bool ok) {
            if (auto s = weak.get(); s && ok) {
                if (auto g = Svc().Library().Find(s->game_.path)) s->game_ = *g;
                s->Build();
                Svc().Toast("Artwork updated");
            }
        });
    }));
    row2.Children().Append(kit::ActionButton(game_.favorite ? "Unfavourite" : "Favourite",
                                             game_.favorite ? kit::glyph::StarFilled : kit::glyph::Star, [weak] {
        auto self = weak.get();
        if (!self) return;
        self->game_.favorite = !self->game_.favorite;
        Svc().Library().Update(self->game_);
        self->Build();
    }));
    row2.Children().Append(kit::ActionButton("Hide", kit::glyph::Hide, [weak] {
        auto self = weak.get();
        if (!self) return;
        kit::Confirm("Hide " + self->game_.DisplayTitle() + "?",
                     "It disappears from the home menu. Show it again from Settings > Interface.", "Hide",
                     [weak] {
                         if (auto s = weak.get()) {
                             s->game_.hidden = true;
                             Svc().Library().Update(s->game_);
                             s->Frame().GoBack();
                         }
                     });
    }));
    info.Children().Append(row2);
    start.Focus(FocusState::Programmatic);
}

void GamePage::Launch(const std::string& mode) {
    Svc().StopMusic();
    Svc().PlaySfx("launch");
    const std::string param = mode.empty() ? game_.path : game_.path + "\n" + mode;
    Frame().Navigate(xaml_typename<ONYX3DS::EmulationPage>(), box_value(kit::H(param)));
}

void GamePage::CloseSide() {
    SideScroll().Visibility(Visibility::Collapsed);
    SidePanel().Children().Clear();
    Build();
}

void GamePage::ShowStates() {
    auto panel = SidePanel();
    panel.Children().Clear();
    panel.Children().Append(kit::SectionHeader("Save states"));
    panel.Children().Append(kit::Text(
        "In game: View + D-pad Up saves, View + D-pad Down loads, View + Left/Right picks the slot.", 18,
        false, "#D8DEE6"));
    auto weak = get_weak();
    bool any = false;
    for (int slot = 0; slot <= 9; ++slot) {
        const std::string dir = JoinPath(Paths().local_state, "states/" + game_.TitleIdHex());
        const std::string file = JoinPath(dir, slot == 0 ? "auto.state" : "slot" + std::to_string(slot) + ".state");
        if (!Svc().Fs().Exists(file)) continue;
        any = true;
        const std::string thumb = file.substr(0, file.size() - 6) + ".png";
        Button b;
        Grid g;
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::Auto());
        c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        g.ColumnDefinitions().Append(c0);
        g.ColumnDefinitions().Append(c1);
        Image img;
        img.Width(200);
        img.Height(150);
        img.Stretch(Stretch::Uniform);
        if (Svc().Fs().Exists(thumb)) img.Source(ImageFromFile(thumb, 200));
        g.Children().Append(img);
        auto label = kit::Text(slot == 0 ? "Auto save (when you last quit)" : "Slot " + std::to_string(slot), 24, true);
        label.VerticalAlignment(VerticalAlignment::Center);
        label.Margin(Thickness{20, 0, 0, 0});
        Grid::SetColumn(label, 1);
        g.Children().Append(label);
        b.Content(g);
        b.HorizontalAlignment(HorizontalAlignment::Stretch);
        b.HorizontalContentAlignment(HorizontalAlignment::Left);
        b.Click([weak, slot](auto&&, auto&&) {
            if (auto self = weak.get()) self->Launch(slot == 0 ? "resume" : "slot:" + std::to_string(slot));
        });
        panel.Children().Append(b);
    }
    if (!any) panel.Children().Append(kit::Text("No save states yet.", 22, false, "#D8DEE6"));
    SideScroll().Visibility(Visibility::Visible);
    if (panel.Children().Size() > 2)
        if (auto c = panel.Children().GetAt(2).try_as<Control>()) c.Focus(FocusState::Programmatic);
}

void GamePage::ShowCheats() {
    auto panel = SidePanel();
    panel.Children().Clear();
    const std::string file = Svc().CheatFilePath(game_.title_id);
    cheats_ = CheatFile::Parse(Svc().Fs().ReadText(file));
    panel.Children().Append(kit::SectionHeader("Cheats"));
    auto weak = get_weak();
    auto download = kit::ActionButton("Download from cheat database", kit::glyph::Download, [weak] {
        auto self = weak.get();
        if (!self) return;
        Svc().Toast("Checking the cheat database...");
        Svc().DownloadCheats(self->game_.title_id, [weak](int added) {
            auto s = weak.get();
            if (!s) return;
            Svc().Toast(added < 0   ? "No cheats for this game in the database (or you're offline)"
                        : added == 0 ? "You already have every cheat from the database"
                                     : "Added " + std::to_string(added) + " cheats");
            s->ShowCheats();
        });
    });
    panel.Children().Append(download);
    if (cheats_.cheats.empty()) {
        panel.Children().Append(kit::Text(
            "No cheats yet. Download them above, or put a " + game_.TitleIdHex() +
                ".txt file (Gateway / Action Replay format) in your Cheats folder.",
            20, false, "#D8DEE6"));
    }
    for (size_t i = 0; i < cheats_.cheats.size(); ++i) {
        const auto& c = cheats_.cheats[i];
        auto toggle = kit::Toggle(c.enabled, [weak, i, file](bool on) {
            auto self = weak.get();
            if (!self || i >= self->cheats_.cheats.size()) return;
            self->cheats_.cheats[i].enabled = on;
            Svc().Fs().WriteText(file, self->cheats_.Serialize());
        });
        panel.Children().Append(kit::SettingRow(c.name, c.comments.empty() ? "" : c.comments.front(), toggle));
    }
    panel.Children().Append(kit::Text(
        "Cheats can crash or corrupt saves. Make a save state first. You can also switch them on and off "
        "in game from the ONYX menu (View + Menu).",
        16, false, "#AAB4C0"));
    SideScroll().Visibility(Visibility::Visible);
    download.Focus(FocusState::Programmatic);
}

void GamePage::ShowGameSettings() {
    auto panel = SidePanel();
    panel.Children().Clear();
    panel.Children().Append(kit::SectionHeader("Settings for this game"));
    panel.Children().Append(kit::Text("Overrides your global settings for " + game_.DisplayTitle() + " only.",
                                      18, false, "#D8DEE6"));
    const std::string tid = game_.TitleIdHex();
    auto& cfg = Svc().Config();
    const CoreOptions global = cfg.EffectiveCoreOptions(Svc().Model(), "");
    const CoreOptionCatalog catalog = Svc().Catalog();

    struct Pick {
        const char* key;
        const char* label;
        std::vector<std::pair<std::string, std::string>> values;
    };
    std::vector<Pick> picks = {
        {keys::kResolution, "Internal resolution",
         {{"1", "1x native (fastest)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"}}},
        {keys::kCpuClock, "CPU clock",
         {{"50", "50%"}, {"75", "75%"}, {"100", "100% (default)"}, {"125", "125%"}, {"150", "150%"},
          {"200", "200%"}}},
        {keys::kAccurateMul, "Accurate shader multiplication",
         {{"enabled", "On (fixes some graphics)"}, {"disabled", "Off (faster)"}}},
        {keys::kLayout, "Screen layout",
         {{"large_screen", "Big top screen"}, {"default", "Stacked"}, {"side_by_side", "Side by side"},
          {"single_screen", "Single screen"}}},
        {keys::kNew3ds, "System model", {{"New 3DS", "New 3DS"}, {"Old 3DS", "Original 3DS"}}},
        {keys::kTextureFilter, "Texture filter",
         {{"none", "None"}, {"Anime4K Ultrafast", "Anime4K Ultrafast"}, {"Bicubic", "Bicubic"},
          {"ScaleForce", "ScaleForce"}, {"xBRZ", "xBRZ"}, {"MMPX", "MMPX"}}},
        {keys::kCustomTextures, "Custom textures", {{"enabled", "On"}, {"disabled", "Off"}}},
    };
    for (auto& p : picks) {
        // Prefer the core's own value list when we have it.
        if (const CoreOptionDef* def = catalog.Find(p.key)) {
            p.values.clear();
            for (const auto& v : def->values) p.values.emplace_back(v.value, v.label);
        }
        std::vector<std::pair<std::string, std::string>> options;
        const auto git = global.find(p.key);
        options.emplace_back("", "Use global setting (" + (git == global.end() ? std::string("default") : git->second) + ")");
        for (auto& v : p.values) options.push_back(v);
        std::string current;
        if (auto it = cfg.per_game.find(tid); it != cfg.per_game.end())
            if (auto v = it->second.find(p.key); v != it->second.end()) current = v->second;
        const std::string key = p.key;
        auto combo = kit::Choice(options, current, [tid, key](const std::string& value) {
            auto& c = Svc().Config();
            if (value.empty()) {
                c.per_game[tid].erase(key);
                if (c.per_game[tid].empty()) c.per_game.erase(tid);
            } else {
                c.per_game[tid][key] = value;
            }
            Svc().SaveSettings();
        });
        panel.Children().Append(kit::SettingRow(p.label, "", combo));
    }
    SideScroll().Visibility(Visibility::Visible);
    if (panel.Children().Size() > 2) {
        if (auto row = panel.Children().GetAt(2).try_as<Grid>(); row && row.Children().Size() > 1)
            if (auto c = row.Children().GetAt(1).try_as<Control>()) c.Focus(FocusState::Programmatic);
    }
}

} // namespace winrt::ONYX3DS::implementation
