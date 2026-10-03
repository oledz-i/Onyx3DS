// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/achievements.h"

#include <cstring>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

namespace onyx {

namespace {

// --- rc_hash file reader over IFileSystem ---------------------------------

IFileSystem* g_hash_fs = nullptr;

struct HashFile {
    std::string path;
    u64 pos = 0;
    u64 size = 0;
};

void* RC_CCONV HashOpen(const char* path) {
    if (!g_hash_fs) return nullptr;
    const auto size = g_hash_fs->Size(path);
    if (!size) return nullptr;
    return new HashFile{path, 0, *size};
}
void RC_CCONV HashSeek(void* h, int64_t offset, int origin) {
    auto* f = static_cast<HashFile*>(h);
    int64_t base = origin == SEEK_CUR ? static_cast<int64_t>(f->pos)
                   : origin == SEEK_END ? static_cast<int64_t>(f->size)
                                        : 0;
    const int64_t target = base + offset;
    f->pos = target < 0 ? 0 : static_cast<u64>(target);
}
int64_t RC_CCONV HashTell(void* h) {
    return static_cast<int64_t>(static_cast<HashFile*>(h)->pos);
}
size_t RC_CCONV HashRead(void* h, void* buffer, size_t n) {
    auto* f = static_cast<HashFile*>(h);
    const Bytes b = g_hash_fs->ReadRange(f->path, f->pos, n);
    std::memcpy(buffer, b.data(), b.size());
    f->pos += b.size();
    return b.size();
}
void RC_CCONV HashClose(void* h) {
    delete static_cast<HashFile*>(h);
}

} // namespace

struct Achievements::Impl {
    IHttpClient& http;
    Dispatcher dispatch;
    rc_client_t* client = nullptr;
    ReadMemory read_memory;
    EventSink sink;
    mutable std::mutex mutex; // guards read_memory / sink swaps

    Impl(IHttpClient& h, Dispatcher d) : http(h), dispatch(std::move(d)) {}

    static Impl& From(rc_client_t* c) {
        return *static_cast<Impl*>(rc_client_get_userdata(c));
    }

    static uint32_t RC_CCONV Read(uint32_t address, uint8_t* buffer, uint32_t n, rc_client_t* c) {
        Impl& self = From(c);
        std::lock_guard lock(self.mutex);
        return self.read_memory ? self.read_memory(address, buffer, n) : 0;
    }

    static void RC_CCONV ServerCall(const rc_api_request_t* request,
                                    rc_client_server_callback_t callback, void* callback_data,
                                    rc_client_t* c) {
        Impl& self = From(c);
        HttpRequest req;
        req.url = request->url;
        if (request->post_data) {
            req.method = "POST";
            req.body = request->post_data;
            req.content_type = request->content_type ? request->content_type
                                                     : "application/x-www-form-urlencoded";
        }
        char clause[128] = {};
        rc_client_get_user_agent_clause(c, clause, sizeof(clause));
        req.headers["User-Agent"] = std::string("ONYX3DS/0.1 ") + clause;
        self.dispatch([&http = self.http, req, callback, callback_data] {
            const HttpResponse r = http.Send(req);
            rc_api_server_response_t resp{};
            resp.body = r.body.c_str();
            resp.body_length = r.body.size();
            // rcheevos treats 0 as "no response"; retryable errors get retried.
            resp.http_status_code = r.status ? r.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            callback(&resp, callback_data);
        });
    }

    static void RC_CCONV OnEvent(const rc_client_event_t* e, rc_client_t* c) {
        Impl& self = From(c);
        EventSink sink;
        {
            std::lock_guard lock(self.mutex);
            sink = self.sink;
        }
        if (!sink) return;
        AchievementEvent ev{};
        auto badge = [](const rc_client_achievement_t* a) {
            char url[256] = {};
            rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url,
                                                sizeof(url));
            return std::string(url);
        };
        switch (e->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            ev.kind = AchievementEvent::Kind::Unlocked;
            ev.title = e->achievement->title;
            ev.detail = e->achievement->description;
            ev.badge_url = badge(e->achievement);
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            ev.kind = AchievementEvent::Kind::GameCompleted;
            ev.title = "Set complete";
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
            ev.kind = AchievementEvent::Kind::LeaderboardStarted;
            ev.title = e->leaderboard->title;
            ev.detail = e->leaderboard->description;
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
            ev.kind = AchievementEvent::Kind::LeaderboardFailed;
            ev.title = e->leaderboard->title;
            break;
        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
            ev.kind = AchievementEvent::Kind::LeaderboardSubmitted;
            ev.title = e->leaderboard->title;
            ev.detail = e->leaderboard->tracker_value ? e->leaderboard->tracker_value : "";
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            ev.kind = AchievementEvent::Kind::ServerError;
            ev.title = "RetroAchievements error";
            ev.detail = e->server_error && e->server_error->error_message
                            ? e->server_error->error_message
                            : "";
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            ev.kind = AchievementEvent::Kind::Disconnected;
            ev.title = "Unlocks will be sent when you're back online";
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            ev.kind = AchievementEvent::Kind::Reconnected;
            ev.title = "Pending unlocks sent";
            break;
        case RC_CLIENT_EVENT_RESET:
            ev.kind = AchievementEvent::Kind::ResetRequired;
            ev.title = "Hardcore mode enabled: restarting the game";
            break;
        default:
            return; // trackers and indicators are not shown yet
        }
        sink(ev);
    }
};

