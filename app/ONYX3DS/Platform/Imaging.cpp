// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Platform/Imaging.h"

#include <unordered_map>

#include "Platform/Log.h"
#include "Platform/UwpPlatform.h"

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;

namespace onyx::app {

bool EncodePng(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, Bytes& png) {
    try {
        InMemoryRandomAccessStream stream;
        BitmapEncoder encoder = BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream).get();
        encoder.SetPixelData(BitmapPixelFormat::Rgba8, BitmapAlphaMode::Ignore, width, height, 96, 96,
                             array_view<const uint8_t>(rgba.data(), rgba.data() + rgba.size()));
        encoder.FlushAsync().get();
        const auto size = static_cast<uint32_t>(stream.Size());
        stream.Seek(0);
        DataReader reader(stream);
        reader.LoadAsync(size).get();
        png.resize(size);
        reader.ReadBytes(array_view<uint8_t>(png.data(), png.data() + png.size()));
        return true;
    } catch (hresult_error const& e) {
        ONYX_ERROR("PNG encode failed: %s", Utf8(e.message()).c_str());
        return false;
    }
}

ImageSource IconFromRgba(const Bytes& rgba, int width, int height) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4) return nullptr;
    WriteableBitmap bmp(width, height);
    // WriteableBitmap wants premultiplied BGRA; icons are opaque so just swizzle.
    uint8_t* dst = bmp.PixelBuffer().data();
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        dst[i * 4 + 0] = rgba[i * 4 + 2];
        dst[i * 4 + 1] = rgba[i * 4 + 1];
        dst[i * 4 + 2] = rgba[i * 4 + 0];
        dst[i * 4 + 3] = rgba[i * 4 + 3];
    }
    bmp.Invalidate();
    return bmp;
}

namespace {
// UI-thread caches. Bounded so a huge library cannot hold every image forever.
std::unordered_map<std::string, ImageSource> g_images;
std::unordered_map<std::string, ImageSource> g_icons;
std::unordered_map<std::string, bool> g_exists;
constexpr size_t kMaxCached = 400;

template <typename Map> void Bound(Map& m) {
    if (m.size() > kMaxCached) m.clear();
}
} // namespace

bool CachedExists(const std::string& path) {
    if (path.empty()) return false;
    if (auto it = g_exists.find(path); it != g_exists.end()) return it->second;
    Bound(g_exists);
    const bool e = UwpFileSystem().Exists(path);
    g_exists.emplace(path, e);
    return e;
}

void LoadIconInto(winrt::Windows::UI::Xaml::Controls::Image const& target, const std::string& path,
                  int width, int height) {
    if (path.empty()) return;
    if (auto it = g_icons.find(path); it != g_icons.end()) {
        target.Source(it->second);
        return;
    }
    auto dispatcher = target.Dispatcher();
    winrt::agile_ref<winrt::Windows::UI::Xaml::Controls::Image> weak_target{target};
    RunAsync([dispatcher, weak_target, path, width, height] {
        auto data = std::make_shared<Bytes>(UwpFileSystem().ReadAll(path));
        if (data->empty()) return;
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                            [weak_target, path, data, width, height] {
                                ImageSource src = nullptr;
                                if (auto it = g_icons.find(path); it != g_icons.end()) {
                                    src = it->second;
                                } else {
                                    src = IconFromRgba(*data, width, height);
                                    if (!src) return;
                                    Bound(g_icons);
                                    g_icons.emplace(path, src);
                                }
                                if (auto img = weak_target.get()) img.Source(src);
                            });
    });
}

ImageSource ImageFromFile(const std::string& path, int decode_width) {
    if (path.empty()) return nullptr;
    const std::string key = path + "|" + std::to_string(decode_width);
    if (auto it = g_images.find(key); it != g_images.end()) return it->second;
    Bound(g_images);
    BitmapImage img;
    g_images.emplace(key, img);
    if (decode_width > 0) img.DecodePixelWidth(decode_width);
    if (path.rfind("ms-appx:", 0) == 0) {
        img.UriSource(winrt::Windows::Foundation::Uri(Wide(path)));
        return img;
    }
    // Files outside the package (LocalState, USB) are read off the UI thread
    // with the *FromApp API and handed back as a stream.
    auto dispatcher = img.Dispatcher();
    winrt::agile_ref<BitmapImage> target{img};
    RunAsync([dispatcher, target, path] {
        UwpFileSystem fs;
        const Bytes data = fs.ReadAll(path);
        if (data.empty()) return;
        InMemoryRandomAccessStream stream;
        DataWriter writer(stream);
        writer.WriteBytes(array_view<const uint8_t>(data.data(), data.data() + data.size()));
        writer.StoreAsync().get();
        writer.DetachStream();
        stream.Seek(0);
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                            [target, stream] {
                                if (auto bmp = target.get()) bmp.SetSourceAsync(stream);
                            });
    });
    return img;
}

} // namespace onyx::app
