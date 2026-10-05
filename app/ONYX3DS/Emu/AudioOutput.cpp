// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Emu/AudioOutput.h"

#include "Emu/AzaharBridge.h"

#include "Platform/Log.h"
#include "Platform/UwpPlatform.h"

namespace onyx::app {

struct AudioOutput::Callback final : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
        ONYX_ERROR("XAudio2 voice error 0x%08X", static_cast<unsigned>(error));
    }
};

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    Stop();
}

bool AudioOutput::Start(uint32_t sample_rate) {
    Stop();
    if (FAILED(XAudio2Create(xaudio_.put(), 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        ONYX_ERROR("XAudio2Create failed");
        return false;
    }
    if (FAILED(xaudio_->CreateMasteringVoice(&master_))) {
        ONYX_ERROR("No audio output device");
        xaudio_ = nullptr;
        return false;
    }
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = sample_rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = sample_rate * 4;
    callback_ = std::make_unique<Callback>();
    if (FAILED(xaudio_->CreateSourceVoice(&voice_, &fmt, 0, 1.02f, callback_.get()))) {
        ONYX_ERROR("CreateSourceVoice failed");
        Stop();
        return false;
    }
    for (auto& b : pool_) b.assign(kFramesPerBuffer * 2, 0);
    staging_.clear();
    staging_.reserve(kFramesPerBuffer * 2);
    frame_in_.clear();
    stretching_ = false;
    last_frame_ = {};
    if (stretcher_) stretcher_->Clear();
    submitted_frames_ = 0;
    rate_ = sample_rate;
    voice_->SetVolume(muted_ ? 0.0f : volume_);
    voice_->Start();
    ONYX_INFO("Audio: %u Hz stereo", sample_rate);
    return true;
}

void AudioOutput::Stop() {
    if (voice_) {
        voice_->Stop();
        voice_->FlushSourceBuffers();
        voice_->DestroyVoice();
        voice_ = nullptr;
    }
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    xaudio_ = nullptr;
    callback_.reset();
}

size_t AudioOutput::QueuedFrames() const {
    if (!voice_) return 0;
    XAUDIO2_VOICE_STATE st{};
    voice_->GetState(&st, 0);
    const uint64_t played = st.SamplesPlayed;
    const uint64_t submitted = submitted_frames_.load();
    return submitted > played ? static_cast<size_t>(submitted - played) : 0;
}

void AudioOutput::Push(const int16_t* data, size_t frames) {
    if (!voice_ || frames == 0) return;
    // Collected per emulated frame; EndFrame decides whether to stretch it.
    frame_in_.insert(frame_in_.end(), data, data + frames * 2);
}

void AudioOutput::EndFrame() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = last_frame_ == std::chrono::steady_clock::time_point{}
                          ? 0.0
                          : std::chrono::duration<double>(now - last_frame_).count();
    last_frame_ = now;
    if (!voice_ || frame_in_.empty()) {
        frame_in_.clear();
        return;
    }
    const size_t in = frame_in_.size() / 2;
    const double want = std::clamp(dt, 0.0, 0.25) * rate_; // frames real time consumed
    const size_t queued = QueuedFrames();

    if (!stretching_) {
        // Behind real time and the queue has run dry: start stretching.
        // Even a few percent slow (e.g. 57 fps) drains the queue and crackles.
        if (want > in * 1.02 && queued < rate_ * 3 / 100) {
            stretching_ = true;
            fast_frames_ = 0;
            if (!stretcher_) stretcher_ = std::make_unique<AudioCore::TimeStretcher>();
            stretcher_->SetOutputSampleRate(rate_);
            ONYX_INFO("Audio: emulation is behind real time, stretching audio");
        }
    }
    if (!stretching_) {
        // ~150 ms cap: anything more is latency nobody wants.
        if (queued <= rate_ * 15 / 100) Submit(frame_in_.data(), in);
        frame_in_.clear();
        return;
    }

    // Ask the stretcher for the real time that passed, steered toward ~60 ms queued.
    const double target = rate_ * 0.06;
    double out = want + (target - static_cast<double>(queued)) * 0.25;
    out = std::clamp(out, in * 0.9, in * 4.0);
    const size_t num_out = static_cast<size_t>(out);
    stretch_out_.resize(num_out * 2);
    const size_t written = stretcher_->Process(frame_in_.data(), in, stretch_out_.data(), num_out);
    Submit(stretch_out_.data(), written);
    frame_in_.clear();

    // Back at full speed for ~2 s: stop stretching (flush what the stretcher holds).
    fast_frames_ = want <= in * 1.01 ? fast_frames_ + 1 : 0;
    if (fast_frames_ >= 120) {
        stretching_ = false;
        stretcher_->Flush();
        stretch_out_.resize(rate_ / 10 * 2);
        const size_t rest = stretcher_->Process(nullptr, 0, stretch_out_.data(), rate_ / 10);
        Submit(stretch_out_.data(), rest);
        stretcher_->Clear();
        ONYX_INFO("Audio: emulation caught up, audio stretching off");
    }
}

void AudioOutput::Submit(const int16_t* data, size_t frames) {
    size_t i = 0;
    while (i < frames) {
        const size_t room = kFramesPerBuffer - staging_.size() / 2;
        const size_t take = std::min(room, frames - i);
        staging_.insert(staging_.end(), data + i * 2, data + (i + take) * 2);
        i += take;
        if (staging_.size() / 2 == kFramesPerBuffer) {
            XAUDIO2_VOICE_STATE st{};
            voice_->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= kBuffers - 1) {
                staging_.clear(); // every pool buffer is still queued: drop
                continue;
            }
            auto& buf = pool_[next_];
            next_ = (next_ + 1) % kBuffers;
            buf.swap(staging_);
            staging_.clear();
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = static_cast<UINT32>(kFramesPerBuffer * 4);
            xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
            if (SUCCEEDED(voice_->SubmitSourceBuffer(&xb))) submitted_frames_ += kFramesPerBuffer;
            if (staging_.capacity() < kFramesPerBuffer * 2) staging_.reserve(kFramesPerBuffer * 2);
        }
    }
}

