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
#include "Emu/GlHost.h"
#include "Emu/InputManager.h"
#include "Emu/VulkanHost.h"
#include "onyx/achievements.h"
#include "onyx/cheats.h"
#include "onyx/core_options.h"
#include "onyx/library.h"
#include "onyx/nes_support.h"
#include "onyx/patch.h"
#include "onyx/paths.h"
#include "onyx/pixel.h"
#include "onyx/settings.h"

namespace onyx::app {

struct GuardedCrash;

// The libretro entry points of one core. The Azahar core is linked statically
// (filled from the retro_* symbols); the NES core is a DLL (filled with
// GetProcAddress). Everything that calls into a core goes through one of these.
struct CoreApi {
    void (RETRO_CALLCONV* init)() = nullptr;
    void (RETRO_CALLCONV* set_environment)(retro_environment_t) = nullptr;
    void (RETRO_CALLCONV* set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (RETRO_CALLCONV* set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (RETRO_CALLCONV* set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (RETRO_CALLCONV* set_input_poll)(retro_input_poll_t) = nullptr;
    void (RETRO_CALLCONV* set_input_state)(retro_input_state_t) = nullptr;
    bool (RETRO_CALLCONV* load_game)(const retro_game_info*) = nullptr;
    void (RETRO_CALLCONV* unload_game)() = nullptr;
    void (RETRO_CALLCONV* get_system_av_info)(retro_system_av_info*) = nullptr;
    void (RETRO_CALLCONV* run)() = nullptr;
    void (RETRO_CALLCONV* reset)() = nullptr;
    size_t (RETRO_CALLCONV* serialize_size)() = nullptr;
    bool (RETRO_CALLCONV* serialize)(void*, size_t) = nullptr;
    bool (RETRO_CALLCONV* unserialize)(const void*, size_t) = nullptr;
    void (RETRO_CALLCONV* cheat_reset)() = nullptr;
    void (RETRO_CALLCONV* cheat_set)(unsigned, bool, const char*) = nullptr;
    void* (RETRO_CALLCONV* get_memory_data)(unsigned) = nullptr;
    size_t (RETRO_CALLCONV* get_memory_size)(unsigned) = nullptr;
    bool initialised = false; // retro_init has run (once per process)
    bool Complete() const { return run && load_game && serialize && get_memory_data; }
};

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
    // The last game stopped because the hardware (Vulkan) renderer crashed.
    bool CrashedInGpu() const { return crashed_in_gpu_.load(); }
    bool UsingSoftwareRenderer() const { return software_.load(); }
    // fceumm_libretro.dll is in the package (CI builds it separately and may skip it).
    static bool NesCoreAvailable();
    GameSystem System() const { return system_; }
    // Frames arrive as CPU pixels (software renderer, or hardware frames read back).
    bool UsingCpuFrames() const;

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
    // Longest gap between two emulated frames since the last call (ms), for the
    // FPS overlay: a stutter shows up here even when the average FPS looks fine.
    double TakeWorstFrameMs() { return worst_frame_us_.exchange(0) / 1000.0; }
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
    bool EnsureNesCore(std::string& error);
    bool RaActive() const { return ra_ && system_ == GameSystem::N3DS; }
    void LoadSram();
    // Writes the battery save if it changed. `wait` = finish the write before returning;
    // otherwise a worker thread does it (a USB drive can take far longer than a frame).
    void FlushSram(bool wait);
    void ApplyNesPatch(IFileSystem& fs, const std::string& rom_path);
    std::string SramPath() const;
    void OnCoreCrashed(const GuardedCrash& crash);
    // If the D3D12 device was removed, logs why (and DRED data when available).
    void LogGpuRemovedReason();
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
    std::unique_ptr<GlHost> gl_;
    AudioOutput audio_;
    InputManager input_;
    Achievements* ra_ = nullptr;
    VulkanProbe probe_{};
    bool ready_ = false;
    // Core in use: Azahar (static) for 3DS games, FCEUmm (DLL) for NES games.
    CoreApi azahar_api_;
    CoreApi nes_api_;
    CoreApi* api_ = &azahar_api_;
    GameSystem system_ = GameSystem::N3DS;
    double core_fps_ = 60.0;                        // pacing; the NES runs at ~60.1
    retro_pixel_format pixel_format_ = RETRO_PIXEL_FORMAT_XRGB8888;
    std::vector<uint32_t> convert_frame_;           // NES frames widened to XRGB8888
    std::vector<uint8_t> rom_data_;                 // NES: the ROM, handed to the core in memory
    std::string rom_path_, rom_dir_, rom_name_, rom_ext_;
    retro_game_info_ext game_info_ext_{};
    std::vector<uint8_t> sram_last_;                // battery save as last written
    bool sram_active_ = false;
    std::thread sram_thread_;                       // background battery save write
    std::atomic<bool> sram_busy_{false};            // that write is still running
    std::atomic<bool> sram_failed_{false};          // the last background write failed: retry
    std::string nes_patches_dir_;                   // Settings folders.nes_patches
    std::string nes_patch_choice_;                  // Settings nes_patch for this game
    size_t option_value_next_ = 0;                  // ring position in option_value_storage_
    // Set when the core crashed. Its state is undefined afterwards, so it is
    // never called again in this process; the user restarts the app.
    std::atomic<bool> core_crashed_{false};
    std::atomic<bool> crashed_in_jit_{false};
    std::atomic<bool> crashed_in_gpu_{false};
    // Vulkan (Dozen) came up at startup. Without it only the software renderer runs.
    bool vulkan_ok_ = false;
    // Mesa's OpenGL on D3D12 made a working context at startup.
    bool gl_ok_ = false;
    // This game uses Azahar's software renderer: frames arrive as CPU pixels.
    std::atomic<bool> software_{true};
    // This game uses the OpenGL hardware renderer (else, when not software, Vulkan).
    std::atomic<bool> use_gl_{false};
    int frames_since_drain_ = 0;
    // Frame rate lock (keys::kFrameLock): 60, or 30 = show every 2nd frame at an
    // even pace while the game keeps running at full speed.
    std::atomic<int> frame_lock_{60};
    std::atomic<long long> worst_frame_us_{0};
    uint64_t hw_frames_ = 0;
    bool custom_textures_allowed_ = true; // false: no pack for this game
    void UpdateFrameLockLocked(); // options_mutex_ held
    std::vector<uint32_t> sw_frame_; // reusable software frame (XRGB8888)
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
