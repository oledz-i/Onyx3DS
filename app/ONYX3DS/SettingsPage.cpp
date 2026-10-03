// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "SettingsPage.h"
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
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
Settings& Cfg() {
    return AppServices::Get().Config();
}
void Save() {
    AppServices::Get().SaveSettings();
}

struct Section {
    const char* id;
    const char* label;
    const wchar_t* glyph;
};
const Section kSections[] = {
    {"folders", "Folders", kit::glyph::Folder},
    {"emulation", "Emulation", kit::glyph::Speed},
    {"controls", "Screen & controls", kit::glyph::Gamepad},
    {"interface", "Interface & themes", kit::glyph::Palette},
    {"updates", "Updates & DLC", kit::glyph::Package},
    {"services", "Online services", kit::glyph::Globe},
    {"system", "System check", kit::glyph::Info},
    {"about", "About", kit::glyph::Star},
};

// Curated emulation options with friendly names. Values come from the core
// when it has announced its catalogue; these lists are the fallback.
struct Curated {
    const char* key;
    const char* label;
    const char* help;
    std::vector<std::pair<std::string, std::string>> values;
};
std::vector<Curated> CuratedOptions() {
    return {
        {keys::kResolution, "Internal resolution",
         "How sharp 3D graphics are. Each step costs GPU time.",
         {{"1", "1x native 400x240"}, {"2", "2x 800x480"}, {"3", "3x 1200x720"}, {"4", "4x 1600x960"},
          {"5", "5x"}, {"6", "6x"}}},
        {keys::kAccurateMul, "Accurate shader multiplication",
         "Fixes rare graphics glitches (e.g. some Pokemon effects). Slower.",
         {{"enabled", "On"}, {"disabled", "Off"}}},
        {keys::kCpuClock, "Emulated CPU clock",
         "Above 100% can smooth out games that slow down on a real 3DS.",
         {{"50", "50%"}, {"75", "75%"}, {"100", "100%"}, {"125", "125%"}, {"150", "150%"}, {"200", "200%"}}},
        {keys::kNew3ds, "System model", "New 3DS gives games more memory and CPU. Some need it.",
         {{"New 3DS", "New 3DS"}, {"Old 3DS", "Original 3DS"}}},
        {keys::kRegion, "System region", "Auto picks the game's region.",
         {{"Auto", "Auto"}, {"Japan", "Japan"}, {"USA", "USA"}, {"Europe", "Europe"},
          {"Australia", "Australia"}, {"China", "China"}, {"Korea", "Korea"}}},
        {keys::kLanguage, "System language", "Language multi-language games start in.",
         {{"English", "English"}, {"Japanese", "Japanese"}, {"French", "French"}, {"Spanish", "Spanish"},
          {"German", "German"}, {"Italian", "Italian"}, {"Dutch", "Dutch"}}},
        {keys::kHwShader, "Hardware shaders", "Runs 3DS shaders on the GPU. Keep on.",
         {{"enabled", "On"}, {"disabled", "Off"}}},
        {keys::kShaderJit, "Shader JIT", "Faster CPU-side shader fallback.",
         {{"enabled", "On"}, {"disabled", "Off"}}},
        {keys::kDiskShaderCache, "Shader cache", "Stores compiled shaders so games stutter less next time.",
         {{"enabled", "On"}, {"disabled", "Off"}}},
        {keys::kTextureFilter, "Texture filter", "Upscales 2D textures. xBRZ/MMPX suit pixel art.",
         {{"none", "None"}, {"Anime4K Ultrafast", "Anime4K Ultrafast"}, {"Bicubic", "Bicubic"},
          {"ScaleForce", "ScaleForce"}, {"xBRZ", "xBRZ"}, {"MMPX", "MMPX"}}},
        {keys::kTextureSampling, "Texture sampling", "Game controlled is what the game asked for.",
         {{"GameControlled", "Game controlled"}, {"NearestNeighbor", "Nearest"}, {"Linear", "Linear"}}},
        {keys::kCustomTextures, "Custom textures", "Load texture packs from your Textures folder.",
         {{"enabled", "On"}, {"disabled", "Off"}}},
        {keys::kDumpTextures, "Dump textures", "For texture pack makers. Slows things down.",
         {{"enabled", "On"}, {"disabled", "Off"}}},
        {keys::kAudioEmulation, "Audio emulation", "HLE is fast; LLE is accurate but much slower.",
         {{"hle", "HLE (fast)"}, {"lle", "LLE (accurate)"}, {"lle_multithread", "LLE multithreaded"}}},
    };
}

