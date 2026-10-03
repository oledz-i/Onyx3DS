#include <doctest/doctest.h>

#include "helpers.h"
#include "onyx/library.h"
#include "onyx/theme.h"

using namespace onyx;
using namespace testutil;

TEST_CASE("File names are cleaned into titles") {
    CHECK(CleanFileTitle("Mario Kart 7 (USA) (En,Fr,Es) [!].3ds") == "Mario Kart 7");
    CHECK(CleanFileTitle("Super_Hero_Game (Europe).cia") == "Super Hero Game");
    CHECK(CleanFileTitle("Plain.cci") == "Plain");
    CHECK(CleanFileTitle("(Only Tags).3ds") == "(Only Tags)");
}

TEST_CASE("Library scan reads games, hides updates and survives a reload") {
    TempDir roms, cache;
    StdFileSystem fs;
    const u64 a = 0x0004000000030800ull, b = 0x0004000000040000ull;
    roms.Write("Racing/Kart Racer (USA).3ds",
               BuildNcsd(a, BuildNcch(a, "CTR-P-AMKE", BuildSmdh("Kart Racer", "", "Example", 2, 0))));
    roms.Write("Zelda-like (EUR).cci",
               BuildNcsd(b, BuildNcch(b, "CTR-P-ZZZP", BuildSmdh("Adventure", "", "Studio", 4, 0))));
    roms.Write("Kart Racer Update.cia",
               BuildEncryptedCia(0x0004000E00030800ull, BuildSmdh("Kart Racer", "", "Example", 2, 0)));
    roms.Write("notes.txt", Bytes{'h', 'i'});
    roms.Write("0004000000030800/tex.png", Bytes{1, 2, 3}); // stray texture pack folder

    GameLibrary lib(fs, cache.Str());
    FolderConfig folders;
    folders.roms = {roms.Str()};
    int progress_calls = 0;
    lib.Scan(folders, [&](const ScanProgress&) { ++progress_calls; });
    CHECK(progress_calls == 3);
    CHECK(lib.All().size() == 3);

    QolSettings qol;
    qol.sort = SortMode::Title;
    auto grid = lib.GridView(qol);
    REQUIRE(grid.size() == 2);
    CHECK(grid[0].title == "Adventure");
    CHECK(grid[1].title == "Kart Racer");
    CHECK(grid[1].region == "USA");
    CHECK(fs.Size(grid[1].icon_path).value_or(0) == 48 * 48 * 4);

    REQUIRE(lib.Installables().size() == 1);
    CHECK(lib.Installables()[0].kind == n3ds::TitleKind::Update);

    // Favourite + play time survive a reload from the JSON cache.
    auto kart = *lib.FindByTitleId(a);
    kart.favorite = true;
    lib.Update(kart);
    lib.RecordSession(kart.path, 600, 1'700'000'000);

    GameLibrary reloaded(fs, cache.Str());
    REQUIRE(reloaded.Load());
    auto again = *reloaded.FindByTitleId(a);
    CHECK(again.favorite);
    CHECK(again.play_seconds == 600);
    CHECK(again.launch_count == 1);
    grid = reloaded.GridView(qol);
    CHECK(grid[0].title == "Kart Racer"); // favourites first

    CHECK(reloaded.GridView(qol, "adven").size() == 1);
}

TEST_CASE("Rescan keeps user data and only re-reads changed files") {
    TempDir roms, cache;
    StdFileSystem fs;
    const u64 a = 0x0004000000030800ull;
    const auto path = roms.Write("k.3ds", BuildNcsd(a, BuildNcch(a, "A", BuildSmdh("Kart", "", "", 1, 0))));
    GameLibrary lib(fs, cache.Str());
    FolderConfig folders;
    folders.roms = {roms.Str()};
    lib.Scan(folders);
    auto g = *lib.Find(path);
    g.user_title = "My Kart";
    g.grid_art = "art.png";
    lib.Update(g);

    ScanProgress last;
    lib.Scan(folders, [&](const ScanProgress& p) { last = p; });
    CHECK(last.files_parsed == 0); // cache hit
    CHECK(lib.Find(path)->DisplayTitle() == "My Kart");

    // Replace the file (different size): re-parsed, user data carried over.
    roms.Write("k.3ds", BuildNcsd(a, BuildNcch(a, "A", BuildSmdh("Kart 2", "", "", 1, 0))) );
    Bytes bigger = fs.ReadAll(path);
    bigger.resize(bigger.size() + 0x200, 0);
    fs.WriteAll(path, bigger);
    lib.Scan(folders, [&](const ScanProgress& p) { last = p; });
    CHECK(last.files_parsed == 1);
    CHECK(lib.Find(path)->title == "Kart 2");
    CHECK(lib.Find(path)->DisplayTitle() == "My Kart");
    CHECK(lib.Find(path)->grid_art == "art.png");
}

TEST_CASE("Themes: built-ins load, user themes override by id, bad colours ignored") {
    TempDir builtin, user;
    StdFileSystem fs;
    const std::string a = R"j({"id":"aero-channel","name":"Aero Channel","colors":{"accent":"#33B5E5"},
        "textures":{"background":"bg.png"},"style":{"empty_slots":12}})j";
    const std::string b = R"j({"id":"midnight-onyx","name":"Midnight Onyx","colors":{"accent":"#9B7BFF"}})j";
    const std::string override_a = R"j({"id":"aero-channel","name":"Aero (mine)","colors":{"accent":"nope"}})j";
    builtin.Write("aero/theme.json", Bytes(a.begin(), a.end()));
    builtin.Write("onyx/theme.json", Bytes(b.begin(), b.end()));
    user.Write("my-aero/theme.json", Bytes(override_a.begin(), override_a.end()));

    const auto themes = LoadThemes(fs, builtin.Str(), user.Str());
    REQUIRE(themes.size() == 2);
    const Theme* aero = FindTheme(themes, "aero-channel");
    REQUIRE(aero);
    CHECK(aero->name == "Aero (mine)");
    CHECK_FALSE(aero->built_in);
    CHECK(aero->colors.accent == "#33B5E5"); // invalid colour kept the default
    const Theme* onyx_theme = FindTheme(themes, "midnight-onyx");
    REQUIRE(onyx_theme);
    CHECK(onyx_theme->colors.accent == "#9B7BFF");
    CHECK(FindTheme(themes, "missing") == &themes.front());

    const auto parsed = Theme::Parse(a, "X:/Themes/aero");
    REQUIRE(parsed);
    CHECK(parsed->Resolve(parsed->textures.background) == "X:/Themes/aero/bg.png");
    const auto round = Theme::Parse(parsed->ToJson(), "X:/Themes/aero");
    CHECK(round->textures.background == "bg.png");
    CHECK(round->style.empty_slots == 12);
}
