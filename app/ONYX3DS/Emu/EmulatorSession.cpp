// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Emu/EmulatorSession.h"

#include <cstdarg>

#include "Emu/AzaharBridge.h"
#include "Platform/CrashHandler.h"
#include "Platform/Imaging.h"
#include "Platform/Log.h"
#include "Platform/UwpPlatform.h"

namespace onyx::app {

namespace {

// ---- path translation (called by Azahar's FileUtil on any thread) ----------
std::atomic<std::shared_ptr<const PathRemapper>> g_remapper;

std::string RemapForAzahar(const std::string& path) {
    const auto remapper = g_remapper.load();
    return remapper ? remapper->Map(path) : path;
}

// ---- libretro C trampolines -------------------------------------------------
bool RETRO_CALLCONV EnvCb(unsigned cmd, void* data) {
    return EmulatorSession::Get().Environment(cmd, data);
}
void RETRO_CALLCONV VideoCb(const void* data, unsigned w, unsigned h, size_t pitch) {
    EmulatorSession::Get().VideoRefresh(data, w, h, pitch);
}
void RETRO_CALLCONV AudioSampleCb(int16_t l, int16_t r) {
    const int16_t frame[2] = {l, r};
    EmulatorSession::Get().AudioBatch(frame, 1);
}
size_t RETRO_CALLCONV AudioBatchCb(const int16_t* data, size_t frames) {
    return EmulatorSession::Get().AudioBatch(data, frames);
}
void RETRO_CALLCONV InputPollCb() {
    EmulatorSession::Get().InputPoll();
}
int16_t RETRO_CALLCONV InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id) {
    return EmulatorSession::Get().InputState(port, device, index, id);
}
void RETRO_CALLCONV CoreLogCb(enum retro_log_level level, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    const LogLevel lv = level == RETRO_LOG_ERROR  ? LogLevel::Error
                        : level == RETRO_LOG_WARN ? LogLevel::Warning
                        : level == RETRO_LOG_INFO ? LogLevel::Info
                                                  : LogLevel::Debug;
    Log(lv, "[azahar] %s", buf);
}

// Sleeping with ~0.5 ms accuracy; the default timer tick on Windows is 15.6 ms.
class HighResTimer {
public:
    HighResTimer() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    }
    ~HighResTimer() {
        if (timer_) CloseHandle(timer_);
    }
    void SleepUntil(std::chrono::steady_clock::time_point t) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (t <= now) return;
        const auto coarse = t - microseconds(700);
        if (timer_ && coarse > now) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(duration_cast<nanoseconds>(coarse - now).count() / 100);
            if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObjectEx(timer_, INFINITE, FALSE);
        }
        while (steady_clock::now() < t) YieldProcessor();
    }

private:
    HANDLE timer_ = nullptr;
};

constexpr double kCoreFps = 60.0;
constexpr uint32_t kCoreSampleRate = 32728;

std::string Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond);
    return b;
}

std::string SafeFileName(std::string s) {
    for (char& c : s)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32) c = '_';
    return s.empty() ? "game" : s;
}

} // namespace

EmulatorSession& EmulatorSession::Get() {
    static EmulatorSession session;
    return session;
}

EmulatorSession::EmulatorSession() = default;

bool EmulatorSession::Initialize(std::string& error) {
    std::lock_guard lock(init_mutex_);
    if (ready_) return true;
    if (!probe_.notes.empty() || probe_.driver_loaded) {
        error = probe_.notes.empty() ? "Graphics initialisation failed" : probe_.notes.front();
        return false;
    }
    azahar_dir_ = JoinPath(Paths().azahar_root, "Azahar") + "/";
    UwpFileSystem fs;
    fs.CreateDirs(azahar_dir_);
    FileUtil::SetUserPath(azahar_dir_);
    FileUtil::SetPathTranslator(&EmulatorSession::TranslatePath);

    if (!presenter_.Initialize(error)) {
        probe_.notes.push_back(error);
        return false;
    }
    if (!dozen_.Load(error)) {
        probe_.notes.push_back(error);
        return false;
    }
    vulkan_ = std::make_unique<VulkanHost>(dozen_, presenter_);
    if (!vulkan_->CreateInstance(probe_, error)) {
        if (probe_.notes.empty()) probe_.notes.push_back(error);
        return false;
    }
    ready_ = true;
    return true;
}

