#include <doctest/doctest.h>

#include "helpers.h"
#include "onyx/achievements.h"
#include "onyx/steamgriddb.h"

using namespace onyx;
using namespace testutil;

TEST_CASE("SteamGridDB: search, best match and art download") {
    TempDir art;
    StdFileSystem fs;
    FakeHttp http;
    const std::string base = "https://www.steamgriddb.com/api/v2";
    http.routes[base + "/search/autocomplete/Kart%20Racer"] = {200, R"({"success":true,"data":[
        {"id":11,"name":"Kart Racer Party","verified":true},
        {"id":22,"name":"Kart Racer","verified":false}]})", ""};
    http.routes[base + "/grids/game/22?dimensions=920x430,460x215&mimes=image/png,image/jpeg"
                       "&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[{"id":1,"score":1,"url":"https://cdn/low.png","mime":"image/png"},
                                         {"id":2,"score":9,"url":"https://cdn/high.jpg","mime":"image/jpeg"}]})", ""};
    http.routes[base + "/heroes/game/22?mimes=image/png,image/jpeg&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[]})", ""};
    http.routes[base + "/logos/game/22?mimes=image/png&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[{"id":3,"url":"https://cdn/logo.png","mime":"image/png"}]})", ""};
    http.routes["https://cdn/high.jpg"] = {200, "JPEGDATA", ""};
    http.routes["https://cdn/logo.png"] = {200, "PNGDATA", ""};

    SteamGridDb sgdb(http, "  key123  ");
    REQUIRE(sgdb.HasKey());
    const auto results = sgdb.Search("Kart Racer");
    REQUIRE(results.size() == 2);
    CHECK(SteamGridDb::BestMatch(results, "Kart Racer™")->id == 22);
    CHECK(http.log.back().headers.at("Authorization") == "Bearer key123");

    GameEntry g;
    g.title = "Kart Racer";
    g.title_id = 0x0004000000030800ull;
    ArtScraper scraper(sgdb, fs, art.Str());
    CHECK(scraper.Scrape(g));
    CHECK(g.grid_art == art.Str() + "/0004000000030800_grid.jpg"); // highest score wins
    CHECK(fs.ReadText(g.grid_art) == "JPEGDATA");
    CHECK(g.hero_art.empty());
    CHECK(fs.ReadText(g.logo_art) == "PNGDATA");
    // The CDN download must not carry the API key.
    for (const auto& r : http.log)
        if (r.url.starts_with("https://cdn/")) CHECK(r.headers.count("Authorization") == 0);
}

TEST_CASE("SteamGridDB without a key or with a rejected key does nothing") {
    FakeHttp http;
    SteamGridDb none(http, "");
    CHECK(none.Search("x").empty());
    CHECK(http.log.empty());
    SteamGridDb bad(http, "k");
    http.routes["https://www.steamgriddb.com/api/v2/search/autocomplete/x"] = {401, "", ""};
    CHECK(bad.Search("x").empty());
    CHECK(bad.LastStatus() == 401);
}

TEST_CASE("BestMatch prefers exact, then prefix, then verified") {
    std::vector<SgdbGame> r = {{1, "Other", false}, {2, "Pokemon X and Y", false}, {3, "Verified", true}};
    CHECK(SteamGridDb::BestMatch(r, "Pokemon X")->id == 2);
    CHECK(SteamGridDb::BestMatch(r, "other")->id == 1);
    CHECK(SteamGridDb::BestMatch(r, "Nothing")->id == 3);
    CHECK_FALSE(SteamGridDb::BestMatch({}, "x").has_value());
}

TEST_CASE("RetroAchievements: token login goes through our HTTP client") {
    // rcheevos builds the URL itself; answer any request with a login success.
    struct AnyHttp final : IHttpClient {
        std::vector<HttpRequest> log;
        HttpResponse Send(const HttpRequest& r) override {
            log.push_back(r);
            return {200, R"({"Success":true,"User":"player","DisplayName":"Player","Token":"tok123",
                           "Score":10,"SoftcoreScore":0,"Messages":0})", ""};
        }
    } any;
    Achievements ra(any, [](std::function<void()> f) { f(); });
    bool done = false, ok = false;
    ra.LoginWithToken("player", "tok123", [&](bool success, const std::string&) {
        done = true;
        ok = success;
    });
    CHECK(done);
    CHECK(ok);
    CHECK(ra.LoggedIn());
    CHECK(ra.UserName() == "Player");
    CHECK(ra.Token() == "tok123");
    REQUIRE_FALSE(any.log.empty());
    CHECK(any.log[0].url.find("retroachievements.org") != std::string::npos);
    CHECK(any.log[0].method == "POST");
    CHECK(any.log[0].headers.at("User-Agent").starts_with("ONYX3DS/0.1"));
    CHECK(ra.Summary() == "No achievement set for this game");
    ra.SetHardcore(true);
    CHECK(ra.Hardcore());
    ra.Logout();
    CHECK_FALSE(ra.LoggedIn());
}

TEST_CASE("RetroAchievements: offline login reports failure") {
    struct Offline final : IHttpClient {
        HttpResponse Send(const HttpRequest&) override { return {0, "", "offline"}; }
    } offline;
    Achievements ra(offline, [](std::function<void()> f) { f(); });
    bool done = false, ok = true;
    ra.LoginWithPassword("player", "pw", [&](bool success, const std::string&) {
        done = true;
        ok = success;
    });
    // rcheevos retries client errors on its own schedule; it must not succeed.
    CHECK_FALSE((ok && done));
    CHECK_FALSE(ra.LoggedIn());
}
