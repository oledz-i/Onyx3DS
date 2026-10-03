// SPDX-License-Identifier: GPL-3.0-or-later
#include "onyx/common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace onyx {

std::string TitleIdToHex(u64 title_id) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(title_id));
    return buf;
}

std::optional<u64> HexToTitleId(std::string_view hex) {
    hex = std::string_view(hex.data(), hex.size());
    if (hex.empty() || hex.size() > 16) return std::nullopt;
    u64 v = 0;
    for (char c : hex) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<u64>(c - 'A' + 10);
        else return std::nullopt;
    }
    return v;
}

std::string Utf16LeToUtf8(std::span<const u8> b) {
    std::string out;
    out.reserve(b.size() / 2);
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        u32 cp = b[i] | (b[i + 1] << 8);
        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < b.size()) {
            const u32 lo = b[i + 2] | (b[i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string ToLower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::string Trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    return ToLower(s.substr(s.size() - suffix.size())) == ToLower(suffix);
}

std::vector<std::string> Split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto pos = s.find(sep, start);
        out.emplace_back(s.substr(start, pos == std::string_view::npos ? s.npos : pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string NormalizeSlashes(std::string_view p) {
    std::string r(p);
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

std::string JoinPath(std::string_view a, std::string_view b) {
    if (a.empty()) return NormalizeSlashes(b);
    if (b.empty()) return NormalizeSlashes(a);
    std::string r = NormalizeSlashes(a);
    while (!r.empty() && r.back() == '/') r.pop_back();
    std::string rb = NormalizeSlashes(b);
    std::size_t i = 0;
    while (i < rb.size() && rb[i] == '/') ++i;
    return r + "/" + rb.substr(i);
}

std::string FileName(std::string_view p) {
    const std::string n = NormalizeSlashes(p);
    const auto pos = n.find_last_of('/');
    return pos == std::string::npos ? n : n.substr(pos + 1);
}

std::string Stem(std::string_view p) {
    const std::string f = FileName(p);
    const auto pos = f.find_last_of('.');
    return (pos == std::string::npos || pos == 0) ? f : f.substr(0, pos);
}

std::string Extension(std::string_view p) {
    const std::string f = FileName(p);
    const auto pos = f.find_last_of('.');
    return (pos == std::string::npos || pos == 0) ? std::string{} : ToLower(f.substr(pos));
}

std::string UrlEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace onyx
