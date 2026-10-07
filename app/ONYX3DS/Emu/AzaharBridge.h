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

// core/frontend/applets/swkbd.h, patch 0050. Plain types only; the struct must match the patched
// header member for member. Strings are valid only while the hook runs.
namespace Frontend {
struct KeyboardRequest {
    int button_config; // 0 = Ok, 1 = Cancel | Ok, 2 = Cancel | I forgot | Ok, 3 = none
    int accept_mode;   // 0 anything, 1 not empty, 2 not empty and not blank, 3 not blank, 4 fixed length
    int type;          // 0 normal, 1 QWERTY, 2 number pad, 3 western
    bool multiline;
    bool password;
    bool prevent_digit;
    bool prevent_at;
    bool prevent_percent;
    bool prevent_backslash;
    bool prevent_profanity;
    bool callback;
    int max_text_length;
    int max_digits;
    const char* hint_text;
    const char* button_text[3];
    const char* error_text;
};
using KeyboardRequestHook = void (*)(const KeyboardRequest* request);
void SetKeyboardRequestHook(KeyboardRequestHook hook);
bool KeyboardRequestPending();
int SubmitKeyboardText(const char* utf8, int button);
bool AbortKeyboardRequest(const char* utf8_fallback);
void DropKeyboardRequest();
const char* KeyboardErrorText(int validation_error);
} // namespace Frontend