void EmulatorSession::SetEvents(SessionEvents events) {
    std::lock_guard lock(events_mutex_);
    events_ = std::move(events);
}

std::string EmulatorSession::TranslatePath(const std::string& path) {
    return RemapForAzahar(path);
}

void EmulatorSession::UpdateFolders(const FolderConfig& folders) {
    g_remapper.store(std::make_shared<const PathRemapper>(azahar_dir_.empty()
                                                              ? JoinPath(Paths().azahar_root, "Azahar")
                                                              : azahar_dir_,
                                                          folders));
}

std::string EmulatorSession::AzaharUserDir() const {
    return azahar_dir_;
}

void EmulatorSession::Message(const std::string& text) {
    ONYX_INFO("message: %s", text.c_str());
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard lock(events_mutex_);
        if (state_ == SessionState::Starting) startup_messages_.push_back(text);
        cb = events_.message;
    }
    if (cb) cb(text);
}

// ---------------------------------------------------------------------------
// Lifecycle

bool EmulatorSession::Start(const GameEntry& game, const Settings& settings, ConsoleModel model,
                            std::string& error) {
    if (!ready_) {
        error = "Graphics are not available. Open Settings > System check for details.";
        return false;
    }
    if (state_ != SessionState::Idle && state_ != SessionState::Failed) {
        error = "A game is already running";
        return false;
    }
    if (core_crashed_) {
        error = "The emulator stopped after an error earlier. Close and reopen ONYX 3DS to play again.";
        return false;
    }
    if (thread_.joinable()) thread_.join();

    game_ = game;
    model_ = model;
    {
        std::lock_guard lock(options_mutex_);
        options_ = settings.EffectiveCoreOptions(model, game.TitleIdHex());
        catalog_ = {};
    }
    UpdateFolders(settings.folders);
    ff_speed_ = settings.qol.fast_forward_speed;
    fast_forward_ = false;
    stop_requested_ = false;
    pause_requested_ = false;
    write_auto_on_stop_ = false;
    presenter_.SetFilter(settings.qol.screen_filter);
    presenter_.SetPaused(false);
    presenter_.SetDimmed(false);
    state_ = SessionState::Starting;
    thread_ = std::thread([this, path = game.path] { EmulationThread(path); });
    return true;
}

void EmulatorSession::Stop(bool write_auto_state) {
    if (state_ == SessionState::Idle) return;
    write_auto_on_stop_ = write_auto_state;
    stop_requested_ = true;
    pause_requested_ = false;
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) thread_.join();
}

void EmulatorSession::SetPaused(bool paused) {
    pause_requested_ = paused;
    presenter_.SetDimmed(paused);
    audio_.SetMuted(paused);
}

u64 EmulatorSession::SessionSeconds() const {
    if (state_ == SessionState::Idle) return 0;
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - started_at_)
                                .count());
}

void EmulatorSession::Post(std::function<void()> command) {
    std::lock_guard lock(command_mutex_);
    commands_.push_back(std::move(command));
}

void EmulatorSession::RunCommands() {
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard lock(command_mutex_);
        pending.swap(commands_);
    }
    for (auto& c : pending) c();
}

void EmulatorSession::EmulationThread(std::string rom_path) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    InstallThreadCrashHooks();
    LogMemoryUsage("before loading the game");
    WatchMemoryFor(15); // an out-of-memory kill shows as usage reaching the limit
    crashed_in_jit_ = false;

    // Everything that calls into the core runs under a crash guard: a fault in
    // Azahar, dynarmic or the driver stops the game with an error on screen
    // instead of closing the app.
    struct Ctx {
        EmulatorSession* self;
        const std::string* path;
        std::string error; // a C++ exception escaping the body
    } ctx{this, &rom_path, {}};
    GuardedCrash crash{};
    const bool ok = RunGuarded(
        [](void* p) {
            auto* c = static_cast<Ctx*>(p);
            try {
                c->self->EmulationThreadBody(*c->path);
            } catch (const std::exception& e) {
                c->error = e.what();
            } catch (...) {
                c->error = "unknown error";
            }
        },
        &ctx, &crash);
    if (!ok) {
        OnCoreCrashed(crash);
    } else if (!ctx.error.empty()) {
        std::snprintf(crash.what, sizeof(crash.what), "error: %s", ctx.error.substr(0, 80).c_str());
        std::snprintf(crash.where, sizeof(crash.where), "the emulator core");
        OnCoreCrashed(crash);
    }
}

