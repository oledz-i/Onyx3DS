// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the pages share: settings, the game library, themes, online
// services, menu music and sounds. One instance for the app's lifetime.
#pragma once

#include "Emu/AudioOutput.h"
#include "Emu/EmulatorSession.h"
#include "Platform/UwpPlatform.h"
#include "onyx/achievements.h"
#include "onyx/cheats.h"
#include "onyx/core_options.h"
#include "onyx/library.h"
#include "onyx/settings.h"
#include "onyx/steamgriddb.h"

#include <utility>
#include "onyx/theme.h"

namespace onyx::app {

class AppServices {
public:
    static AppServices& Get();

    // Loads settings, library cache, themes and sounds. UI thread, at launch.
    void Initialize();

    // ---- settings ---------------------------------------------------------
    Settings& Config() { return settings_; }
    // Writes settings.json on a background thread (latest write wins), so the
    // UI never waits on the file system.
    void SaveSettings();
    // Writes any pending settings now, on the calling thread (suspend/exit).
    void FlushSettings();
    ConsoleModel Model() const { return model_; }
    PerfProfile EffectiveProfile() const;

    // ---- library ----------------------------------------------------------
    GameLibrary& Library() { return *library_; }
    // Rescans in the background; `done` runs on the UI thread.
    void RescanLibrary(std::function<void(size_t games)> done);
    bool Scanning() const { return scanning_; }
    std::optional<GameEntry> SelectedGame() const { return selected_; }
    void SetSelectedGame(std::optional<GameEntry> g) { selected_ = std::move(g); }

    // ---- themes -----------------------------------------------------------
    const std::vector<Theme>& Themes() const { return themes_; }
    const Theme& CurrentTheme() const;
    void SetTheme(const std::string& id);
    void ReloadThemes();
    winrt::Windows::UI::Color ThemeColor(const std::string& hex) const;
    winrt::Windows::UI::Xaml::Media::SolidColorBrush ThemeBrush(const std::string& hex) const;
    std::string ThemeAsset(const std::string& relative) const; // absolute path or ms-appx URI
    // The theme's font (bold: its heading font). Falls back to Segoe UI when unset.
    winrt::Windows::UI::Xaml::Media::FontFamily ThemeFont(bool bold = false) const;
    // True when ThemeFont(true) is a real bold face (no synthetic weight needed).
    bool ThemeHasBoldFont() const { return !CurrentTheme().style.font_bold.empty(); }
    // Makes the theme font the default for every page, dialog and control.
    void ApplyGlobalFont();

    // ---- sound ------------------------------------------------------------
    void PlaySfx(const char* id);
    void StartMusic();
    void StopMusic(bool fade = true);
    void ApplyAudioSettings();

    // ---- online -----------------------------------------------------------
    IHttpClient& Http() { return http_; }
    UwpFileSystem& Fs() { return fs_; }
    CheatDatabase& Cheats() { return *cheat_db_; }
    Achievements& RetroAchievements() { return *ra_; }
    // Fetches SteamGridDB art for one game; `done(ok)` on the UI thread.
    void ScrapeArt(const GameEntry& game, bool overwrite, std::function<void(bool)> done);
    // Downloads cheats for a game into the cheats folder (or LocalState).
    void DownloadCheats(u64 title_id, std::function<void(int added)> done);
    std::string CheatsFolder() const;
    std::string CheatFilePath(u64 title_id) const;

    // ---- core option catalogue (cached across sessions) -------------------
    CoreOptionCatalog Catalog() const;
    void StoreCatalog(const CoreOptionCatalog& catalog);

    // ---- back button ------------------------------------------------------
    // A page can claim B / Back (e.g. to close a side panel first). Returns
    // true when the override handled it.
    void SetBackOverride(std::function<bool()> handler) { back_override_ = std::move(handler); }
    bool TryHandleBack() { return back_override_ && back_override_(); }

    // ---- restart ----------------------------------------------------------
    // Restarts the app (settings flushed first) and reopens `game_path` once it
    // is back; closes the app if the system refuses the restart.
    void RestartApp(const std::string& game_path);
    // Launch arguments "resume=<path>" from a restart: read once.
    void SetLaunchArguments(const std::wstring& args);
    std::string TakeResumePath() { return std::exchange(resume_path_, {}); }

    // ---- toasts -------------------------------------------------------------
    // Pages register a sink; messages from background work land there.
    void SetToastSink(std::function<void(const std::string&)> sink);
    void Toast(const std::string& text);

private:
    std::string resume_path_;
    AppServices() = default;
    void LoadSounds();
    void StartMusicLocked();

    UwpFileSystem fs_;
    UwpHttpClient http_;
    Settings settings_;
    ConsoleModel model_ = ConsoleModel::Unknown;
    std::unique_ptr<GameLibrary> library_;
    std::unique_ptr<CheatDatabase> cheat_db_;
    std::unique_ptr<Achievements> ra_;
    std::vector<Theme> themes_;
    std::atomic<bool> scanning_{false};
    std::optional<GameEntry> selected_;
    std::mutex save_mutex_;
    std::string pending_settings_;
    bool save_queued_ = false;
    SoundEffects sfx_;
    winrt::Windows::Media::Playback::MediaPlayer music_{nullptr};
    std::string music_path_;
    std::mutex music_mutex_;
    std::function<bool()> back_override_;
    std::mutex toast_mutex_;
    std::function<void(const std::string&)> toast_sink_;
    mutable std::mutex catalog_mutex_;
    CoreOptionCatalog catalog_;
};

// Formats "2h 14m", "45m", "Never played".
std::string FormatPlayTime(u64 seconds);
std::string FormatLastPlayed(u64 unix_seconds);
std::string FormatSize(u64 bytes);

} // namespace onyx::app