std::string ValueOf(const std::string& key) {
    const auto o = Cfg().EffectiveCoreOptions(Svc().Model(), "");
    auto it = o.find(key);
    if (it != o.end()) return it->second;
    if (const CoreOptionDef* d = Svc().Catalog().Find(key)) return d->default_value;
    return {};
}
} // namespace

void SettingsPage::InitializeComponent() {
    SettingsPageT::InitializeComponent();
    for (const auto& s : kSections) {
        ListViewItem item;
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(16);
        row.Children().Append(kit::Glyph(s.glyph, 24));
        row.Children().Append(kit::Text(s.label, 24, true));
        item.Content(row);
        item.Tag(box_value(kit::H(s.id)));
        item.Padding(Thickness{16, 14, 16, 14});
        SectionList().Items().Append(item);
    }
    SectionList().SelectionChanged([weak = get_weak()](auto&&, auto&&) {
        auto self = weak.get();
        if (!self) return;
        if (auto item = self->SectionList().SelectedItem().try_as<ListViewItem>()) {
            const std::string id = kit::S(unbox_value<hstring>(item.Tag()));
            if (id != self->section_) {
                Svc().PlaySfx("move");
                self->Show(id);
            }
        }
    });
}

void SettingsPage::ApplyPageTheme() {
    const Theme& t = Svc().CurrentTheme();
    Root().Background(Svc().ThemeBrush(t.colors.background_bottom));
    BackgroundImage().Source(ImageFromFile(Svc().ThemeAsset(t.textures.background)));
    const auto c = Svc().ThemeColor(t.colors.background_top);
    Root().RequestedTheme((0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B) < 128 ? ElementTheme::Dark
                                                                             : ElementTheme::Light);
    PageTitle().Foreground(Svc().ThemeBrush(t.colors.text));
    ContentCard().Background(Svc().ThemeBrush(t.colors.panel));
}

void SettingsPage::OnNavigatedTo(NavigationEventArgs const& e) {
    ApplyPageTheme();
    auto weak = get_weak();
    Svc().SetToastSink([weak](const std::string& text) {
        if (auto self = weak.get()) kit::ShowToast(self->ToastHost(), text);
    });
    Svc().SetBackOverride(nullptr);

    const std::string wanted = kit::S(unbox_value_or<hstring>(e.Parameter(), L""));
    if (!wanted.empty()) section_ = wanted;
    for (uint32_t i = 0; i < SectionList().Items().Size(); ++i) {
        auto item = SectionList().Items().GetAt(i).as<ListViewItem>();
        if (kit::S(unbox_value<hstring>(item.Tag())) == section_) {
            SectionList().SelectedIndex(static_cast<int>(i));
            item.Focus(FocusState::Programmatic);
        }
    }
    Show(section_);
}

void SettingsPage::OnNavigatedFrom(NavigationEventArgs const&) {
    Save();
    if (library_dirty_) {
        library_dirty_ = false;
        Svc().RescanLibrary(nullptr);
    }
}

void SettingsPage::Add(UIElement const& e) {
    ContentPanel().Children().Append(e);
}

void SettingsPage::Note(const std::string& text) {
    auto t = kit::Text(text, 18, false, Svc().CurrentTheme().colors.text_muted);
    t.Margin(Thickness{0, 4, 0, 12});
    Add(t);
}

void SettingsPage::Show(const std::string& section) {
    section_ = section;
    ContentPanel().Children().Clear();
    ContentScroll().ChangeView(nullptr, 0.0, nullptr, true);
    if (section == "folders") BuildFolders();
    else if (section == "emulation") BuildEmulation();
    else if (section == "controls") BuildControls();
    else if (section == "interface") BuildInterface();
    else if (section == "updates") BuildUpdates();
    else if (section == "services") BuildServices();
    else if (section == "system") BuildSystem();
    else BuildAbout();
}

// ---------------------------------------------------------------------------
// Folders