void EmulatorSession::OnCoreCrashed(const GuardedCrash& crash) {
    DrainStderrToLog(); // the driver's own explanation, if it gave one
    core_crashed_ = true;
    crashed_in_jit_ = crash.in_jit;
    ONYX_ERROR("Emulator crashed: %s at %s%s", crash.what, crash.where,
               crash.in_jit ? " (CPU JIT)" : "");
    // Only our own pieces are cleaned up; the core is left alone.
    try {
        audio_.Stop();
        presenter_.SetPaused(false);
    } catch (...) {
    }
    state_ = SessionState::Failed;
    std::string reason = std::string("The emulator hit an error (") + crash.what + " in " +
                         crash.where + ") and the game was stopped.";
    if (crash.in_jit)
        reason += "\n\nIt happened in the CPU JIT, so ONYX switched the CPU to the interpreter "
                  "(Settings > CPU JIT). Games will run slower until you switch it back.";
    reason += "\n\nClose and reopen ONYX 3DS before starting another game.";
    std::function<void(const std::string&)> stopped;
    {
        std::lock_guard lock(events_mutex_);
        stopped = events_.stopped;
    }
    if (stopped) stopped(reason);
}

void EmulatorSession::EmulationThreadBody(const std::string& rom_path) {

    std::string fail_reason;
    auto fail = [&](const std::string& why) {
        ONYX_ERROR("Game failed to start: %s", why.c_str());
        fail_reason = why;
    };

    retro_set_environment(&EnvCb);
    retro_set_video_refresh(&VideoCb);
    retro_set_audio_sample(&AudioSampleCb);
    retro_set_audio_sample_batch(&AudioBatchCb);
    retro_set_input_poll(&InputPollCb);
    retro_set_input_state(&InputStateCb);
    if (!core_initialised_) {
        retro_init();
        core_initialised_ = true;
    }

    hw_render_set_ = false;
    memory_map_.clear();
    {
        std::lock_guard lock(events_mutex_);
        startup_messages_.clear();
    }
    // The core explains load failures (encrypted ROM, missing system files)
    // through SET_MESSAGE; use its words when it gives up.
    auto core_reason = [this](const std::string& fallback) {
        std::lock_guard lock(events_mutex_);
        for (auto it = startup_messages_.rbegin(); it != startup_messages_.rend(); ++it) {
            const std::string m = ToLower(*it);
            if (m.find("fail") != std::string::npos || m.find("decrypt") != std::string::npos ||
                m.find("not supported") != std::string::npos ||
                m.find("unable") != std::string::npos || m.find("error") != std::string::npos)
                return *it;
        }
        return fallback;
    };
    retro_game_info info{};
    info.path = rom_path.c_str();
    bool loaded = retro_load_game(&info);
    std::string err;
    if (!loaded) {
        fail(core_reason("The emulator could not open this game. Is it a decrypted dump?"));
    } else if (!hw_render_set_) {
        fail("The emulator did not request Vulkan rendering");
    } else if (!vulkan_->CreateDevice(err)) {
        fail(err);
    } else {
        retro_system_av_info av{};
        retro_get_system_av_info(&av);
        audio_.Start(av.timing.sample_rate > 0 ? static_cast<uint32_t>(av.timing.sample_rate)
                                               : kCoreSampleRate);
        if (hw_render_.context_reset) hw_render_.context_reset();
        // context_reset is where Azahar actually boots the game.
        const std::string reason = core_reason({});
        if (!reason.empty()) fail(reason);
    }

    DrainStderrToLog();
    if (!fail_reason.empty()) {
        if (loaded) retro_unload_game();
        vulkan_->DestroyDevice();
        audio_.Stop();
        state_ = SessionState::Failed;
        std::function<void(const std::string&)> stopped;
        {
            std::lock_guard lock(events_mutex_);
            stopped = events_.stopped;
        }
        if (stopped) stopped(fail_reason);
        return;
    }

    // Achievements: identify the game in the background; memory reads come
    // from the core's memory map on this thread.
    if (ra_) {
        ra_->SetMemoryReader([this](u32 a, u8* b, u32 n) { return ReadMemory(a, b, n); });
        ra_->LoadGame(rom_path, [this](bool ok, const std::string& msg) {
            if (ok && ra_->GameLoaded()) Message(ra_->Summary());
            else if (!msg.empty()) ONYX_INFO("RetroAchievements: %s", msg.c_str());
        });
    }

    started_at_ = std::chrono::steady_clock::now();
    state_ = SessionState::Running;
    {
        std::function<void()> started;
        {
            std::lock_guard lock(events_mutex_);
            started = events_.started;
        }
        if (started) started();
    }

    HighResTimer timer;
    auto deadline = std::chrono::steady_clock::now();
    while (!stop_requested_) {
        RunCommands();
        if (pause_requested_) {
            state_ = SessionState::Paused;
            presenter_.SetPaused(true);
            if (ra_) ra_->Idle();
            timer.SleepUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(16));
            deadline = std::chrono::steady_clock::now();
            continue;
        }
        if (state_ == SessionState::Paused) {
            state_ = SessionState::Running;
            presenter_.SetPaused(false);
        }
        retro_run();
        if (ra_ && ra_->GameLoaded()) ra_->DoFrame();
        if (++frames_since_drain_ >= 120) { // ~2 s: surface driver warnings
            frames_since_drain_ = 0;
            DrainStderrToLog();
        }

        // Frame pacing: 60 Hz (or the fast-forward multiple) on a precise timer.
        const int ff = ff_speed_.load();
        const bool ffw = fast_forward_.load();
        if (ffw && ff <= 0) {
            deadline = std::chrono::steady_clock::now();
        } else {
            const double speed = ffw ? ff / 100.0 : 1.0;
            deadline += std::chrono::nanoseconds(static_cast<long long>(1e9 / kCoreFps / speed));
            const auto now = std::chrono::steady_clock::now();
            if (now > deadline + std::chrono::milliseconds(50)) deadline = now; // too far behind
            else timer.SleepUntil(deadline);
        }
        // Keep ~60 ms of audio queued by nudging playback speed by <=0.5%.
        if (!ffw && audio_.SampleRate()) {
            const double target = audio_.SampleRate() * 0.06;
            const double queued = static_cast<double>(audio_.QueuedFrames());
            audio_.SetRateNudge(1.0 + 0.005 * std::clamp((queued - target) / target, -1.0, 1.0));
        }
    }

    state_ = SessionState::Stopping;
    RunCommands();
    if (write_auto_on_stop_) DoSaveState(0);
    if (ra_) ra_->UnloadGame();
    retro_unload_game();
    if (hw_render_.context_destroy) hw_render_.context_destroy();
    vulkan_->DestroyDevice();
    audio_.Stop();
    presenter_.SetPaused(false);
    state_ = SessionState::Idle;

    std::function<void(const std::string&)> stopped;
    {
        std::lock_guard lock(events_mutex_);
        stopped = events_.stopped;
    }
    if (stopped) stopped({});
}

