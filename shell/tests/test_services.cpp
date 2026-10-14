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

TEST_CASE("SteamGridDB: ranking puts the 3DS game first and parses release years") {
    const auto parsed = SteamGridDb::ParseSearch(
        R"({"success":true,"data":[
            {"id":1,"name":"Super Smash Bros.","verified":true,"release_date":917000000},
            {"id":2,"name":"Super Smash Bros. for Nintendo 3DS","verified":false,"release_date":1411000000},
            {"id":3,"name":"Super Smash Bros. Ultimate","verified":true,"release_date":1543000000}]})");
    REQUIRE(parsed.size() == 3);
    CHECK(parsed[0].year == 1999);
    CHECK(parsed[1].year == 2014);
    const auto ranked = SteamGridDb::Rank(parsed, "Super Smash Bros. for Nintendo 3DS");
    REQUIRE(ranked.size() == 3);
    CHECK(ranked[0].id == 2);
    CHECK(ranked.back().id == 1); // the 1999 game goes last
    // No years and no 3DS hint: exact name still wins over a longer one.
    const auto flat = SteamGridDb::Rank({{5, "Mario Kart 7 Deluxe", true}, {6, "Mario Kart 7", false}}, "Mario Kart 7");
    CHECK(flat.front().id == 6);
}