void SettingsPage::BuildFolders() {
    auto weak = get_weak();
    Add(kit::SectionHeader("Folders"));
    Note("Point ONYX 3DS at your USB drive. Updates, DLC, texture packs, mods and cheats are read "
         "straight from these folders, so nothing has to be copied onto the console.");

    const auto drives = RemovableDriveRoots();
    for (const auto& d : drives) {
        Add(kit::SettingRow("Set up " + d + " for ONYX 3DS",
                            "Creates " + d + "/ONYX3DS with Roms, Updates & DLC, Textures, Mods, Cheats, "
                            "System, Screenshots, Music and Themes, and fills any empty folder below.",
                            kit::ActionButton("Set up", kit::glyph::Usb, [weak, d] {
                                Cfg().folders.ApplyDriveLayout(d);
                                for (const auto& sub : FolderConfig::DriveLayoutSubfolders())
                                    Svc().Fs().CreateDirs(JoinPath(JoinPath(d, "ONYX3DS"), sub));
                                Save();
                                if (auto s = weak.get()) {
                                    s->library_dirty_ = true;
                                    s->Show("folders");
                                }
                                Svc().Toast("Folders created on " + d);
                            })));
    }
    if (drives.empty()) Note("No USB drive detected. Plug one in and come back here.");

    Add(kit::SectionHeader("Games"));
    for (const auto& dir : Cfg().folders.roms) {
        Add(kit::SettingRow(dir, "Scanned with its subfolders",
                            kit::ActionButton("Remove", kit::glyph::Hide, [weak, dir] {
                                auto& roms = Cfg().folders.roms;
                                roms.erase(std::remove(roms.begin(), roms.end(), dir), roms.end());
                                Save();
                                if (auto s = weak.get()) {
                                    s->library_dirty_ = true;
                                    s->Show("folders");
                                }
                            })));
    }
    Add(kit::SettingRow("Add a games folder", FolderKindHelp(FolderKind::Roms),
                        kit::ActionButton("Add", kit::glyph::Folder, [weak] {
                            if (auto s = weak.get())
                                s->Frame().Navigate(xaml_typename<ONYX3DS::FolderPage>(), box_value(L"roms"));
                        })));
    Add(kit::SettingRow("Rescan now", "Picks up games you just copied over",
                        kit::ActionButton("Rescan", kit::glyph::Refresh, [] {
                            Svc().Toast("Scanning...");
                            Svc().RescanLibrary([](size_t n) {
                                Svc().Toast("Library updated: " + std::to_string(n) + " files");
                            });
                        })));

    Add(kit::SectionHeader("Content folders"));
    for (int k = 1; k < static_cast<int>(FolderKind::Count); ++k) {
        const auto kind = static_cast<FolderKind>(k);
        const std::string current = Cfg().folders.Get(kind);
        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Spacing(12);
        buttons.Children().Append(kit::ActionButton("Change", kit::glyph::Folder, [weak, kind] {
            if (auto s = weak.get())
                s->Frame().Navigate(xaml_typename<ONYX3DS::FolderPage>(), box_value(kit::H(FolderKindKey(kind))));
        }));
        if (!current.empty()) {
            buttons.Children().Append(kit::ActionButton("Clear", nullptr, [weak, kind] {
                Cfg().folders.Set(kind, "");
                Save();
                EmulatorSession::Get().UpdateFolders(Cfg().folders);
                if (auto s = weak.get()) s->Show("folders");
            }));
        }
        Add(kit::SettingRow(FolderKindLabel(kind),
                            std::string(FolderKindHelp(kind)) + "\n" + (current.empty() ? "Not set" : current),
                            buttons));
    }
}

// ---------------------------------------------------------------------------
// Emulation

