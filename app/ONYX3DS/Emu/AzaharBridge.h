// SPDX-License-Identifier: GPL-3.0-or-later
//
// The few Azahar internals the app calls directly, declared by hand so the
// frontend does not need Azahar's (very large) include tree. Signatures must
// match the patched Azahar sources in patches/azahar exactly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace FileUtil {
using PathTranslator = std::string (*)(const std::string& path);
void SetPathTranslator(PathTranslator translator); // added by patch 0001
void SetUserPath(const std::string& path);
} // namespace FileUtil

namespace Service::AM {
enum class InstallStatus : std::uint32_t {
    Success,
    ErrorFailedToOpenFile,
    ErrorFileNotFound,
    ErrorAborted,
    ErrorInvalid,
    ErrorEncrypted,
};
using ProgressCallback = void(std::size_t, std::size_t);
InstallStatus InstallCIA(const std::string& path,
                         std::function<ProgressCallback>&& update_callback);
} // namespace Service::AM

namespace soundtouch {
class SoundTouch;
}
// audio_core/time_stretch.h (unpatched). Same members, so the size matches.
namespace AudioCore {
class TimeStretcher {
public:
    TimeStretcher();
    ~TimeStretcher();
    void SetOutputSampleRate(unsigned int sample_rate);
    std::size_t Process(const std::int16_t* in, std::size_t num_in, std::int16_t* out,
                        std::size_t num_out);
    void Clear();
    void Flush();

private:
    std::unique_ptr<soundtouch::SoundTouch> sound_touch;
    double stretch_ratio = 1.0;
};
} // namespace AudioCore
