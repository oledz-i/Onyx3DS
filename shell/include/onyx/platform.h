// SPDX-License-Identifier: GPL-3.0-or-later
//
// Platform services the shell needs. The Xbox app implements these with
// *FromApp Win32 calls and Windows.Web.Http; tests use the std:: versions in
// std_platform.h and a scripted fake HTTP client.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "onyx/common.h"

namespace onyx {

struct DirEntry {
    std::string name;
    bool is_dir = false;
    u64 size = 0;
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    virtual bool Exists(const std::string& path) = 0;
    virtual bool IsDirectory(const std::string& path) = 0;
    virtual std::vector<DirEntry> List(const std::string& dir) = 0;
    // Reads up to `length` bytes at `offset`. Returns fewer bytes at EOF.
    virtual Bytes ReadRange(const std::string& path, u64 offset, std::size_t length) = 0;
    virtual std::optional<u64> Size(const std::string& path) = 0;
    virtual bool WriteAll(const std::string& path, std::span<const u8> data) = 0;
    virtual bool CreateDirs(const std::string& path) = 0;
    virtual bool Remove(const std::string& path) = 0;
    virtual bool Copy(const std::string& from, const std::string& to) = 0;

    Bytes ReadAll(const std::string& path) {
        const auto sz = Size(path);
        return sz ? ReadRange(path, 0, static_cast<std::size_t>(*sz)) : Bytes{};
    }
    std::string ReadText(const std::string& path) {
        const Bytes b = ReadAll(path);
        return {b.begin(), b.end()};
    }
    bool WriteText(const std::string& path, std::string_view text) {
        return WriteAll(path, {reinterpret_cast<const u8*>(text.data()), text.size()});
    }
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string content_type; // for POST bodies
};

struct HttpResponse {
    int status = 0; // 0 = transport failure (offline, DNS, TLS...)
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

// Blocking HTTP. The shell only calls this from worker threads, never from
// the UI thread or the emulation thread.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse Send(const HttpRequest& request) = 0;

    HttpResponse Get(const std::string& url, std::map<std::string, std::string> headers = {}) {
        HttpRequest r;
        r.url = url;
        r.headers = std::move(headers);
        return Send(r);
    }
};

} // namespace onyx