void SettingsPage::BuildEmulation() {
    auto weak = get_weak();
    Add(kit::SectionHeader("Performance profile"));
    const ConsoleModel model = Svc().Model();
    const std::string auto_label =
        std::string("Auto (") + ConsoleModelName(model) + ": " + PerfProfileName(ResolveAuto(model)) + ")";
    auto profile = kit::Choice({{"auto", auto_label},
                                {"series_s", "Series S: 2x resolution, fast shaders"},
                                {"series_x", "Series X: 4x resolution, accurate shaders"},
                                {"xbox_one", "Xbox One: native resolution"},
                                {"custom", "Custom: I'll choose everything"}},
                               PerfProfileName(Cfg().profile), [weak](const std::string& v) {
                                   Cfg().profile = PerfProfileFromName(v);
                                   Save();
                                   if (auto s = weak.get()) s->Show("emulation");
                               });
    Add(kit::SettingRow("Profile", "Sets resolution and the costly accuracy options for your console. "
                                   "Games aim for a steady 30-60 FPS on Series S with the default.",
                        profile));
    const PerfProfile effective = Svc().EffectiveProfile();
    const CoreOptions owned = ProfileOptions(effective);
    if (effective != PerfProfile::Custom)
        Note("Greyed-out settings are set by the profile. Pick Custom to change them, or override them per "
             "game from the game's channel page.");

    Add(kit::SectionHeader("Emulation"));
    const CoreOptionCatalog catalog = Svc().Catalog();
    for (auto c : CuratedOptions()) {
        if (const CoreOptionDef* def = catalog.Find(c.key); def && !def->values.empty()) {
            c.values.clear();
            for (const auto& v : def->values) c.values.emplace_back(v.value, v.label);
        }
        const std::string key = c.key;
        auto combo = kit::Choice(c.values, ValueOf(key), [key](const std::string& v) {
            Cfg().core[key] = v;
            Save();
        });
        if (effective != PerfProfile::Custom && owned.count(key)) combo.IsEnabled(false);
        Add(kit::SettingRow(c.label, c.help, combo));
    }

    Add(kit::SectionHeader("All emulator options"));
    if (catalog.options.empty()) {
        Note("The full list from the Azahar core appears here after you start any game once.");
        return;
    }
    Note("Everything the Azahar core exposes, grouped as the core groups it. The renderer is fixed to "
         "Vulkan because that is what runs on Xbox.");
    for (const auto& cat : catalog.categories) {
        bool header = false;
        for (const auto& def : catalog.options) {
            if (def.category != cat.key || def.key == keys::kGraphicsApi) continue;
            if (!header) {
                auto h = kit::Text(cat.label, 24, true);
                h.Margin(Thickness{0, 16, 0, 4});
                Add(h);
                header = true;
            }
            std::vector<std::pair<std::string, std::string>> values;
            for (const auto& v : def.values) values.emplace_back(v.value, v.label);
            const std::string key = def.key;
            auto combo = kit::Choice(values, catalog.Value(Cfg().EffectiveCoreOptions(Svc().Model(), ""), key),
                                     [key](const std::string& v) {
                                         Cfg().core[key] = v;
                                         Save();
                                     });
            if (effective != PerfProfile::Custom && owned.count(key)) combo.IsEnabled(false);
            Add(kit::SettingRow(def.label, def.info, combo));
        }
    }
}

// ---------------------------------------------------------------------------
// Screen & controls

void SettingsPage::BuildControls() {
    Add(kit::SectionHeader("Screens"));
    auto core_choice = [&](const char* key, const char* label, const char* help,
                           std::vector<std::pair<std::string, std::string>> fallback) {
        if (const CoreOptionDef* def = Svc().Catalog().Find(key); def && !def->values.empty()) {
            fallback.clear();
            for (const auto& v : def->values) fallback.emplace_back(v.value, v.label);
        }
        const std::string k = key;
        Add(kit::SettingRow(label, help, kit::Choice(fallback, ValueOf(k), [k](const std::string& v) {
                                Cfg().core[k] = v;
                                Save();
                            })));
    };
    core_choice(keys::kLayout, "Layout", "How the two 3DS screens share your TV. View + LB cycles in game.",
                {{"large_screen", "Big top screen"}, {"default", "Stacked"}, {"side_by_side", "Side by side"},
                 {"single_screen", "Single screen"}});
    core_choice(keys::kLargeScreenProportion, "Big screen size", "For the big-top-screen layout",
                {{"2.25", "2.25x"}, {"3.00", "3.00x"}, {"4.00", "4.00x"}});
    core_choice(keys::kSwapScreen, "Starting screen", "Which screen is the big one",
                {{"Top", "Top screen"}, {"Bottom", "Bottom screen"}});
    core_choice(keys::kSwapScreenMode, "Swap screens button (L3)", "Toggle, or hold to peek",
                {{"Toggle", "Toggle"}, {"Hold", "Hold"}});
    core_choice(keys::kRender3d, "Stereoscopic 3D", "Off for TVs; side-by-side for 3D TVs and headsets",
                {{"off", "Off (2D)"}, {"side-by-side", "Side by side"}, {"anaglyph", "Anaglyph red/cyan"}});
    const auto filter = Cfg().qol.screen_filter;
    Add(kit::SettingRow("Upscaling look", "How the 3DS picture is stretched to your TV",
                        kit::Choice({{"sharp", "Sharp pixels"}, {"smooth", "Smooth"}, {"crt", "CRT scanlines"}},
                                    filter == ScreenFilter::Sharp ? "sharp"
                                    : filter == ScreenFilter::Crt ? "crt"
                                                                  : "smooth",
                                    [](const std::string& v) {
                                        Cfg().qol.screen_filter = v == "sharp" ? ScreenFilter::Sharp
                                                                  : v == "crt" ? ScreenFilter::Crt
                                                                               : ScreenFilter::Smooth;
                                        Save();
                                    })));

    Add(kit::SectionHeader("Controller"));
    Add(kit::SettingRow("Button layout",
                        "Positional keeps Nintendo's button positions (Xbox B = 3DS A). Labels makes Xbox A = 3DS A.",
                        kit::Choice({{"pos", "Positional (recommended)"}, {"labels", "Match labels"}},
                                    Cfg().qol.swap_face_buttons ? "labels" : "pos", [](const std::string& v) {
                                        Cfg().qol.swap_face_buttons = v == "labels";
                                        Save();
                                    })));
    Add(kit::SettingRow("Stick deadzone", "Raise it if a worn stick drifts",
                        kit::Range(0, 40, 1, Cfg().qol.stick_deadzone, [](double v) {
                            Cfg().qol.stick_deadzone = static_cast<int>(v);
                            Save();
                        })));
    core_choice(keys::kAnalogFunction, "Right stick", "C-Stick for New 3DS games, or a touch-screen cursor (R3 taps)",
                {{"c_stick_and_touchscreen", "C-Stick + touch cursor"}, {"touchscreen_pointer", "Touch cursor only"},
                 {"c_stick", "C-Stick only"}});
    Add(kit::SettingRow("Fast forward", "Hold View + RB, or tap it to toggle",
                        kit::Choice({{"hold", "Hold"}, {"toggle", "Toggle"}},
                                    Cfg().qol.fast_forward_mode == FastForwardMode::Toggle ? "toggle" : "hold",
                                    [](const std::string& v) {
                                        Cfg().qol.fast_forward_mode =
                                            v == "toggle" ? FastForwardMode::Toggle : FastForwardMode::Hold;
                                        Save();
                                    })));
    Add(kit::SettingRow("Fast forward speed", "0 means as fast as the console can go",
                        kit::Choice({{"200", "2x"}, {"300", "3x"}, {"400", "4x"}, {"0", "Unlimited"}},
                                    std::to_string(Cfg().qol.fast_forward_speed), [](const std::string& v) {
                                        Cfg().qol.fast_forward_speed = std::atoi(v.c_str());
                                        Save();
                                    })));

    Add(kit::SectionHeader("In-game shortcuts (hold View)"));
    for (const char* line : {"View + Menu: ONYX menu (save/load, cheats, settings, quit)",
                             "View + RB: fast forward", "View + LB: next screen layout", "View + Y: screenshot",
                             "View + A: show/hide FPS", "View + D-pad Up / Down: save / load state",
                             "View + D-pad Left / Right: change state slot",
                             "Tap View on its own: 3DS Select    L3: swap screens    R3: touch"})
        Note(line);
}

