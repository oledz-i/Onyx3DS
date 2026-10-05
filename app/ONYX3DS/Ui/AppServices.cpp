// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Ui/AppServices.h"

#include "Platform/Log.h"

using namespace winrt;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::UI::Xaml::Media;

namespace onyx::app {

AppServices& AppServices::Get() {
    static AppServices services;
    return services;
}

void AppServices::Initialize() {
    const AppPaths& p = Paths();
    fs_.CreateDirs(p.cache);
    fs_.CreateDirs(p.art);
    model_ = DetectConsole();

    const std::string text = fs_.ReadText(p.settings_file);
    settings_ = text.empty() ? Settings::Defaults(model_) : Settings::FromJson(text, model_);
    if (text.empty()) {
        // First launch: if a USB drive is plugged in and already has the
        // ONYX3DS layout, use it straight away.
        for (const auto& drive : RemovableDriveRoots()) {
            if (fs_.IsDirectory(JoinPath(drive, "ONYX3DS/Roms"))) {
                settings_.folders.ApplyDriveLayout(drive);
                ONYX_INFO("Found ONYX3DS folders on %s", drive.c_str());
                break;
            }
        }
        SaveSettings();
    }
    ONYX_INFO("Console: %s, profile %s", ConsoleModelName(model_),
              PerfProfileName(EffectiveProfile()));

    library_ = std::make_unique<GameLibrary>(fs_, p.cache);
    library_->Load();
    cheat_db_ = std::make_unique<CheatDatabase>(http_, fs_, p.cache);
    ra_ = std::make_unique<Achievements>(http_, [](std::function<void()> f) { RunAsync(std::move(f)); });
    Achievements::UseFileSystem(&fs_);
    ra_->SetHardcore(settings_.services.ra_hardcore);
    if (settings_.services.ra_enabled && !settings_.services.ra_token.empty()) {
        ra_->LoginWithToken(settings_.services.ra_username, settings_.services.ra_token,
                            [](bool ok, const std::string& msg) {
                                if (!ok) ONYX_WARN("RetroAchievements login: %s", msg.c_str());
                            });
    }
    EmulatorSession::Get().SetAchievements(ra_.get());

    {
        const std::string cat = fs_.ReadText(JoinPath(p.cache, "core_options.json"));
        std::lock_guard lock(catalog_mutex_);
        if (!cat.empty()) catalog_ = CoreOptionCatalog::FromJson(cat);
    }
    ReloadThemes();
    LoadSounds();
}

void AppServices::SaveSettings() {
    std::string text = settings_.ToJson(); // snapshot on the caller's thread
    {
        std::lock_guard lock(save_mutex_);
        pending_settings_ = std::move(text);
        if (save_queued_) return; // the queued writer picks up the newest text
        save_queued_ = true;
    }
    RunAsync([this] {
        for (;;) {
            std::string t;
            {
                std::lock_guard lock(save_mutex_);
                if (pending_settings_.empty()) {
                    save_queued_ = false;
                    return;
                }
                t.swap(pending_settings_);
            }
            fs_.WriteText(Paths().settings_file, t);
        }
    });
}

void AppServices::FlushSettings() {
    std::string text = settings_.ToJson();
    std::lock_guard lock(save_mutex_);
    pending_settings_.clear();
    fs_.WriteText(Paths().settings_file, text);
}

PerfProfile AppServices::EffectiveProfile() const {
    return settings_.profile == PerfProfile::Auto ? ResolveAuto(model_) : settings_.profile;
}

void AppServices::RescanLibrary(std::function<void(size_t)> done) {
    if (scanning_.exchange(true)) return;
    const FolderConfig folders = settings_.folders;
    const bool auto_art = settings_.services.steamgriddb_auto && !settings_.services.steamgriddb_api_key.empty();
    RunAsync([this, folders, auto_art, done = std::move(done)] {
        library_->Scan(folders);
        const auto games = library_->All();
        scanning_ = false;
        RunOnUi([done, n = games.size()] {
            if (done) done(n);
        });
        if (auto_art) {
            // Fill in missing artwork quietly after the grid is up.
            SteamGridDb sgdb(http_, settings_.services.steamgriddb_api_key);
            ArtScraper scraper(sgdb, fs_, Paths().art);
            int fetched = 0;
            for (auto g : games) {
                if (!g.IsLaunchable() || !g.grid_art.empty()) continue;
                if (scraper.Scrape(g)) {
                    library_->Update(g);
                    ++fetched;
                }
            }
            if (fetched) {
                RunOnUi([this, fetched, done] {
                    Toast("Artwork downloaded for " + std::to_string(fetched) +
                          (fetched == 1 ? " game" : " games"));
                    if (done) done(library_->All().size());
                });
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Themes

void AppServices::ReloadThemes() {
    themes_ = LoadThemes(fs_, Paths().builtin_themes, settings_.folders.themes);
    ONYX_INFO("%zu themes available (built-in dir %s: %s, %zu entries)", themes_.size(),
              Paths().builtin_themes.c_str(), fs_.IsDirectory(Paths().builtin_themes) ? "found" : "MISSING",
              fs_.List(Paths().builtin_themes).size());
}

const Theme& AppServices::CurrentTheme() const {
    static const Theme fallback = [] {
        Theme t;
        t.id = "fallback";
        t.name = "Plain";
        return t;
    }();
    const Theme* t = FindTheme(themes_, settings_.qol.theme_id);
    return t ? *t : fallback;
}

void AppServices::SetTheme(const std::string& id) {
    settings_.qol.theme_id = id;
    SaveSettings();
    ApplyGlobalFont();
    LoadSounds();
    if (settings_.qol.menu_music) {
        StopMusic(false);
        StartMusic();
    }
}

namespace {
uint8_t HexByte(const std::string& s, size_t i) {
    return static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16));
}

// "a/b/../c" -> "a/c"
std::string NormalizeRelative(const std::string& path) {
    std::vector<std::string> parts;
    for (const auto& seg : Split(NormalizeSlashes(path), '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == ".." && !parts.empty()) parts.pop_back();
        else parts.push_back(seg);
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}
} // namespace

winrt::Windows::UI::Color AppServices::ThemeColor(const std::string& hex) const {
    winrt::Windows::UI::Color c{255, 128, 128, 128};
    try {
        if (hex.size() >= 7 && hex[0] == '#') {
            c.R = HexByte(hex, 1);
            c.G = HexByte(hex, 3);
            c.B = HexByte(hex, 5);
            c.A = hex.size() >= 9 ? HexByte(hex, 7) : 255;
        }
    } catch (...) {
    }
    return c;
}

SolidColorBrush AppServices::ThemeBrush(const std::string& hex) const {
    return SolidColorBrush(ThemeColor(hex));
}

std::string AppServices::ThemeAsset(const std::string& relative) const {
    if (relative.empty()) return {};
    const Theme& t = CurrentTheme();
    if (t.built_in) {
        // Built-in themes live in the package: use ms-appx URIs.
        const std::string rel = NormalizeRelative("Assets/Themes/" + FileName(t.folder) + "/" + relative);
        return "ms-appx:///" + rel;
    }
    return t.Resolve(relative);
}

void AppServices::SetLaunchArguments(const std::wstring& args) {
    constexpr std::wstring_view kResume = L"resume=";
    if (args.rfind(kResume, 0) == 0) resume_path_ = winrt::to_string(args.substr(kResume.size()));
}

void AppServices::RestartApp(const std::string& game_path) {
    ONYX_INFO("Restarting ONYX%s", game_path.empty() ? "" : " to reopen the game");
    FlushSettings();
    const std::wstring args = game_path.empty() ? std::wstring() : L"resume=" + std::wstring(winrt::to_hstring(game_path));
    try {
        auto op = winrt::Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(args);
        op.Completed([](auto const& async, winrt::Windows::Foundation::AsyncStatus status) {
            // Success never comes back (the app is gone); anything else: just close.
            ONYX_WARN("Restart refused (status %d, reason %d); closing instead", static_cast<int>(status),
                      status == winrt::Windows::Foundation::AsyncStatus::Completed
                          ? static_cast<int>(async.GetResults())
                          : -1);
            winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
        });
    } catch (winrt::hresult_error const& e) {
        ONYX_WARN("Restart not available (0x%08X); closing instead", static_cast<unsigned>(e.code().value));
        winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
    }
}

FontFamily AppServices::ThemeFont(bool bold) const {
    const auto& style = CurrentTheme().style;
    const std::string& name = bold && !style.font_bold.empty() ? style.font_bold : style.font;
    if (name.empty()) return FontFamily(L"Segoe UI");
    return FontFamily(winrt::to_hstring(name));
}

void AppServices::ApplyGlobalFont() {
    try {
        const FontFamily font = ThemeFont(false);
        // Control templates (buttons, toggles, dialogs, combo boxes) read this resource.
        auto resources = winrt::Windows::UI::Xaml::Application::Current().Resources();
        resources.Insert(winrt::box_value(L"ContentControlThemeFontFamily"), font);
        // Plain TextBlocks inherit the font from the root Frame.
        if (auto window = winrt::Windows::UI::Xaml::Window::Current()) {
            if (auto frame = window.Content().try_as<winrt::Windows::UI::Xaml::Controls::Frame>())
                frame.FontFamily(font);
        }
    } catch (winrt::hresult_error const& e) {
        ONYX_WARN("Could not apply the theme font: 0x%08X", static_cast<unsigned>(e.code().value));
    }
}

// ---------------------------------------------------------------------------
// Sound

void AppServices::LoadSounds() {
    const Theme& t = CurrentTheme();
    auto load = [&](const char* id, const std::string& rel) {
        if (rel.empty()) return;
        std::string path = t.built_in
                               ? JoinPath(Paths().install,
                                          NormalizeRelative("Assets/Themes/" + FileName(t.folder) + "/" + rel))
                               : t.Resolve(rel);
        if (!sfx_.Load(id, path)) ONYX_WARN("Sound %s missing (%s)", id, path.c_str());
    };
    load("move", t.audio.move);
    load("select", t.audio.select);
    load("back", t.audio.back);
    load("launch", t.audio.launch);
    ApplyAudioSettings();
}

void AppServices::PlaySfx(const char* id) {
    sfx_.Play(id);
}

void AppServices::ApplyAudioSettings() {
    sfx_.SetEnabled(settings_.qol.menu_sounds);
    sfx_.SetVolume(0.75f);
    const double volume = settings_.qol.music_volume / 100.0;
    const bool enabled = settings_.qol.menu_music;
    RunAsync([this, volume, enabled] {
        std::lock_guard lock(music_mutex_);
        if (!music_) return;
        music_.Volume(volume);
        if (!enabled) music_.Pause();
    });
}

void AppServices::StartMusic() {
    if (!settings_.qol.menu_music) return;
    // StorageFile lookups block, so build the source on the thread pool.
    // MediaPlayer is agile and can be driven from any thread.
    RunAsync([this] {
        std::lock_guard lock(music_mutex_);
        StartMusicLocked();
    });
}

void AppServices::StartMusicLocked() {
    if (!settings_.qol.menu_music) return;
    try {
        if (!music_) {
            music_ = MediaPlayer();
            music_.AudioCategory(MediaPlayerAudioCategory::GameMedia);
            music_.IsLoopingEnabled(true);
        }
        music_.Volume(settings_.qol.music_volume / 100.0);

        // User playlist from the Music folder wins over the theme's track.
        std::vector<std::string> tracks;
        if (!settings_.folders.music.empty()) {
            for (const auto& e : fs_.List(settings_.folders.music)) {
                const std::string ext = Extension(e.name);
                if (!e.is_dir && (ext == ".mp3" || ext == ".m4a" || ext == ".wma" || ext == ".wav" ||
                                  ext == ".flac" || ext == ".aac"))
                    tracks.push_back(JoinPath(settings_.folders.music, e.name));
            }
        }
        if (!tracks.empty()) {
            const std::string key = "user:" + settings_.folders.music;
            if (music_path_ != key) {
                MediaPlaybackList list;
                list.ShuffleEnabled(true);
                list.AutoRepeatEnabled(true);
                for (const auto& tr : tracks) {
                    std::wstring w = Wide(tr);
                    std::replace(w.begin(), w.end(), L'/', L'\\');
                    auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(w).get();
                    list.Items().Append(MediaPlaybackItem(MediaSource::CreateFromStorageFile(file)));
                }
                music_.IsLoopingEnabled(false);
                music_.Source(list);
                music_path_ = key;
            }
        } else {
            const std::string uri = ThemeAsset(CurrentTheme().audio.music);
            if (uri.empty()) return;
            if (music_path_ != uri) {
                if (uri.rfind("ms-appx:", 0) == 0) {
                    music_.Source(MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri(Wide(uri))));
                } else {
                    std::wstring w = Wide(uri);
                    std::replace(w.begin(), w.end(), L'/', L'\\');
                    auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(w).get();
                    music_.Source(MediaSource::CreateFromStorageFile(file));
                }
                music_.IsLoopingEnabled(true);
                music_path_ = uri;
            }
        }
        music_.Play();
    } catch (hresult_error const& e) {
        ONYX_WARN("Menu music unavailable: %s", Utf8(e.message()).c_str());
    }
}

void AppServices::StopMusic(bool fade) {
    RunAsync([this, fade] {
        std::lock_guard lock(music_mutex_);
        if (!music_) return;
        if (fade) {
            // Short fade so launching a game does not cut the music mid-note.
            const double start = music_.Volume();
            for (int i = 10; i >= 0; --i) {
                music_.Volume(start * i / 10.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            music_.Pause();
            music_.Volume(start);
        } else {
            music_.Pause();
        }
    });
}

// ---------------------------------------------------------------------------
// Online

void AppServices::ScrapeArt(const GameEntry& game, bool overwrite, std::function<void(bool)> done) {
    const std::string key = settings_.services.steamgriddb_api_key;
    if (key.empty()) {
        Toast("Add your SteamGridDB API key in Settings > Online services first");
        if (done) done(false);
        return;
    }
    // Init-capture drops the const so the scraper can fill in the art paths.
    RunAsync([this, g = game, key, overwrite, done = std::move(done)]() mutable {
        SteamGridDb sgdb(http_, key);
        ArtScraper scraper(sgdb, fs_, Paths().art);
        const bool ok = scraper.Scrape(g, overwrite);
        if (ok) library_->Update(g);
        const int status = sgdb.LastStatus();
        RunOnUi([this, ok, status, done] {
            if (!ok) {
                Toast(status == 401 || status == 403 ? "SteamGridDB rejected the API key"
                      : status == 0                  ? "Can't reach SteamGridDB. Are you online?"
                                                     : "No artwork found for this game");
            }
            if (done) done(ok);
        });
    });
}

std::string AppServices::CheatsFolder() const {
    // Azahar's own cheats folder unless the user picked one.
    return settings_.folders.cheats.empty() ? JoinPath(Paths().azahar_root, "Azahar/cheats")
                                            : settings_.folders.cheats;
}

std::string AppServices::CheatFilePath(u64 title_id) const {
    return JoinPath(CheatsFolder(), TitleIdToHex(title_id) + ".txt");
}

void AppServices::DownloadCheats(u64 title_id, std::function<void(int)> done) {
    const std::string dir = CheatsFolder();
    RunAsync([this, title_id, dir, done = std::move(done)] {
        int added = -1;
        if (cheat_db_->RefreshIndex(NowUnix())) added = cheat_db_->DownloadInto(title_id, dir);
        RunOnUi([done, added] {
            if (done) done(added);
        });
    });
}

CoreOptionCatalog AppServices::Catalog() const {
    std::lock_guard lock(catalog_mutex_);
    return catalog_;
}

void AppServices::StoreCatalog(const CoreOptionCatalog& catalog) {
    if (catalog.options.empty()) return;
    {
        std::lock_guard lock(catalog_mutex_);
        catalog_ = catalog;
    }
    fs_.WriteText(JoinPath(Paths().cache, "core_options.json"), catalog.ToJson());
}

void AppServices::SetToastSink(std::function<void(const std::string&)> sink) {
    std::lock_guard lock(toast_mutex_);
    toast_sink_ = std::move(sink);
}

void AppServices::Toast(const std::string& text) {
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard lock(toast_mutex_);
        sink = toast_sink_;
    }
    ONYX_INFO("toast: %s", text.c_str());
    if (sink) RunOnUi([sink, text] { sink(text); });
}

// ---------------------------------------------------------------------------

std::string FormatPlayTime(u64 s) {
    if (s < 60) return s ? "Under a minute" : "Never played";
    const u64 h = s / 3600, m = (s % 3600) / 60;
    if (!h) return std::to_string(m) + "m";
    return std::to_string(h) + "h " + std::to_string(m) + "m";
}

std::string FormatLastPlayed(u64 t) {
    if (!t) return "Not played yet";
    const u64 now = NowUnix();
    const u64 d = now > t ? now - t : 0;
    if (d < 3600) return "Played just now";
    if (d < 86400) return "Played today";
    if (d < 2 * 86400) return "Played yesterday";
    if (d < 30 * 86400) return "Played " + std::to_string(d / 86400) + " days ago";
    return "Played a while ago";
}

std::string FormatSize(u64 b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GB", b / double(1ull << 30));
    else std::snprintf(buf, sizeof(buf), "%.0f MB", b / double(1ull << 20));
    return buf;
}

} // namespace onyx::app
