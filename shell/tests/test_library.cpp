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

TEST_CASE("NES titles are cleaned from file names") {
    CHECK(CleanFileTitle("Super Mario Bros. (USA) (Rev 1).nes") == "Super Mario Bros.");
    CHECK(CleanFileTitle("Contra (U) [!].nes") == "Contra");
    CHECK(CleanFileTitle("Mega_Man_2 (Japan).unf") == "Mega Man 2");
    CHECK(IsNesRomExtension(".nes"));
    CHECK(IsNesRomExtension(".unf"));
    CHECK_FALSE(IsNesRomExtension(".fds"));
    CHECK_FALSE(IsNesRomExtension(".3ds"));
}

TEST_CASE("NES library scans only the NES folder and stays separate from 3DS games") {
    TempDir roms, cache3ds, cache_nes;
    StdFileSystem fs;
    const u64 a = 0x0004000000030800ull;
    // The NES folder sits inside the 3DS Roms folder, as on a set-up USB drive.
    roms.Write("Kart Racer (USA).3ds",
               BuildNcsd(a, BuildNcch(a, "CTR-P-AMKE", BuildSmdh("Kart Racer", "", "Example", 2, 0))));
    roms.Write("NES/Super Mario Bros. (USA).nes", Bytes{'N', 'E', 'S', 0x1A, 2, 1});
    roms.Write("NES/Hacks/Mega Man 2 (Japan).unf", Bytes{'U', 'N', 'I', 'F', 0, 0});
    roms.Write("NES/Broken.nes", Bytes{1, 2, 3, 4});
    roms.Write("NES/Disk Game.fds", Bytes{'F', 'D', 'S', 0x1A});
    roms.Write("NES/readme.txt", Bytes{'x'});

    FolderConfig folders;
    folders.roms = {roms.Str()};
    folders.nes_roms = roms.Str() + "/NES";

    GameLibrary nes(fs, cache_nes.Str(), GameSystem::Nes);
    int progress = 0;
    nes.Scan(folders, [&](const ScanProgress&) { ++progress; });
    CHECK(progress == 3);
    REQUIRE(nes.All().size() == 3);
    for (const auto& g : nes.All()) {
        CHECK(g.system == GameSystem::Nes);
        CHECK(g.IsLaunchable());
    }
    QolSettings qol;
    qol.sort = SortMode::Title;
    const auto grid = nes.GridView(qol);
    REQUIRE(grid.size() == 3);
    CHECK(grid[0].title == "Broken");
    CHECK_FALSE(grid[0].note.empty()); // no iNES header
    CHECK(grid[1].title == "Mega Man 2");
    CHECK(grid[1].region == "JPN");
    CHECK(grid[1].note.empty());
    CHECK(grid[2].title == "Super Mario Bros.");
    CHECK(grid[2].region == "USA");

    // The 3DS library ignores .nes files and never walks into the NES folder.
    GameLibrary n3ds(fs, cache3ds.Str());
    n3ds.Scan(folders);
    REQUIRE(n3ds.All().size() == 1);
    CHECK(n3ds.All()[0].title == "Kart Racer");
    CHECK(n3ds.All()[0].system == GameSystem::N3DS);

    // With no NES folder configured the NES library is empty.
    GameLibrary empty(fs, cache_nes.Str(), GameSystem::Nes);
    FolderConfig none;
    none.roms = {roms.Str()};
    empty.Scan(none);
    CHECK(empty.All().empty());

    // Favourites, play time and the system survive a reload from the cache.
    auto smb = grid[2];
    smb.favorite = true;
    nes.Update(smb);
    nes.RecordSession(smb.path, 120, 1'700'000'000);
    GameLibrary reloaded(fs, cache_nes.Str(), GameSystem::Nes);
    REQUIRE(reloaded.Load());
    const auto again = reloaded.Find(smb.path);
    REQUIRE(again);
    CHECK(again->system == GameSystem::Nes);
    CHECK(again->favorite);
    CHECK(again->play_seconds == 120);

    // A rescan with unchanged files re-reads nothing.
    ScanProgress last;
    reloaded.Scan(folders, [&](const ScanProgress& p) { last = p; });
    CHECK(last.files_parsed == 0);
    CHECK(reloaded.Find(smb.path)->favorite);
}

TEST_CASE("Save keys: 3DS keeps the title ID, NES games get a stable name-based key") {
    GameEntry t;
    t.title_id = 0x0004000000030800ull;
    CHECK(t.SaveKey() == t.TitleIdHex());

    GameEntry a;
    a.system = GameSystem::Nes;
    a.path = "E:/ONYX3DS/Roms/NES/Contra (USA).nes";
    GameEntry b = a;
    b.path = "F:/Games/Contra (USA).NES"; // other drive, other case: same save data
    GameEntry c = a;
    c.path = "E:/ONYX3DS/Roms/NES/Gradius (USA).nes";
    CHECK(a.SaveKey().rfind("nes-", 0) == 0);
    CHECK(a.SaveKey() == b.SaveKey());
    CHECK(a.SaveKey() != c.SaveKey());
    CHECK(a.SaveKey() != a.TitleIdHex()); // never collides with the 3DS "0000..." folder
}