// ---------------------------------------------------------------------------
// Interface

void SettingsPage::BuildInterface() {
    auto weak = get_weak();
    Add(kit::SectionHeader("Theme"));
    std::vector<std::pair<std::string, std::string>> themes;
    for (const auto& t : Svc().Themes()) themes.emplace_back(t.id, t.name + (t.built_in ? "" : " (USB)"));
    Add(kit::SettingRow("Home menu theme", Svc().CurrentTheme().description,
                        kit::Choice(themes, Svc().CurrentTheme().id, [weak](const std::string& id) {
                            Svc().SetTheme(id);
                            if (auto s = weak.get()) {
                                s->ApplyPageTheme();
                                s->Show("interface");
                            }
                        })));
    Add(kit::SettingRow("Reload themes", "After copying a theme folder into Themes on your USB drive",
                        kit::ActionButton("Reload", kit::glyph::Refresh, [weak] {
                            Svc().ReloadThemes();
                            Svc().Toast(std::to_string(Svc().Themes().size()) + " themes available");
                            if (auto s = weak.get()) s->Show("interface");
                        })));
    Add(kit::SettingRow("Background motion", "Parallax light layers that drift behind the channels",
                        kit::Toggle(Cfg().qol.background_parallax, [](bool on) {
                            Cfg().qol.background_parallax = on;
                            Save();
                        })));
    Add(kit::SettingRow("Channels per row", "3 to 6",
                        kit::Range(3, 6, 1, Cfg().qol.grid_columns, [](double v) {
                            Cfg().qol.grid_columns = static_cast<int>(v);
                            Save();
                        })));

    Add(kit::SectionHeader("Sound"));
    Add(kit::SettingRow("Menu music", "Plays the theme's track, or your own from the Music folder",
                        kit::Toggle(Cfg().qol.menu_music, [](bool on) {
                            Cfg().qol.menu_music = on;
                            Save();
                            if (on) Svc().StartMusic();
                            else Svc().StopMusic(false);
                        })));
    Add(kit::SettingRow("Music volume", "",
                        kit::Range(0, 100, 5, Cfg().qol.music_volume, [](double v) {
                            Cfg().qol.music_volume = static_cast<int>(v);
                            Svc().ApplyAudioSettings();
                            Save();
                        })));
    Add(kit::SettingRow("Menu sounds", "Clicks and chimes when you move around",
                        kit::Toggle(Cfg().qol.menu_sounds, [](bool on) {
                            Cfg().qol.menu_sounds = on;
                            Svc().ApplyAudioSettings();
                            Save();
                        })));

    Add(kit::SectionHeader("Quality of life"));
    auto toggle = [&](const char* label, const char* help, bool& field) {
        bool* f = &field;
        Add(kit::SettingRow(label, help, kit::Toggle(field, [f](bool on) {
                                *f = on;
                                Save();
                            })));
    };
    auto& q = Cfg().qol;
    toggle("Start games straight from the menu", "Skip the channel preview when you press A", q.quick_launch);
    toggle("Auto-save when quitting", "Saves your exact spot so Resume picks up where you left off",
           q.auto_save_state_on_exit);
    toggle("Always resume from auto-save", "Start every game from its auto-save if it has one", q.auto_load_state);
    toggle("Open the last game on launch", "Jump straight back into what you were playing", q.resume_last_game);
    toggle("Ask before quitting a game", "", q.confirm_quit);
    toggle("Show FPS in game", "Also View + A while playing", q.show_fps);
    toggle("Show the clock", "", q.show_clock);
    toggle("24-hour clock", "", q.clock_24h);
    toggle("Hide updates and DLC from the menu", "They're still listed under Updates & DLC",
           q.hide_updates_dlc_from_grid);

    std::vector<GameEntry> hidden;
    for (const auto& g : Svc().Library().All())
        if (g.hidden) hidden.push_back(g);
    if (!hidden.empty()) {
        Add(kit::SectionHeader("Hidden games"));
        for (auto g : hidden) {
            Add(kit::SettingRow(g.DisplayTitle(), FileName(g.path),
                                kit::ActionButton("Show", kit::glyph::Check, [weak, g]() mutable {
                                    g.hidden = false;
                                    Svc().Library().Update(g);
                                    if (auto s = weak.get()) s->Show("interface");
                                })));
        }
    }
}

