// SPDX-License-Identifier: GPL-3.0-or-later
//
// RetroAchievements through rcheevos' rc_client.
//
// Status as of October 2026: rcheevos can hash 3DS games (console 62) and the
// site lists 3DS titles, but no 3DS achievement sets are published yet. This
// integration logs in, identifies games and runs whatever sets the server
// returns, so it starts working the day sets go live without an app update.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "onyx/platform.h"

struct rc_client_t;

namespace onyx {

struct AchievementInfo {
    u32 id = 0;
    std::string title;
    std::string description;
    std::string badge_url;
    u32 points = 0;
    bool unlocked = false;
    float progress = 0.0f; // 0-1 when the set tracks progress
};

struct AchievementEvent {
    enum class Kind {
        Unlocked, GameCompleted, LeaderboardStarted, LeaderboardFailed,
        LeaderboardSubmitted, ServerError, Disconnected, Reconnected, ResetRequired,
    } kind;
    std::string title;
    std::string detail;
    std::string badge_url;
};

class Achievements {
public:
    using ReadMemory = std::function<u32(u32 address, u8* buffer, u32 num_bytes)>;
    using EventSink = std::function<void(const AchievementEvent&)>;
    using Done = std::function<void(bool ok, const std::string& message)>;
    // Runs blocking HTTP calls off the calling thread. The app passes a thread
    // pool dispatcher; tests run inline.
    using Dispatcher = std::function<void(std::function<void()>)>;

    Achievements(IHttpClient& http, Dispatcher dispatch);
    ~Achievements();
    Achievements(const Achievements&) = delete;
    Achievements& operator=(const Achievements&) = delete;

    void SetMemoryReader(ReadMemory reader);
    void SetEventSink(EventSink sink);
    // rcheevos hashes ROM files itself; route its reads through our VFS so USB
    // paths work under the UWP sandbox.
    static void UseFileSystem(IFileSystem* fs);

    void LoginWithPassword(const std::string& user, const std::string& password, Done done);
    void LoginWithToken(const std::string& user, const std::string& token, Done done);
    void Logout();
    bool LoggedIn() const;
    std::string UserName() const;
    std::string Token() const; // store this, never the password

    void SetHardcore(bool enabled);
    bool Hardcore() const;

    void LoadGame(const std::string& rom_path, Done done);
    void UnloadGame();
    bool GameLoaded() const;
    std::string GameTitle() const;
    std::vector<AchievementInfo> List() const;
    std::string Summary() const; // "3 of 40 achievements, 25 of 400 points"

    // Call once per emulated frame while running, and Idle() while paused.
    void DoFrame();
    void Idle();
    void Reset();

    std::string UserAgent() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace onyx