struct DoneBox {
    Achievements::Done done;
};

static void RC_CCONV DoneTrampoline(int result, const char* error, rc_client_t*, void* ud) {
    std::unique_ptr<DoneBox> box(static_cast<DoneBox*>(ud));
    if (box->done) box->done(result == RC_OK, error ? error : "");
}

Achievements::Achievements(IHttpClient& http, Dispatcher dispatch)
    : impl_(std::make_unique<Impl>(http, std::move(dispatch))) {
    impl_->client = rc_client_create(&Impl::Read, &Impl::ServerCall);
    rc_client_set_userdata(impl_->client, impl_.get());
    rc_client_set_event_handler(impl_->client, &Impl::OnEvent);
}

Achievements::~Achievements() {
    if (impl_->client) rc_client_destroy(impl_->client);
}

void Achievements::UseFileSystem(IFileSystem* fs) {
    g_hash_fs = fs;
    if (!fs) return;
    static rc_hash_filereader reader{&HashOpen, &HashSeek, &HashTell, &HashRead, &HashClose};
    rc_hash_init_custom_filereader(&reader);
}

void Achievements::SetMemoryReader(ReadMemory reader) {
    std::lock_guard lock(impl_->mutex);
    impl_->read_memory = std::move(reader);
}

void Achievements::SetEventSink(EventSink sink) {
    std::lock_guard lock(impl_->mutex);
    impl_->sink = std::move(sink);
}

void Achievements::LoginWithPassword(const std::string& user, const std::string& password,
                                     Done done) {
    rc_client_begin_login_with_password(impl_->client, user.c_str(), password.c_str(),
                                        &DoneTrampoline, new DoneBox{std::move(done)});
}

void Achievements::LoginWithToken(const std::string& user, const std::string& token, Done done) {
    rc_client_begin_login_with_token(impl_->client, user.c_str(), token.c_str(), &DoneTrampoline,
                                     new DoneBox{std::move(done)});
}

void Achievements::Logout() {
    rc_client_logout(impl_->client);
}

bool Achievements::LoggedIn() const {
    return rc_client_get_user_info(impl_->client) != nullptr;
}

std::string Achievements::UserName() const {
    const auto* u = rc_client_get_user_info(impl_->client);
    return u && u->display_name ? u->display_name : "";
}

std::string Achievements::Token() const {
    const auto* u = rc_client_get_user_info(impl_->client);
    return u && u->token ? u->token : "";
}

void Achievements::SetHardcore(bool enabled) {
    rc_client_set_hardcore_enabled(impl_->client, enabled ? 1 : 0);
}

bool Achievements::Hardcore() const {
    return rc_client_get_hardcore_enabled(impl_->client) != 0;
}

void Achievements::LoadGame(const std::string& rom_path, Done done) {
    rc_client_begin_identify_and_load_game(impl_->client, RC_CONSOLE_NINTENDO_3DS,
                                           rom_path.c_str(), nullptr, 0, &DoneTrampoline,
                                           new DoneBox{std::move(done)});
}

void Achievements::UnloadGame() {
    rc_client_unload_game(impl_->client);
}

bool Achievements::GameLoaded() const {
    return rc_client_get_game_info(impl_->client) != nullptr;
}

std::string Achievements::GameTitle() const {
    const auto* g = rc_client_get_game_info(impl_->client);
    return g && g->title ? g->title : "";
}

std::vector<AchievementInfo> Achievements::List() const {
    std::vector<AchievementInfo> out;
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        impl_->client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return out;
    for (uint32_t b = 0; b < list->num_buckets; ++b) {
        const auto& bucket = list->buckets[b];
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            AchievementInfo info;
            info.id = a->id;
            info.title = a->title ? a->title : "";
            info.description = a->description ? a->description : "";
            info.points = a->points;
            info.unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
            info.progress = a->measured_percent / 100.0f;
            char url[256] = {};
            rc_client_achievement_get_image_url(
                a, info.unlocked ? RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED
                                 : RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE,
                url, sizeof(url));
            info.badge_url = url;
            out.push_back(std::move(info));
        }
    }
    rc_client_destroy_achievement_list(list);
    return out;
}

std::string Achievements::Summary() const {
    if (!GameLoaded()) return "No achievement set for this game";
    rc_client_user_game_summary_t s{};
    rc_client_get_user_game_summary(impl_->client, &s);
    if (s.num_core_achievements == 0) return "No achievements published for this game yet";
    return std::to_string(s.num_unlocked_achievements) + " of " +
           std::to_string(s.num_core_achievements) + " achievements, " +
           std::to_string(s.points_unlocked) + " of " + std::to_string(s.points_core) +
           " points";
}

void Achievements::DoFrame() { rc_client_do_frame(impl_->client); }
void Achievements::Idle() { rc_client_idle(impl_->client); }
void Achievements::Reset() { rc_client_reset(impl_->client); }

std::string Achievements::UserAgent() const {
    char clause[128] = {};
    rc_client_get_user_agent_clause(impl_->client, clause, sizeof(clause));
    return std::string("ONYX3DS/0.1 ") + clause;
}

} // namespace onyx