// ---------------------------------------------------------------------------
// Updates & DLC

void SettingsPage::BuildUpdates() {
    auto weak = get_weak();
    Add(kit::SectionHeader("Updates & DLC"));
    Note("Put update and DLC .cia files in your Updates & DLC folder (or anywhere in your games folders), "
         "then install them into the emulated SD card. Installed content stays installed; you can delete "
         "the .cia afterwards.");
    const auto items = Svc().Library().Installables();
    if (items.empty()) {
        Note(Cfg().folders.updates_dlc.empty() ? "Set an Updates & DLC folder under Folders first."
                                               : "No update or DLC files found. Rescan after copying them over.");
        return;
    }
    const bool busy = EmulatorSession::Get().State() != SessionState::Idle &&
                      EmulatorSession::Get().State() != SessionState::Failed;
    if (busy) Note("Close the running game to install.");
    auto install = [weak](std::vector<GameEntry> list) {
        RunAsync([weak, list] {
            int ok = 0;
            for (const auto& g : list) {
                std::string msg;
                Svc().Toast("Installing " + g.DisplayTitle() + " " + n3ds::TitleKindName(g.kind) + "...");
                if (EmulatorSession::Get().InstallCia(g.path, nullptr, msg)) ++ok;
                else Svc().Toast(g.DisplayTitle() + ": " + msg);
            }
            Svc().Toast("Installed " + std::to_string(ok) + " of " + std::to_string(list.size()));
        });
    };
    auto all = kit::ActionButton("Install all (" + std::to_string(items.size()) + ")", kit::glyph::Download,
                                 [install, items] { install(items); }, true);
    all.IsEnabled(!busy);
    Add(kit::SettingRow("Install everything", "Takes a minute per gigabyte", all));
    for (const auto& g : items) {
        auto b = kit::ActionButton("Install", kit::glyph::Download, [install, g] { install({g}); });
        b.IsEnabled(!busy);
        Add(kit::SettingRow(g.DisplayTitle() + " - " + n3ds::TitleKindName(g.kind),
                            g.TitleIdHex() + "   " + FileName(g.path) + "   " + FormatSize(g.file_size), b));
    }
}

// ---------------------------------------------------------------------------
// Online services