void AudioOutput::SetVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (voice_) voice_->SetVolume(muted_ ? 0.0f : volume_);
}

void AudioOutput::SetMuted(bool muted) {
    muted_ = muted;
    if (voice_) voice_->SetVolume(muted_ ? 0.0f : volume_);
}

void AudioOutput::SetRateNudge(double ratio) {
    if (voice_) voice_->SetFrequencyRatio(static_cast<float>(std::clamp(ratio, 0.995, 1.005)));
}

// ---------------------------------------------------------------------------

namespace {
// Minimal RIFF/WAVE reader for PCM files.
bool ParseWav(const Bytes& file, WAVEFORMATEX& fmt, std::vector<uint8_t>& pcm) {
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4))
        return false;
    size_t pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= file.size()) {
        const uint32_t size = file[pos + 4] | (file[pos + 5] << 8) | (file[pos + 6] << 16) |
                              (static_cast<uint32_t>(file[pos + 7]) << 24);
        const uint8_t* body = file.data() + pos + 8;
        if (pos + 8 + size > file.size()) break;
        if (!std::memcmp(file.data() + pos, "fmt ", 4) && size >= 16) {
            std::memcpy(&fmt, body, 16);
            fmt.cbSize = 0;
            have_fmt = fmt.wFormatTag == WAVE_FORMAT_PCM;
        } else if (!std::memcmp(file.data() + pos, "data", 4)) {
            pcm.assign(body, body + size);
        }
        pos += 8 + size + (size & 1);
    }
    return have_fmt && !pcm.empty();
}
} // namespace

SoundEffects::SoundEffects() {
    if (SUCCEEDED(XAudio2Create(xaudio_.put(), 0, XAUDIO2_DEFAULT_PROCESSOR)))
        xaudio_->CreateMasteringVoice(&master_);
}

SoundEffects::~SoundEffects() {
    for (auto* v : voices_) v->DestroyVoice();
    if (master_) master_->DestroyVoice();
}

bool SoundEffects::Load(const std::string& id, const std::string& wav_path) {
    UwpFileSystem fs;
    Sound s;
    if (!ParseWav(fs.ReadAll(wav_path), s.format, s.data)) return false;
    std::lock_guard lock(mutex_);
    std::erase_if(sounds_, [&](const auto& p) { return p.first == id; });
    sounds_.emplace_back(id, std::move(s));
    return true;
}

void SoundEffects::SetVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
}

void SoundEffects::Play(const std::string& id) {
    if (!enabled_ || !xaudio_ || !master_) return;
    std::lock_guard lock(mutex_);
    for (auto& [name, sound] : sounds_) {
        if (name != id) continue;
        // Reuse an idle voice with the same format, or make one (they are cheap).
        IXAudio2SourceVoice* voice = nullptr;
        for (auto* v : voices_) {
            XAUDIO2_VOICE_STATE st{};
            v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            XAUDIO2_VOICE_DETAILS d{};
            v->GetVoiceDetails(&d);
            if (st.BuffersQueued == 0 && d.InputSampleRate == sound.format.nSamplesPerSec &&
                d.InputChannels == sound.format.nChannels) {
                voice = v;
                break;
            }
        }
        if (!voice) {
            if (voices_.size() >= 8 || FAILED(xaudio_->CreateSourceVoice(&voice, &sound.format)))
                return;
            voices_.push_back(voice);
        }
        XAUDIO2_BUFFER b{};
        b.AudioBytes = static_cast<UINT32>(sound.data.size());
        b.pAudioData = sound.data.data();
        b.Flags = XAUDIO2_END_OF_STREAM;
        voice->SetVolume(volume_);
        voice->SubmitSourceBuffer(&b);
        voice->Start();
        return;
    }
}

} // namespace onyx::app