// ---------------------------------------------------------------------------
// Commands

void EmulatorSession::Reset() {
    Post([this] {
        retro_reset();
        if (ra_) ra_->Reset();
        Message("Reset");
    });
}

std::string EmulatorSession::StatePath(int slot) const {
    const std::string dir = JoinPath(Paths().local_state, "states/" + game_.TitleIdHex());
    return JoinPath(dir, slot == 0 ? "auto.state" : "slot" + std::to_string(slot) + ".state");
}

std::string EmulatorSession::StateThumbPath(int slot) const {
    std::string p = StatePath(slot);
    return p.substr(0, p.size() - 6) + ".png";
}

bool EmulatorSession::HasState(int slot) const {
    UwpFileSystem fs;
    return fs.Exists(StatePath(slot));
}

void EmulatorSession::SaveState(int slot) {
    Post([this, slot] { DoSaveState(slot); });
}

void EmulatorSession::LoadState(int slot) {
    Post([this, slot] { DoLoadState(slot); });
}

void EmulatorSession::DoSaveState(int slot) {
    if (ra_ && ra_->Hardcore() && slot != 0) {
        Message("Save states are off in RetroAchievements hardcore mode");
        return;
    }
    const size_t size = retro_serialize_size();
    if (size == 0) {
        Message("This game cannot be saved right now");
        return;
    }
    std::vector<uint8_t> data(size);
    if (!retro_serialize(data.data(), size)) {
        Message("Saving the state failed");
        return;
    }
    UwpFileSystem fs;
    const std::string path = StatePath(slot);
    fs.CreateDirs(path.substr(0, path.find_last_of('/')));
    if (!fs.WriteAll(path, data)) {
        Message("Could not write the save state");
        return;
    }
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (presenter_.CaptureLatest(rgba, w, h)) {
        Bytes png;
        if (EncodePng(rgba, w, h, png)) fs.WriteAll(StateThumbPath(slot), png);
    }
    if (slot != 0) Message("Saved to slot " + std::to_string(slot));
}