void SettingsPage::BuildServices() {
    auto weak = get_weak();
    auto& sv = Cfg().services;

    Add(kit::SectionHeader("SteamGridDB artwork"));
    Note("Channel art, banners and logos. Get a free API key at steamgriddb.com > Preferences > API "
         "(on a phone or PC), then type it here.");
    Add(kit::SettingRow("API key", "", kit::Field(sv.steamgriddb_api_key, "Paste or type your key", [](const std::string& v) {
                            Cfg().services.steamgriddb_api_key = Trim(v);
                            Save();
                        })));
    Add(kit::SettingRow("Fetch art for new games automatically", "",
                        kit::Toggle(sv.steamgriddb_auto, [](bool on) {
                            Cfg().services.steamgriddb_auto = on;
                            Save();
                        })));
    Add(kit::SettingRow("Download art for every game", "Only games without art; may take a minute",
                        kit::ActionButton("Download", kit::glyph::Picture, [] {
                            int n = 0;
                            for (const auto& g : Svc().Library().GridView(Cfg().qol))
                                if (g.grid_art.empty()) {
                                    Svc().ScrapeArt(g, false, nullptr);
                                    ++n;
                                }
                            Svc().Toast(n ? "Fetching art for " + std::to_string(n) + " games"
                                          : "Every game already has art");
                        })));

    Add(kit::SectionHeader("RetroAchievements"));
    auto& ra = Svc().RetroAchievements();
    Note("Sign in with your retroachievements.org account. Your password is only used to get a login "
         "token and is never saved. Note: as of October 2026 RetroAchievements has not published any 3DS "
         "achievement sets yet. ONYX 3DS identifies your games and will pick sets up as soon as they exist.");
    if (ra.LoggedIn()) {
        Add(kit::SettingRow("Signed in as " + ra.UserName(), "",
                            kit::ActionButton("Sign out", kit::glyph::Quit, [weak] {
                                Svc().RetroAchievements().Logout();
                                Cfg().services.ra_token.clear();
                                Cfg().services.ra_enabled = false;
                                Save();
                                if (auto s = weak.get()) s->Show("services");
                            })));
    } else {
        auto user = std::make_shared<std::string>(sv.ra_username);
        auto pass = std::make_shared<std::string>();
        Add(kit::SettingRow("Username", "", kit::Field(sv.ra_username, "RetroAchievements username",
                                                        [user](const std::string& v) { *user = Trim(v); })));
        Add(kit::SettingRow("Password", "", kit::Field("", "Password", [pass](const std::string& v) { *pass = v; }, true)));
        Add(kit::SettingRow("Sign in", "", kit::ActionButton("Sign in", kit::glyph::Trophy, [weak, user, pass] {
                                Svc().Toast("Signing in...");
                                Svc().RetroAchievements().LoginWithPassword(*user, *pass, [weak, user](bool ok, const std::string& msg) {
                                    RunOnUi([weak, ok, msg, user] {
                                        if (ok) {
                                            auto& s = Cfg().services;
                                            s.ra_username = *user;
                                            s.ra_token = Svc().RetroAchievements().Token();
                                            s.ra_enabled = true;
                                            Save();
                                            Svc().Toast("Signed in to RetroAchievements");
                                        } else {
                                            Svc().Toast("Sign in failed: " + (msg.empty() ? "check your details" : msg));
                                        }
                                        if (auto self = weak.get()) self->Show("services");
                                    });
                                });
                            }, true)));
    }
    Add(kit::SettingRow("Hardcore mode", "No save states or cheats; unlocks count as hardcore",
                        kit::Toggle(sv.ra_hardcore, [](bool on) {
                            Cfg().services.ra_hardcore = on;
                            Svc().RetroAchievements().SetHardcore(on);
                            Save();
                        })));
    Add(kit::SettingRow("Unlock notifications", "", kit::Toggle(sv.ra_notifications, [](bool on) {
                            Cfg().services.ra_notifications = on;
                            Save();
                        })));

    Add(kit::SectionHeader("Cheat database"));
    Note("Cheats come from the community CTRPF Action Replay database on GitHub "
         "(iSharingan/CTRPF-AR-CHEAT-CODES) and are saved as <TitleID>.txt in your Cheats folder, the same "
         "format Azahar uses on PC.");
    Add(kit::SettingRow("Download cheats automatically", "When you open a game's channel for the first time",
                        kit::Toggle(sv.cheats_auto_download, [](bool on) {
                            Cfg().services.cheats_auto_download = on;
                            Save();
                        })));
    Add(kit::SettingRow("Refresh the cheat list", "", kit::ActionButton("Refresh", kit::glyph::Refresh, [] {
                            RunAsync([] {
                                const bool ok = Svc().Cheats().RefreshIndex(NowUnix(), true);
                                Svc().Toast(ok ? "Cheats available for " + std::to_string(Svc().Cheats().IndexSize()) + " games"
                                               : "Couldn't reach GitHub");
                            });
                        })));
}