TEST_CASE("SteamGridDB: a game picked by the user and an image link") {
    TempDir art;
    StdFileSystem fs;
    FakeHttp http;
    const std::string base = "https://www.steamgriddb.com/api/v2";
    http.routes[base + "/grids/game/77?dimensions=920x430,460x215&mimes=image/png,image/jpeg"
                       "&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[{"id":1,"score":3,"url":"https://cdn/a.png","mime":"image/png"}]})", ""};
    http.routes[base + "/heroes/game/77?mimes=image/png,image/jpeg&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[]})", ""};
    http.routes[base + "/logos/game/77?mimes=image/png&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[]})", ""};
    http.routes["https://cdn/a.png"] = {200, std::string("\x89PNG\r\n\x1a\n", 8) + "x", ""};
    http.routes["https://pics/x.jpg"] = {200, std::string("\xFF\xD8\xFF\xE0", 4) + "jpegdata", ""};
    http.routes["https://pics/page.html"] = {200, "<html>not an image</html>", ""};

    SteamGridDb sgdb(http, "key");
    ArtScraper scraper(sgdb, fs, art.Str());
    GameEntry g;
    g.title = "Anything";
    g.title_id = 0x0004000000012300ull;
    CHECK(scraper.ScrapeGame(g, 77, true)); // no search: the user chose id 77
    CHECK(g.grid_art == art.Str() + "/0004000000012300_grid.png");
    CHECK_FALSE(scraper.ScrapeGame(g, 0, true));

    CHECK(scraper.SetGridFromUrl(g, " https://pics/x.jpg "));
    CHECK(g.grid_art == art.Str() + "/0004000000012300_grid.jpg");
    const std::string before = g.grid_art;
    CHECK_FALSE(scraper.SetGridFromUrl(g, "https://pics/page.html")); // not an image
    CHECK_FALSE(scraper.SetGridFromUrl(g, "not a link"));
    CHECK(g.grid_art == before);
}

TEST_CASE("SteamGridDB: misses are remembered and expire; grids fall back to any size") {
    TempDir dir;
    StdFileSystem fs;
    ArtMissCache cache;
    cache.Add("0004000000012300", 1000);
    cache.Save(fs, dir.Str() + "/misses.txt");
    ArtMissCache loaded;
    loaded.Load(fs, dir.Str() + "/misses.txt");
    CHECK(loaded.RecentlyMissed("0004000000012300", 2000));
    CHECK_FALSE(loaded.RecentlyMissed("other", 2000));
    CHECK_FALSE(loaded.RecentlyMissed("0004000000012300", 1000 + ArtMissCache::kRetrySeconds + 1));
    loaded.Forget("0004000000012300");
    CHECK_FALSE(loaded.RecentlyMissed("0004000000012300", 2000));

    FakeHttp http;
    const std::string base = "https://www.steamgriddb.com/api/v2";
    http.routes[base + "/grids/game/9?dimensions=920x430,460x215&mimes=image/png,image/jpeg"
                       "&types=static&nsfw=false&humor=false"] = {200, R"({"success":true,"data":[]})", ""};
    http.routes[base + "/grids/game/9?mimes=image/png,image/jpeg&types=static&nsfw=false&humor=false"] =
        {200, R"({"success":true,"data":[{"id":4,"score":1,"url":"https://cdn/b.png","mime":"image/png"}]})", ""};
    SteamGridDb sgdb(http, "key");
    const auto images = sgdb.Images(SgdbArt::Grid, 9);
    REQUIRE(images.size() == 1);
    CHECK(images[0].url == "https://cdn/b.png");
}

#include "onyx/game_info.h"

TEST_CASE("Game info: Wikipedia infobox, release date and screenshot names") {
    CHECK(GameInfoService::CleanWikiText("[[Nintendo EAD Tokyo|Nintendo EAD]]") == "Nintendo EAD");
    CHECK(GameInfoService::CleanWikiText("{{Plainlist|\n* [[Nintendo]]\n* [[Monolith Soft]]\n}}") == "Nintendo, Monolith Soft");
    CHECK(GameInfoService::CleanWikiText("[[Platform game|Platformer]]<ref>x</ref>") == "Platformer");
    CHECK(GameInfoService::ReleaseDate("{{Video game release|JP|November 3, 2011|NA|November 13, 2011}}") == "November 3, 2011");
    CHECK(GameInfoService::ReleaseDate("{{vgrelease|WW|2011-11-03}}") == "November 3, 2011");
    CHECK(GameInfoService::ReleaseDate("2011") == "2011");

    const auto titles = GameInfoService::ParseSearchTitles(
        R"({"query":{"search":[{"title":"Super Mario 3D Land"},{"title":"Super Mario 3D World"}]}})");
    REQUIRE(titles.size() == 2);
    const std::string page = R"({"query":{"pages":[{"title":"Super Mario 3D Land","extract":"Super Mario 3D Land is a platform game.","revisions":[{"slots":{"main":{"content":
"{{Infobox video game\n| title = Super Mario 3D Land\n| developer = [[Nintendo EAD Tokyo|Nintendo EAD]]\n| publisher = [[Nintendo]]\n| genre = [[Platform game|Platform]]\n| released = {{Video game release|JP|November 3, 2011|NA|November 13, 2011}}\n}}\n"}}}]}]}})";
    const auto articles = GameInfoService::ParseArticles(page);
    REQUIRE(articles.size() == 1);
    const GameInfo info = GameInfoService::FromArticle(articles[0]);
    CHECK(info.developer == "Nintendo EAD");
    CHECK(info.publisher == "Nintendo");
    CHECK(info.genre == "Platform");
    CHECK(info.released == "November 3, 2011");
    CHECK(info.summary.starts_with("Super Mario 3D Land"));

    const auto names = GameInfoService::ParseListing(
        R"(<a href="Super%20Mario%203D%20Land%20%28Europe%29.png">x</a><a href="Super%20Mario%203D%20Land%20%28USA%29.png">y</a><a href="Super%20Mario%203D%20World.png">z</a>)");
    REQUIRE(names.size() == 3);
    CHECK(names[1] == "Super Mario 3D Land (USA)");
    CHECK(GameInfoService::BestSnapName(names, "Super Mario 3D Land") == "Super Mario 3D Land (USA)");
    CHECK(GameInfoService::BestSnapName(names, "Zelda").empty());
}