void EmulatorSession::DoLoadState(int slot) {
    if (ra_ && ra_->Hardcore()) {
        Message("Loading states is off in RetroAchievements hardcore mode");
        return;
    }
    UwpFileSystem fs;
    const Bytes data = fs.ReadAll(StatePath(slot));
    if (data.empty()) {
        Message(slot == 0 ? "No auto save yet" : "Slot " + std::to_string(slot) + " is empty");
        return;
    }
    if (!retro_unserialize(data.data(), data.size())) {
        Message("That save state belongs to a different version of the game or emulator");
        return;
    }
    if (ra_) ra_->Reset();
    Message(slot == 0 ? "Resumed where you left off" : "Loaded slot " + std::to_string(slot));
}

void EmulatorSession::SetFastForward(bool on) {
    fast_forward_ = on;
    audio_.SetMuted(on || pause_requested_);
}

void EmulatorSession::ApplyCoreOptions(const CoreOptions& options) {
    std::lock_guard lock(options_mutex_);
    for (const auto& [k, v] : options) options_[k] = v;
    options_[keys::kGraphicsApi] = "Vulkan";
    options_dirty_ = true;
}

CoreOptions EmulatorSession::CurrentCoreOptions() const {
    std::lock_guard lock(options_mutex_);
    return options_;
}

CoreOptionCatalog EmulatorSession::Catalog() const {
    std::lock_guard lock(options_mutex_);
    return catalog_;
}

void EmulatorSession::CycleLayout() {
    static const char* layouts[] = {"large_screen", "default", "side_by_side", "single_screen"};
    std::string current;
    {
        std::lock_guard lock(options_mutex_);
        current = options_.count(keys::kLayout) ? options_[keys::kLayout] : "large_screen";
    }
    size_t i = 0;
    for (; i < std::size(layouts); ++i)
        if (current == layouts[i]) break;
    const char* next = layouts[(i + 1) % std::size(layouts)];
    ApplyCoreOptions({{keys::kLayout, next}});
    static const std::map<std::string, std::string> names = {
        {"large_screen", "Big top screen"}, {"default", "Stacked"},
        {"side_by_side", "Side by side"}, {"single_screen", "Single screen"}};
    Message("Layout: " + names.at(next));
}

void EmulatorSession::ApplyCheats(const CheatFile& cheats) {
    Post([this, cheats] {
        retro_cheat_reset();
        unsigned index = 0;
        int enabled = 0;
        for (const auto& c : cheats.cheats) {
            if (!c.enabled || !c.IsValid()) continue;
            std::string code;
            for (const auto& l : c.lines) code += l + "\n";
            retro_cheat_set(index++, true, code.c_str());
            ++enabled;
        }
        Message(enabled ? std::to_string(enabled) + (enabled == 1 ? " cheat on" : " cheats on")
                        : "Cheats off");
    });
}

void EmulatorSession::Screenshot() {
    RunAsync([this] {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!presenter_.CaptureLatest(rgba, w, h)) return;
        Bytes png;
        if (!EncodePng(rgba, w, h, png)) return;
        UwpFileSystem fs;
        auto remapper = g_remapper.load();
        std::string dir = remapper ? remapper->Map(azahar_dir_ + "screenshots") : "";
        if (dir.empty() || dir == azahar_dir_ + "screenshots")
            dir = JoinPath(Paths().local_state, "screenshots");
        fs.CreateDirs(dir);
        const std::string path =
            JoinPath(dir, SafeFileName(game_.DisplayTitle()) + " " + Timestamp() + ".png");
        if (fs.WriteAll(path, png)) Message("Screenshot saved");
    });
}