// ---------------------------------------------------------------------------
// System check

void SettingsPage::BuildSystem() {
    auto& session = EmulatorSession::Get();
    const VulkanProbe& p = session.Probe();
    Add(kit::SectionHeader("This console"));
    Note(std::string("Model: ") + ConsoleModelName(Svc().Model()) + "    Profile in use: " +
         PerfProfileName(Svc().EffectiveProfile()));
    const uint64_t limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    Note("Memory available to ONYX 3DS: " + FormatSize(limit));
    if (limit < (3ull << 30)) {
        auto warn = kit::Text("ONYX 3DS is running with App resources. In Dev Home, select ONYX 3DS, press "
                              "View > View details, and set App type to Game. Games need the extra memory "
                              "and GPU time.",
                              20, true, "#E8A33D");
        Add(warn);
    }

    Add(kit::SectionHeader("Graphics (Vulkan on DirectX 12)"));
    auto check = [&](bool ok, const std::string& what) {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(12);
        row.Children().Append(kit::Glyph(ok ? kit::glyph::Check : kit::glyph::Warning, 22, ok ? "#3DBE6B" : "#E8A33D"));
        row.Children().Append(kit::Text(what, 20));
        row.Margin(Thickness{0, 4, 0, 4});
        Add(row);
    };
    check(session.Ready(), session.Ready() ? "Ready to run games" : "Not ready (see notes below)");
    check(p.driver_loaded, "Dozen Vulkan driver (vulkan_dzn.dll) loaded");
    check(p.instance_created, "Vulkan instance created");
    check(p.device_found, "GPU found: " + (p.device_name.empty() ? std::string("none") : p.device_name));
    if (!p.driver_info.empty()) Note(p.driver_info);
    check(p.luid_matched, "Vulkan and the presenter share the same GPU");
    check(p.external_memory, "Frame sharing between Vulkan and DirectX 12");
    check(p.external_fence && p.timeline, "GPU-side frame sync (otherwise CPU sync, a little slower)");
    for (const auto& n : p.notes) Note(n);

    Add(kit::SectionHeader("Recent log"));
    auto weak = get_weak();
    Add(kit::SettingRow("Copy the log to your USB drive", "Handy for bug reports",
                        kit::ActionButton("Copy log", kit::glyph::Save, [] {
                            const auto drives = RemovableDriveRoots();
                            if (drives.empty()) {
                                Svc().Toast("Plug in a USB drive first");
                                return;
                            }
                            const std::string dst = JoinPath(drives.front(), "onyx3ds-log.txt");
                            Svc().Toast(Svc().Fs().Copy(Paths().log_file, dst) ? "Saved " + dst : "Copy failed");
                        })));
    std::string tail;
    for (const auto& line : RecentLog(40)) tail += line + "\n";
    auto log = kit::Text(tail.empty() ? "(empty)" : tail, 14);
    log.winrt::Windows::UI::Xaml::Media::FontFamily(FontFamily(L"Consolas"));
    log.IsTextSelectionEnabled(true);
    Add(kit::Card(log, 16));
    (void)weak;
}

// ---------------------------------------------------------------------------
// About

void SettingsPage::BuildAbout() {
    const auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
    Add(kit::SectionHeader("ONYX 3DS " + std::to_string(v.Major) + "." + std::to_string(v.Minor) + "." +
                           std::to_string(v.Build)));
    Note("A Nintendo 3DS emulator frontend for Xbox Series X|S Dev Mode, running the Azahar emulator core "
         "with Vulkan translated to DirectX 12 by Mesa's Dozen driver.");
    Note("Bring your own games: dump cartridges and system files from a 3DS you own. ONYX 3DS does not "
         "include or download any games, firmware or keys.");
    Add(kit::SectionHeader("Credits & licences"));
    for (const char* line : {
             "Azahar emulator (GPLv3+) - azahar-emu.org, built on Citra",
             "Mesa 3D / Dozen Vulkan-on-D3D12 driver (MIT)",
             "rcheevos by RetroAchievements (MIT)",
             "nlohmann/json (MIT), doctest (MIT)",
             "DirectX Shader Compiler DXIL.dll (Microsoft redistributable)",
             "Menu artwork, themes and music: original, generated for ONYX 3DS (GPLv3+)",
             "Cheat database: CTRPF Action Replay codes by the community",
             "Artwork: SteamGridDB community uploads",
             "ONYX 3DS is GPLv3+. Source: see README on the project's repository."})
        Note(line);
}

} // namespace winrt::ONYX3DS::implementation