TEST_CASE("Game info: fetch, cache and remember misses") {
    TempDir dir;
    StdFileSystem fs;
    FakeHttp http;
    const std::string api = "https://en.wikipedia.org/w/api.php";
    http.routes[api + "?action=query&list=search&srlimit=4&format=json&srsearch=Kart%20Racer%20Nintendo%203DS%20video%20game"] =
        {200, R"({"query":{"search":[{"title":"Kart Racer"}]}})", ""};
    http.routes[api + "?action=query&prop=extracts|revisions&exintro=1&explaintext=1&rvprop=content&rvslots=main&redirects=1&format=json&formatversion=2&titles=Kart%20Racer"] =
        {200, R"({"query":{"pages":[{"title":"Kart Racer","extract":"A racing game.","revisions":[{"slots":{"main":{"content":"{{Infobox video game\n| publisher = [[Nintendo]]\n}}"}}}]}]}})", ""};
    http.routes["https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%203DS/Named_Snaps/"] =
        {200, R"(<a href="Kart%20Racer%20%28USA%29.png">a</a>)", ""};
    http.routes["https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%203DS/Named_Snaps/Kart%20Racer%20%28USA%29.png"] =
        {200, std::string("\x89PNG\r\n\x1a\n", 8) + "snap", ""};
    http.routes["https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%203DS/Named_Boxarts/"] =
        {200, R"(<a href="Kart%20Racer%20%28USA%29.png">a</a>)", ""};
    http.routes["https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%203DS/Named_Boxarts/Kart%20Racer%20%28USA%29.png"] =
        {200, std::string("\x89PNG\r\n\x1a\n", 8) + "box", ""};
    GameInfoService svc(http, fs, dir.Str());
    svc.SetFetchBoxArt(true);
    GameEntry g;
    g.title = "Kart Racer";
    g.title_id = 0x0004000000030800ull;
    CHECK_FALSE(svc.Cached(g).found);
    const GameInfo info = svc.Get(g);
    CHECK(info.found);
    CHECK(info.publisher == "Nintendo");
    CHECK(info.shots.size() == 1);
    CHECK_FALSE(info.boxart.empty());
    CHECK(svc.Cached(g).boxart == info.boxart);
    const size_t calls = http.log.size();
    CHECK(svc.Get(g).found); // from disk
    CHECK(http.log.size() == calls);
    CHECK(svc.Cached(g).summary == "A racing game.");

    GameEntry unknown;
    unknown.title = "Nobody Knows";
    unknown.title_id = 0x0004000000099900ull;
    http.routes[api + "?action=query&list=search&srlimit=4&format=json&srsearch=Nobody%20Knows%20Nintendo%203DS%20video%20game"] =
        {200, R"({"query":{"search":[]}})", ""};
    CHECK_FALSE(svc.Get(unknown).found);
    const size_t after = http.log.size();
    CHECK_FALSE(svc.Get(unknown).found);
    CHECK(http.log.size() == after); // the miss is remembered

    // A game with no box in the thumbnail project is asked about once.
    CHECK(svc.EnsureBoxArt(unknown).empty());
    const size_t boxes = http.log.size();
    CHECK(svc.EnsureBoxArt(unknown).empty());
    CHECK(http.log.size() == boxes);
}

TEST_CASE("Game info: critics' score and stars") {
    std::string src;
    CHECK(GameInfoService::ParseScore("{{Video game reviews\n| MC = 85/100\n| Edge = 8/10\n}}", &src) == 85);
    CHECK(src == "Metacritic");
    CHECK(GameInfoService::ParseScore("|MC=94 / 100") == 94);
    CHECK(GameInfoService::ParseScore("| GR = 87.5%", &src) == 87);
    CHECK(src == "GameRankings");
    CHECK(GameInfoService::ParseScore("| MC = (3DS) 85/100") == 0); // unclear platform: no score
    CHECK(GameInfoService::ParseScore("no reviews here") == 0);
    CHECK(GameInfoService::ParseScore("| MC = 0/100") == 0);
    GameInfo i;
    CHECK(i.Stars() == 0.0);
    i.score = 85;
    CHECK(i.Stars() == 4.5);
    i.score = 94;
    CHECK(i.Stars() == 4.5);
    i.score = 96;
    CHECK(i.Stars() == 5.0);
    i.score = 60;
    CHECK(i.Stars() == 3.0);
    i.score = 42;
    CHECK(i.Stars() == 2.0);
}