bool EmulatorSession::InstallCia(const std::string& path, const std::function<void(double)>& progress,
                                 std::string& message) {
    if (state_ != SessionState::Idle && state_ != SessionState::Failed) {
        message = "Close the game before installing updates or DLC";
        return false;
    }
    FileUtil::SetUserPath(azahar_dir_);
    const auto status = Service::AM::InstallCIA(path, [&](std::size_t done, std::size_t total) {
        if (progress && total) progress(static_cast<double>(done) / static_cast<double>(total));
    });
    switch (status) {
    case Service::AM::InstallStatus::Success: message = "Installed"; return true;
    case Service::AM::InstallStatus::ErrorEncrypted:
        message = "This CIA is encrypted. Decrypt it on your 3DS (GodMode9) first."; break;
    case Service::AM::InstallStatus::ErrorFileNotFound:
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        message = "The file could not be opened"; break;
    case Service::AM::InstallStatus::ErrorAborted: message = "Installation cancelled"; break;
    default: message = "Not a valid CIA file"; break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// libretro callbacks

void EmulatorSession::VideoRefresh(const void* data, unsigned width, unsigned height, size_t) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID && vulkan_) vulkan_->OnFrame(width, height);
    // nullptr = duplicate frame: the presenter keeps showing the last one.
}

size_t EmulatorSession::AudioBatch(const int16_t* data, size_t frames) {
    if (!fast_forward_) audio_.Push(data, frames);
    return frames;
}

void EmulatorSession::InputPoll() {
    for (Hotkey h : input_.Poll()) {
        switch (h) {
        case Hotkey::SaveState: Post([this] { DoSaveState(slot_); }); break;
        case Hotkey::LoadState: Post([this] { DoLoadState(slot_); }); break;
        case Hotkey::PrevSlot:
            slot_ = slot_ <= 1 ? 9 : slot_ - 1;
            Message("State slot " + std::to_string(slot_.load()));
            break;
        case Hotkey::NextSlot:
            slot_ = slot_ >= 9 ? 1 : slot_ + 1;
            Message("State slot " + std::to_string(slot_.load()));
            break;
        case Hotkey::CycleLayout: CycleLayout(); break;
        case Hotkey::Screenshot: Screenshot(); break;
        default: {
            // Menu, fast forward and FPS are UI decisions.
            std::function<void(Hotkey)> cb;
            {
                std::lock_guard lock(events_mutex_);
                cb = events_.hotkey;
            }
            if (cb) cb(h);
        }
        }
    }
}

int16_t EmulatorSession::InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    return input_.Query(device, index, id);
}

uint32_t EmulatorSession::ReadMemory(uint32_t address, uint8_t* buffer, uint32_t n) {
    // RetroAchievements addresses for 3DS are emulated virtual addresses,
    // which is exactly how Azahar describes its memory map.
    uint32_t done = 0;
    while (done < n) {
        const uint64_t a = static_cast<uint64_t>(address) + done;
        const retro_memory_descriptor* hit = nullptr;
        for (const auto& d : memory_map_) {
            if (d.ptr && a >= d.start && a < d.start + d.len) {
                hit = &d;
                break;
            }
        }
        if (!hit) break;
        const uint64_t avail = hit->start + hit->len - a;
        const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(avail, n - done));
        std::memcpy(buffer + done, static_cast<const uint8_t*>(hit->ptr) + hit->offset + (a - hit->start),
                    take);
        done += take;
    }
    return done;
}

void EmulatorSession::ParseOptionsV2(const retro_core_options_v2* opts) {
    CoreOptionCatalog cat;
    if (opts->categories)
        for (auto* c = opts->categories; c->key; ++c)
            cat.categories.push_back({c->key, c->desc ? c->desc : c->key, c->info ? c->info : ""});
    for (auto* d = opts->definitions; d && d->key; ++d) {
        CoreOptionDef def;
        def.key = d->key;
        def.label = d->desc_categorized ? d->desc_categorized : (d->desc ? d->desc : d->key);
        def.info = d->info_categorized ? d->info_categorized : (d->info ? d->info : "");
        def.category = d->category_key ? d->category_key : "";
        def.default_value = d->default_value ? d->default_value : "";
        for (const auto& v : d->values) {
            if (!v.value) break;
            def.values.push_back({v.value, v.label ? v.label : v.value});
        }
        if (def.default_value.empty() && !def.values.empty()) def.default_value = def.values[0].value;
        cat.options.push_back(std::move(def));
    }
    std::lock_guard lock(options_mutex_);
    catalog_ = std::move(cat);
}

