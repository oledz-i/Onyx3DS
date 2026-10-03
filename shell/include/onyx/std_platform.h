// SPDX-License-Identifier: GPL-3.0-or-later
//
// std::filesystem implementation of IFileSystem. Used by the unit tests and by
// the desktop debug build; the Xbox app uses UwpFileSystem instead because
// paths on a USB drive need the *FromApp APIs.
#pragma once

#include <filesystem>
#include <fstream>

#include "onyx/platform.h"

namespace onyx {

class StdFileSystem final : public IFileSystem {
public:
    bool Exists(const std::string& p) override {
        std::error_code ec;
        return std::filesystem::exists(Path(p), ec);
    }
    bool IsDirectory(const std::string& p) override {
        std::error_code ec;
        return std::filesystem::is_directory(Path(p), ec);
    }
    std::vector<DirEntry> List(const std::string& dir) override {
        std::vector<DirEntry> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(Path(dir), ec)) {
            DirEntry d;
            d.name = e.path().filename().string();
            d.is_dir = e.is_directory(ec);
            d.size = d.is_dir ? 0 : static_cast<u64>(e.file_size(ec));
            out.push_back(std::move(d));
        }
        return out;
    }
    Bytes ReadRange(const std::string& p, u64 offset, std::size_t length) override {
        std::ifstream f(Path(p), std::ios::binary);
        if (!f) return {};
        f.seekg(static_cast<std::streamoff>(offset));
        Bytes b(length);
        f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(length));
        b.resize(static_cast<std::size_t>(f.gcount()));
        return b;
    }
    std::optional<u64> Size(const std::string& p) override {
        std::error_code ec;
        const auto s = std::filesystem::file_size(Path(p), ec);
        if (ec) return std::nullopt;
        return static_cast<u64>(s);
    }
    bool WriteAll(const std::string& p, std::span<const u8> data) override {
        std::ofstream f(Path(p), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(f);
    }
    bool CreateDirs(const std::string& p) override {
        std::error_code ec;
        std::filesystem::create_directories(Path(p), ec);
        return IsDirectory(p);
    }
    bool Remove(const std::string& p) override {
        std::error_code ec;
        return std::filesystem::remove(Path(p), ec);
    }
    bool Copy(const std::string& from, const std::string& to) override {
        std::error_code ec;
        std::filesystem::copy_file(Path(from), Path(to),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }

private:
    static std::filesystem::path Path(const std::string& p) {
        return std::filesystem::path(std::u8string(p.begin(), p.end()));
    }
};

} // namespace onyx
