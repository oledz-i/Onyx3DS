// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/UwpPlatform.h"

#include "Platform/Log.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Web::Http;
namespace Streams = winrt::Windows::Storage::Streams;

namespace onyx::app {

namespace {

std::wstring ToWin(const std::string& path) {
    std::wstring w = Wide(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    // "E:" alone means the current directory on E:, which is never what we want.
    if (w.size() == 2 && w[1] == L':') w += L'\\';
    return w;
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};

HANDLE Open(const std::wstring& path, DWORD access, DWORD creation) {
    CREATEFILE2_EXTENDED_PARAMETERS p{};
    p.dwSize = sizeof(p);
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    p.dwFileFlags = (access & GENERIC_WRITE) ? 0 : FILE_FLAG_SEQUENTIAL_SCAN;
    return CreateFile2FromAppW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               creation, &p);
}

} // namespace

bool UwpFileSystem::Exists(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    return GetFileAttributesExFromAppW(ToWin(path).c_str(), GetFileExInfoStandard, &d) != 0;
}

bool UwpFileSystem::IsDirectory(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExFromAppW(ToWin(path).c_str(), GetFileExInfoStandard, &d)) return false;
    return (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::vector<DirEntry> UwpFileSystem::List(const std::string& dir) {
    std::vector<DirEntry> out;
    std::wstring pattern = ToWin(dir);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        DirEntry e;
        e.name = Utf8(name);
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (static_cast<u64>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

Bytes UwpFileSystem::ReadRange(const std::string& path, u64 offset, std::size_t length) {
    Handle f{Open(ToWin(path), GENERIC_READ, OPEN_EXISTING)};
    if (!f) return {};
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(f.h, pos, nullptr, FILE_BEGIN)) return {};
    Bytes out(length);
    std::size_t done = 0;
    while (done < length) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length - done, 64u << 20));
        DWORD got = 0;
        if (!ReadFile(f.h, out.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    out.resize(done);
    return out;
}

std::optional<u64> UwpFileSystem::Size(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExFromAppW(ToWin(path).c_str(), GetFileExInfoStandard, &d))
        return std::nullopt;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return std::nullopt;
    return (static_cast<u64>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool UwpFileSystem::WriteAll(const std::string& path, std::span<const u8> data) {
    // Write to a temp file and swap it in, so a crash never leaves half a
    // settings.json or library.json behind.
    const std::wstring target = ToWin(path);
    const std::wstring tmp = target + L".tmp";
    {
        Handle f{Open(tmp, GENERIC_WRITE, CREATE_ALWAYS)};
        if (!f) return false;
        std::size_t done = 0;
        while (done < data.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 64u << 20));
            DWORD wrote = 0;
            if (!WriteFile(f.h, data.data() + done, chunk, &wrote, nullptr)) return false;
            done += wrote;
        }
        FlushFileBuffers(f.h);
    }
    DeleteFileFromAppW(target.c_str());
    return MoveFileFromAppW(tmp.c_str(), target.c_str()) != 0;
}

bool UwpFileSystem::CreateDirs(const std::string& path) {
    std::wstring w = ToWin(path);
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    if (w.empty()) return false;
    if (IsDirectory(Utf8(w))) return true;
    const auto slash = w.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 2) CreateDirs(Utf8(w.substr(0, slash)));
    CreateDirectoryFromAppW(w.c_str(), nullptr);
    return IsDirectory(Utf8(w));
}

bool UwpFileSystem::Remove(const std::string& path) {
    const std::wstring w = ToWin(path);
    return IsDirectory(path) ? RemoveDirectoryFromAppW(w.c_str()) != 0
                             : DeleteFileFromAppW(w.c_str()) != 0;
}

bool UwpFileSystem::Copy(const std::string& from, const std::string& to) {
    return CopyFileFromAppW(ToWin(from).c_str(), ToWin(to).c_str(), FALSE) != 0;
}

// ---------------------------------------------------------------------------

UwpHttpClient::UwpHttpClient() {
    Filters::HttpBaseProtocolFilter filter;
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.AllowUI(false);
    client_ = HttpClient(filter);
}

HttpResponse UwpHttpClient::Send(const HttpRequest& request) {
    HttpResponse out;
    try {
        const Uri uri(Wide(request.url));
        HttpMethod method = request.method == "POST" ? HttpMethod::Post() : HttpMethod::Get();
        HttpRequestMessage msg(method, uri);
        for (const auto& [k, v] : request.headers) {
            // User-Agent must go through the typed header collection.
            if (_stricmp(k.c_str(), "User-Agent") == 0) {
                msg.Headers().UserAgent().TryParseAdd(Wide(v));
            } else {
                msg.Headers().TryAppendWithoutValidation(Wide(k), Wide(v));
            }
        }
        if (method == HttpMethod::Post()) {
            Streams::DataWriter writer;
            writer.WriteBytes(winrt::array_view<const uint8_t>(
                reinterpret_cast<const uint8_t*>(request.body.data()),
                reinterpret_cast<const uint8_t*>(request.body.data()) + request.body.size()));
            HttpBufferContent content(writer.DetachBuffer());
            content.Headers().ContentType(Headers::HttpMediaTypeHeaderValue::Parse(
                Wide(request.content_type.empty() ? "application/x-www-form-urlencoded"
                                                  : request.content_type)));
            msg.Content(content);
        }
        auto op = client_.SendRequestAsync(msg);
        // 20 s is plenty for small API calls; art downloads are a few hundred KB.
        if (op.wait_for(std::chrono::seconds(20)) != AsyncStatus::Completed) {
            op.Cancel();
            out.error = "timeout";
            return out;
        }
        HttpResponseMessage resp = op.GetResults();
        out.status = static_cast<int>(resp.StatusCode());
        const Streams::IBuffer buffer = resp.Content().ReadAsBufferAsync().get();
        out.body.assign(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
    } catch (hresult_error const& e) {
        out.status = 0;
        out.error = Utf8(e.message());
    }
    return out;
}

// ---------------------------------------------------------------------------

const AppPaths& Paths() {
    static const AppPaths paths = [] {
        AppPaths p;
        using namespace winrt::Windows::Storage;
        using namespace winrt::Windows::ApplicationModel;
        p.local_state = NormalizeSlashes(Utf8(ApplicationData::Current().LocalFolder().Path()));
        p.install = NormalizeSlashes(Utf8(Package::Current().InstalledLocation().Path()));
        p.azahar_root = JoinPath(p.local_state, "system");
        p.cache = JoinPath(p.local_state, "cache");
        p.art = JoinPath(p.cache, "art");
        p.builtin_themes = JoinPath(p.install, "Assets/Themes");
        p.settings_file = JoinPath(p.local_state, "settings.json");
        p.log_file = JoinPath(p.local_state, "onyx.log");
        return p;
    }();
    return paths;
}

ConsoleModel DetectConsole() {
    GAMING_DEVICE_MODEL_INFORMATION info{};
    if (SUCCEEDED(GetGamingDeviceModelInformation(&info)) &&
        info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
        switch (info.deviceId) {
        case GAMING_DEVICE_DEVICE_ID_XBOX_ONE: return ConsoleModel::XboxOne;
        case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S: return ConsoleModel::XboxOneS;
        case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
        case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT: return ConsoleModel::XboxOneX;
        case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S: return ConsoleModel::SeriesS;
        case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
        case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT: return ConsoleModel::SeriesX;
        default: break;
        }
    }
    const auto family =
        winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily();
    return family == L"Windows.Xbox" ? ConsoleModel::Unknown : ConsoleModel::Desktop;
}

std::string ConsoleDescription() {
    return ConsoleModelName(DetectConsole());
}

std::vector<std::string> RemovableDriveRoots() {
    std::vector<std::string> out;
    // On Xbox, external drives mount as D: and up (E: is the usual first USB
    // drive). Querying attributes is cheap and needs no extra permissions.
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
        const wchar_t root[] = {letter, L':', L'\\', 0};
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (GetFileAttributesExFromAppW(root, GetFileExInfoStandard, &d))
            out.push_back(std::string(1, static_cast<char>(letter)) + ":");
    }
    return out;
}

void RunAsync(std::function<void()> work) {
    winrt::Windows::System::Threading::ThreadPool::RunAsync(
        [work = std::move(work)](auto&&) {
            try {
                work();
            } catch (hresult_error const& e) {
                ONYX_ERROR("background task failed: %s", Utf8(e.message()).c_str());
            } catch (std::exception const& e) {
                ONYX_ERROR("background task failed: %s", e.what());
            }
        });
}

void RunOnUi(std::function<void()> work) {
    auto dispatcher =
        winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
    if (dispatcher.HasThreadAccess()) {
        work();
        return;
    }
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [work = std::move(work)] { work(); });
}

u64 NowUnix() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
}

} // namespace onyx::app
