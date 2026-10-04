// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the statically linked Azahar libretro core on its own thread and
// connects it to the Xbox: Vulkan-on-D3D12 video, XAudio2, controllers, save
// states, cheats, achievements and the folder remapping.
//
// libretro callbacks are plain C functions, so there is one session object
// for the whole app (Get()).
#pragma once

#include "Emu/AudioOutput.h"
#include "Emu/D3D12Presenter.h"
#include "Emu/InputManager.h"
#include "Emu/VulkanHost.h"
#include "onyx/achievements.h"
#include "onyx/cheats.h"
#include "onyx/core_options.h"
#include "onyx/library.h"
#include "onyx/paths.h"
#include "onyx/settings.h"

namespace onyx::app {

struct GuardedCrash;

enum class SessionState { Idle, Starting, Running, Paused, Stopping, Failed };

struct SessionEvents {
    std::function<void()> started;
    std::function<void(const std::string& reason)> stopped;  // empty reason = normal quit
    std::function<void(const std::string& text)> message;    // toasts
    std::function<void(Hotkey)> hotkey;                      // menu, fps, screenshot...
    std::function<void(const AchievementEvent&)> achievement;
};

class EmulatorSession {
public:
    static EmulatorSession& Get();

    // Graphics/audio bring-up and the Dozen self-test. Call once on startup
    // (off the UI thread). Safe to call again; it returns the first result.
    bool Initialize(std::string& error);
    const VulkanProbe& Probe() const { return probe_; }
    bool Ready() const { return ready_; }
    // The last game stopped because the CPU JIT crashed.
    bool CrashedInJit() const { return crashed_in_jit_.load(); }

    D3D12Presenter& Presenter() { return presenter_; }
    InputManager& Input() { return input_; }
    AudioOutput& Audio() { return audio_; }
    void SetAchievements(Achievements* ra) { ra_ = ra; }
    void SetEvents(SessionEvents events);

    // ---- game lifecycle (UI thread) ----------------------------------------
    bool Start(const GameEntry& game, const Settings& settings, ConsoleModel model,
               std::string& error);
    void Stop(bool write_auto_state);
    void SetPaused(bool paused);
    SessionState State() const { return state_.load(); }
    const GameEntry& Game() const { return game_; }
    u64 SessionSeconds() const;

    void Reset();
    void SaveState(int slot);  // slot 0 = "auto"
    void LoadState(int slot);
    int CurrentSlot() const { return slot_.load(); }
    void SetCurrentSlot(int slot) { slot_ = std::clamp(slot, 1, 9); }
    std::string StatePath(int slot) const;
    std::string StateThumbPath(int slot) const;
    bool HasState(int slot) const;

    void SetFastForward(bool on);
    bool FastForward() const { return fast_forward_.load(); }
    void SetFastForwardSpeed(int percent) { ff_speed_ = percent; }

    // Live option changes (resolution, layout, ...). Applied between frames.
    void ApplyCoreOptions(const CoreOptions& options);
    CoreOptions CurrentCoreOptions() const;
    CoreOptionCatalog Catalog() const;
    void CycleLayout();

    // Cheats for the running game: pushes the enabled ones into the core.
    void ApplyCheats(const CheatFile& cheats);

    // Screenshot of the current frame to the screenshots folder (PNG).
    void Screenshot();

    // Updates & DLC: installs a CIA into the emulated SD card. Not while a
    // game is running. Progress is 0..1.
    bool InstallCia(const std::string& path, const std::function<void(double)>& progress,
                    std::string& message);

    // Called by FileUtil for every path Azahar opens.
    static std::string TranslatePath(const std::string& path);
    void UpdateFolders(const FolderConfig& folders);
    std::string AzaharUserDir() const;

    // libretro environment / callbacks (public for the C trampolines)
    bool Environment(unsigned cmd, void* data);
    void VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch);
    size_t AudioBatch(const int16_t* data, size_t frames);
    void InputPoll();
    int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id);

private:
    EmulatorSession();
    void EmulationThread(std::string rom_path);
    void EmulationThreadBody(const std::string& rom_path);
    void OnCoreCrashed(const GuardedCrash& crash);
    void RunCommands();
    void Pace(std::chrono::steady_clock::time_point& deadline);
    void Post(std::function<void()> command);
    void Message(const std::string& text);
    void DoSaveState(int slot);
    void DoLoadState(int slot);
    uint32_t ReadMemory(uint32_t address, uint8_t* buffer, uint32_t n);
    void ParseOptionsV2(const retro_core_options_v2* options);
    void ParseOptionsV1(const retro_core_option_definition* defs);
    void ParseVariables(const retro_variable* vars);

    D3D12Presenter presenter_;
    DozenDriver dozen_;
    std::unique_ptr<VulkanHost> vulkan_;
    AudioOutput audio_;
    InputManager input_;
    Achievements* ra_ = nullptr;
    VulkanProbe probe_{};
    bool ready_ = false;
    bool core_initialised_ = false;
    // Set when the core crashed. Its state is undefined afterwards, so it is
    // never called again in this process; the user restarts the app.
    std::atomic<bool> core_crashed_{false};
    std::atomic<bool> crashed_in_jit_{false};
    int frames_since_drain_ = 0;
    std::mutex init_mutex_;

    SessionEvents events_;
    std::mutex events_mutex_;
    std::vector<std::string> startup_messages_; // core messages while a game boots

    std::atomic<SessionState> state_{SessionState::Idle};
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};
    std::atomic<bool> write_auto_on_stop_{false};
    std::mutex command_mutex_;
    std::vector<std::function<void()>> commands_;

    GameEntry game_;
    ConsoleModel model_ = ConsoleModel::Unknown;
    std::chrono::steady_clock::time_point started_at_{};
    std::atomic<int> slot_{1};
    std::atomic<bool> fast_forward_{false};
    std::atomic<int> ff_speed_{300};

    // core options
    mutable std::mutex options_mutex_;
    CoreOptions options_;
    CoreOptionCatalog catalog_;
    std::atomic<bool> options_dirty_{false};
    std::vector<std::string> option_value_storage_; // keeps GET_VARIABLE strings alive

    // hw render
    retro_hw_render_callback hw_render_{};
    bool hw_render_set_ = false;
    std::vector<retro_memory_descriptor> memory_map_;
    std::string azahar_dir_;
};

} // namespace onyx::app
