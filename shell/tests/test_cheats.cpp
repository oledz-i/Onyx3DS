#include <doctest/doctest.h>

#include "helpers.h"
#include "onyx/cheats.h"

using namespace onyx;
using namespace testutil;

static const char* kSample = R"(
[Money and infinite]
28FDC878 0000007F
08FDC878 77BFFFFF

[99999 workers]
*citra_enabled
*Max out the workforce
08FF2AC0 0000270F

[Heading only]

[Broken]
NOTHEX 1234
)";

TEST_CASE("Cheat files parse in Azahar's format") {
    const CheatFile f = CheatFile::Parse(kSample);
    REQUIRE(f.cheats.size() == 3); // "Heading only" has no code lines
    CHECK(f.cheats[0].name == "Money and infinite");
    CHECK(f.cheats[0].lines.size() == 2);
    CHECK_FALSE(f.cheats[0].enabled);
    CHECK(f.cheats[1].enabled);
    CHECK(f.cheats[1].comments.at(0) == "Max out the workforce");
    CHECK(f.cheats[0].IsValid());
    CHECK_FALSE(f.cheats[2].IsValid());
}

TEST_CASE("Serialise round trip keeps the enabled marker") {
    const CheatFile f = CheatFile::Parse(kSample);
    const std::string text = f.Serialize();
    CHECK(text.find("*citra_enabled") != std::string::npos);
    const CheatFile again = CheatFile::Parse(text);
    REQUIRE(again.cheats.size() == f.cheats.size());
    CHECK(again.cheats[1].enabled);
    CHECK(again.cheats[1].lines == f.cheats[1].lines);
}

TEST_CASE("Merging adds new cheats switched off and keeps local state") {
    CheatFile local = CheatFile::Parse("[A]\n*citra_enabled\n00000000 00000001\n");
    const CheatFile remote =
        CheatFile::Parse("[A]\n00000000 00000002\n[B]\n*citra_enabled\n00000000 00000003\n");
    CHECK(local.MergeFrom(remote) == 1);
    REQUIRE(local.cheats.size() == 2);
    CHECK(local.cheats[0].lines[0] == "00000000 00000001"); // local version kept
    CHECK(local.cheats[0].enabled);
    CHECK_FALSE(local.cheats[1].enabled);                    // downloaded ones start off
}

TEST_CASE("Cheat database: index from the GitHub tree, raw download, merge to disk") {
    TempDir cache, cheats;
    StdFileSystem fs;
    FakeHttp http;
    const std::string tree = R"({"sha":"x","tree":[
        {"path":"Cheats","type":"tree"},
        {"path":"Cheats/Bravely Second - End Layer (USA)/000400000017BA00.txt","type":"blob"},
        {"path":"Cheats/Bad/NOTATITLE.txt","type":"blob"},
        {"path":"README.md","type":"blob"}],"truncated":false})";
    http.routes["https://api.github.com/repos/iSharingan/CTRPF-AR-CHEAT-CODES/git/trees/master?recursive=1"] = {200, tree, ""};
    http.routes["https://raw.githubusercontent.com/iSharingan/CTRPF-AR-CHEAT-CODES/master/"
                "Cheats/Bravely%20Second%20-%20End%20Layer%20%28USA%29/000400000017BA00.txt"] = {200, kSample, ""};

    CheatDatabase db(http, fs, cache.Str());
    REQUIRE(db.RefreshIndex(1000));
    CHECK(db.IndexSize() == 1);
    CHECK(db.PathFor(0x000400000017BA00ull).has_value());
    CHECK(http.log.back().headers.at("User-Agent") == "ONYX3DS");

    CHECK(db.DownloadInto(0x000400000017BA00ull, cheats.Str()) == 2); // broken one dropped
    const auto text = fs.ReadText(cheats.Str() + "/000400000017BA00.txt");
    CHECK(text.find("[99999 workers]") != std::string::npos);
    CHECK(text.find("*citra_enabled") == std::string::npos); // nothing auto-enabled
    CHECK(db.DownloadInto(0x000400000017BA00ull, cheats.Str()) == 0); // idempotent
    CHECK(db.DownloadInto(0x0004000000000001ull, cheats.Str()) == -1); // unknown title

    // Second database object reuses the cached index without a request.
    const auto requests = http.log.size();
    CheatDatabase cached(http, fs, cache.Str());
    CHECK(cached.RefreshIndex(2000));
    CHECK(http.log.size() == requests);
    // ... until it is a week old.
    CheatDatabase stale(http, fs, cache.Str());
    CHECK(stale.RefreshIndex(1000 + 8 * 24 * 3600));
    CHECK(http.log.size() == requests + 1);
}

TEST_CASE("Offline index refresh falls back to the stale cache") {
    TempDir cache;
    StdFileSystem fs;
    FakeHttp http;
    http.routes["https://api.github.com/repos/iSharingan/CTRPF-AR-CHEAT-CODES/git/trees/master?recursive=1"] =
        {200, R"({"tree":[{"path":"Cheats/G/0004000000030800.txt","type":"blob"}]})", ""};
    CheatDatabase db(http, fs, cache.Str());
    REQUIRE(db.RefreshIndex(0));
    http.routes.clear(); // console goes offline
    CheatDatabase offline(http, fs, cache.Str());
    CHECK(offline.RefreshIndex(30ull * 24 * 3600));
    CHECK(offline.IndexSize() == 1);
}
