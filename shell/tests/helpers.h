// SPDX-License-Identifier: GPL-3.0-or-later
// Builders for synthetic 3DS files and a scripted HTTP client.
#pragma once

#include <cstring>
#include <filesystem>
#include <map>

#include "onyx/platform.h"
#include "onyx/std_platform.h"

namespace testutil {

using namespace onyx;

inline void PutU16(Bytes& b, std::size_t off, u16 v) {
    if (b.size() < off + 2) b.resize(off + 2);
    b[off] = v & 0xFF;
    b[off + 1] = v >> 8;
}
inline void PutU32(Bytes& b, std::size_t off, u32 v) {
    if (b.size() < off + 4) b.resize(off + 4);
    for (int i = 0; i < 4; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
}
inline void PutU64(Bytes& b, std::size_t off, u64 v) {
    PutU32(b, off, static_cast<u32>(v));
    PutU32(b, off + 4, static_cast<u32>(v >> 32));
}
inline void PutBe32(Bytes& b, std::size_t off, u32 v) {
    if (b.size() < off + 4) b.resize(off + 4);
    for (int i = 0; i < 4; ++i) b[off + i] = (v >> (24 - 8 * i)) & 0xFF;
}
inline void PutBe64(Bytes& b, std::size_t off, u64 v) {
    PutBe32(b, off, static_cast<u32>(v >> 32));
    PutBe32(b, off + 4, static_cast<u32>(v));
}
inline void PutStr(Bytes& b, std::size_t off, std::string_view s) {
    if (b.size() < off + s.size()) b.resize(off + s.size());
    std::memcpy(b.data() + off, s.data(), s.size());
}
inline void PutUtf16(Bytes& b, std::size_t off, std::string_view ascii) {
    for (std::size_t i = 0; i < ascii.size(); ++i) PutU16(b, off + i * 2, static_cast<u8>(ascii[i]));
}

// SMDH with the same English name in every language slot and a solid icon.
inline Bytes BuildSmdh(std::string_view short_name, std::string_view long_name,
                       std::string_view publisher, u32 region, u16 rgb565) {
    Bytes s(0x36C0, 0);
    PutStr(s, 0, "SMDH");
    for (int lang = 0; lang < 16; ++lang) {
        if (lang >= 12) break;
        const std::size_t base = 0x8 + static_cast<std::size_t>(lang) * 0x200;
        if (lang == 1) { // English only, so Best() must pick slot 1
            PutUtf16(s, base, short_name);
            PutUtf16(s, base + 0x80, long_name);
            PutUtf16(s, base + 0x180, publisher);
        }
    }
    PutU32(s, 0x2018, region);
    for (std::size_t i = 0; i < 48 * 48; ++i) PutU16(s, 0x24C0 + i * 2, rgb565);
    return s;
}

// A decrypted NCCH with an ExeFS whose only file is "icon".
inline Bytes BuildNcch(u64 program_id, std::string_view product_code, const Bytes& smdh,
                       bool no_crypto = true) {
    const u32 mu = 0x200;
    Bytes n(0x200, 0);
    PutStr(n, 0x100, "NCCH");
    PutU64(n, 0x108, program_id);
    PutU64(n, 0x118, program_id);
    PutStr(n, 0x150, product_code);
    n[0x18D] = 0x3;              // executable + data
    n[0x18E] = 0;                // media unit 0x200
    n[0x18F] = no_crypto ? 0x4 : 0x0;
    const u32 exefs_off_mu = 1;  // right after the header
    PutU32(n, 0x1A0, exefs_off_mu);
    const u32 exefs_size = 0x200 + static_cast<u32>(smdh.size());
    PutU32(n, 0x1A4, (exefs_size + mu - 1) / mu);
    Bytes exefs(0x200, 0);
    PutStr(exefs, 0, "icon");
    PutU32(exefs, 8, 0);
    PutU32(exefs, 12, static_cast<u32>(smdh.size()));
    n.resize(exefs_off_mu * mu);
    n.insert(n.end(), exefs.begin(), exefs.end());
    n.insert(n.end(), smdh.begin(), smdh.end());
    return n;
}

inline Bytes BuildNcsd(u64 media_id, const Bytes& ncch) {
    Bytes c(0x4000, 0);
    PutStr(c, 0x100, "NCSD");
    PutU64(c, 0x108, media_id);
    PutU32(c, 0x120, 0x4000 / 0x200); // partition 0 offset in media units
    PutU32(c, 0x124, static_cast<u32>(ncch.size() / 0x200 + 1));
    c.insert(c.end(), ncch.begin(), ncch.end());
    return c;
}

// CIA whose content is title-key encrypted (unreadable) but whose meta block
// carries the SMDH, which is the common case for update and DLC packages.
inline Bytes BuildEncryptedCia(u64 title_id, const Bytes& smdh) {
    auto align = [](u64 v) { return (v + 63) & ~u64{63}; };
    const u32 header = 0x2020, cert = 0xA00, ticket = 0x350, tmd = 0x208;
    const u64 content = 0x400;
    const u32 meta = 0x400 + static_cast<u32>(smdh.size());
    Bytes c;
    PutU32(c, 0x0, header);
    PutU32(c, 0x8, cert);
    PutU32(c, 0xC, ticket);
    PutU32(c, 0x10, tmd);
    PutU32(c, 0x14, meta);
    PutU64(c, 0x18, content);
    const u64 cert_off = align(header), ticket_off = align(cert_off + cert);
    const u64 tmd_off = align(ticket_off + ticket), content_off = align(tmd_off + tmd);
    const u64 meta_off = align(content_off + content);
    c.resize(meta_off + meta, 0);
    PutBe32(c, tmd_off, 0x10004);                       // RSA-2048 SHA-256
    PutBe64(c, tmd_off + 4 + 0x100 + 0x3C + 0x4C, title_id);
    for (u64 i = 0; i < content; ++i) c[content_off + i] = static_cast<u8>(i * 7 + 3); // noise
    std::memcpy(c.data() + meta_off + 0x400, smdh.data(), smdh.size());
    return c;
}

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("onyx_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                 std::to_string(counter_++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string Str() const { return path_.generic_string(); }
    std::string Write(const std::string& rel, const Bytes& data) const {
        const auto full = path_ / rel;
        std::filesystem::create_directories(full.parent_path());
        StdFileSystem fs;
        fs.WriteAll(full.generic_string(), data);
        return full.generic_string();
    }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

class FakeHttp final : public IHttpClient {
public:
    std::map<std::string, HttpResponse> routes; // exact URL -> response
    std::vector<HttpRequest> log;
    HttpResponse Send(const HttpRequest& r) override {
        log.push_back(r);
        if (auto it = routes.find(r.url); it != routes.end()) return it->second;
        return {404, "", "not found"};
    }
};

} // namespace testutil