void EmulatorSession::ParseOptionsV1(const retro_core_option_definition* defs) {
    CoreOptionCatalog cat;
    for (auto* d = defs; d && d->key; ++d) {
        CoreOptionDef def;
        def.key = d->key;
        def.label = d->desc ? d->desc : d->key;
        def.info = d->info ? d->info : "";
        def.default_value = d->default_value ? d->default_value : "";
        for (const auto& v : d->values) {
            if (!v.value) break;
            def.values.push_back({v.value, v.label ? v.label : v.value});
        }
        if (def.default_value.empty() && !def.values.empty()) def.default_value = def.values[0].value;
        cat.options.push_back(std::move(def));
    }
    std::lock_guard lock(options_mutex_);
    catalog_ = std::move(cat);
}

void EmulatorSession::ParseVariables(const retro_variable* vars) {
    // v0: "Description; value1|value2|..."
    CoreOptionCatalog cat;
    for (auto* v = vars; v && v->key; ++v) {
        CoreOptionDef def;
        def.key = v->key;
        const std::string text = v->value ? v->value : "";
        const auto semi = text.find("; ");
        def.label = semi == std::string::npos ? def.key : text.substr(0, semi);
        if (semi != std::string::npos)
            for (const auto& val : Split(text.substr(semi + 2), '|')) def.values.push_back({val, val});
        if (!def.values.empty()) def.default_value = def.values[0].value;
        cat.options.push_back(std::move(def));
    }
    std::lock_guard lock(options_mutex_);
    catalog_ = std::move(cat);
}

bool EmulatorSession::Environment(unsigned cmd, void* data) {
    static std::string system_dir;
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        system_dir = Paths().azahar_root;
        *static_cast<const char**>(data) = system_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<const retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_VULKAN;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto* cb = static_cast<retro_hw_render_callback*>(data);
        if (cb->context_type != RETRO_HW_CONTEXT_VULKAN) return false; // only Vulkan on Xbox
        hw_render_ = *cb;
        hw_render_set_ = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        if (!vulkan_) return false;
        vulkan_->SetNegotiationInterface(
            static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data));
        return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT: {
        auto* q = static_cast<retro_hw_render_context_negotiation_interface*>(data);
        if (q->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) {
            q->interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        if (!vulkan_ || !vulkan_->HasDevice()) return false;
        *static_cast<const retro_hw_render_interface**>(data) =
            reinterpret_cast<const retro_hw_render_interface*>(vulkan_->Interface());
        return true;
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = &CoreLogCb;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto* m = static_cast<const retro_message*>(data);
        if (m && m->msg) Message(m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        if (data) ParseOptionsV2(static_cast<const retro_core_options_v2*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        const auto* intl = static_cast<const retro_core_options_v2_intl*>(data);
        if (intl && intl->us) ParseOptionsV2(intl->us);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        ParseOptionsV1(static_cast<const retro_core_option_definition*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        ParseVariables(static_cast<const retro_variable*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* var = static_cast<retro_variable*>(data);
        if (!var || !var->key) return false;
        std::lock_guard lock(options_mutex_);
        const std::string key = var->key;
        auto it = options_.find(key);
        if (it == options_.end()) {
            const CoreOptionDef* def = catalog_.Find(key);
            if (!def) return false;
            it = options_.emplace(key, def->default_value).first;
        }
        var->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = options_dirty_.exchange(false);
        return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
        const auto* map = static_cast<const retro_memory_map*>(data);
        memory_map_.assign(map->descriptors, map->descriptors + map->num_descriptors);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
        *static_cast<bool*>(data) = true; // codeGeneration capability is declared
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_USERNAME:
        *static_cast<const char**>(data) = "ONYX";
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *static_cast<int*>(data) = 3;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *static_cast<bool*>(data) = fast_forward_;
        return true;
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
        *static_cast<float*>(data) = 60.0f;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        stop_requested_ = true;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        return true;
    // Not provided on Xbox: the core falls back on its own.
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
    case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE:
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
    case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
    default:
        return false;
    }
}

} // namespace onyx::app
