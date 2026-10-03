// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cheat files in the format Azahar reads from <cheats>/<TITLEID>.txt:
//
//   [Infinite Health]
//   *citra_enabled          <- present when the cheat is switched on
//   *a comment line
//   D3000000 00000000
//   ...
//
// plus a client for the community Action Replay database on GitHub
// (iSharingan/CTRPF-AR-CHEAT-CODES), which stores the same format as
// Cheats/<Game Name (Region)>/<TITLEID>.txt.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "onyx/platform.h"

namespace onyx {

struct Cheat {
    std::string name;
    bool enabled = false;
    std::vector<std::string> comments;
    std::vector<std::string> lines; // "XXXXXXXX YYYYYYYY"
    bool IsValid() const;           // every code line is two 8-digit hex words
};

struct CheatFile {
    std::vector<Cheat> cheats;

    static CheatFile Parse(std::string_view text);
    std::string Serialize() const;

    // Adds cheats from `incoming` whose names are not already present. Existing
    // cheats (and whether they are switched on) are left alone. Returns how
    // many were added.
    int MergeFrom(const CheatFile& incoming);
    Cheat* Find(std::string_view name);
};

struct CheatSource {
    std::string label = "CTRPF Action Replay database";
    std::string owner = "iSharingan";
    std::string repo = "CTRPF-AR-CHEAT-CODES";
    std::string branch = "master";
    std::string prefix = "Cheats/";
};

class CheatDatabase {
public:
    CheatDatabase(IHttpClient& http, IFileSystem& fs, std::string cache_dir,
                  CheatSource source = {});

    // Downloads the repository file list (one request) and caches it. The
    // cache is reused for 7 days unless `force` is set.
    bool RefreshIndex(u64 now_unix, bool force = false);
    bool HasIndex() const { return !index_.empty(); }
    std::size_t IndexSize() const { return index_.size(); }

    // Path inside the repo for a title, e.g. "Cheats/Foo (USA)/0004000000123400.txt".
    std::optional<std::string> PathFor(u64 title_id) const;

    // Downloads the cheats for a title. std::nullopt when the database has none
    // or the console is offline.
    std::optional<CheatFile> Fetch(u64 title_id);

    // Fetch + merge into <cheats_dir>/<TITLEID>.txt. Returns cheats added, or -1.
    int DownloadInto(u64 title_id, const std::string& cheats_dir);

    // Exposed for tests: parses a GitHub git/trees response.
    static std::map<u64, std::string> ParseTree(std::string_view json, const std::string& prefix);
    std::string RawUrl(const std::string& repo_path) const;

private:
    bool LoadCachedIndex(u64 now_unix);

    IHttpClient& http_;
    IFileSystem& fs_;
    std::string cache_dir_;
    CheatSource source_;
    std::map<u64, std::string> index_;
};

} // namespace onyx
