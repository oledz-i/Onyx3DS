// SPDX-License-Identifier: GPL-3.0-or-later
//
// XAudio2 output for the emulator (32728 Hz stereo s16 from the 3DS DSP) and
// for the home menu's sound effects.
#pragma once

namespace AudioCore {
class TimeStretcher;
}

namespace onyx::app {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Start(uint32_t sample_rate);
    void Stop();

    // Interleaved stereo frames from the emulation thread. Audio beyond the
    // latency cap is dropped (fast-forward, hitches) instead of piling up.
    void Push(const int16_t* data, size_t frames);
    // Frames queued and not yet played, for the frame pacer.
    size_t QueuedFrames() const;
    uint32_t SampleRate() const { return rate_; }

    void SetVolume(float volume); // 0..1
    void SetMuted(bool muted);
    // Small resampling nudge (±0.5%) that keeps the queue near its target
    // without audible pitch change; called once per frame by the pacer.
    void SetRateNudge(double ratio);
    // Called after every emulated frame. While emulation runs slower than real
    // time, the frame's audio is time-stretched (same pitch, longer) to fill the
    // real time that passed, instead of leaving gaps that crackle.
    void EndFrame();

private:
    struct Callback;
    winrt::com_ptr<IXAudio2> xaudio_;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    std::unique_ptr<Callback> callback_;
    uint32_t rate_ = 0;

    // Fixed pool of buffers so the hot path never allocates.
    static constexpr size_t kBuffers = 32;
    static constexpr size_t kFramesPerBuffer = 1024;
    std::array<std::vector<int16_t>, kBuffers> pool_;
    size_t next_ = 0;
    std::vector<int16_t> staging_;
    std::atomic<uint64_t> submitted_frames_{0};
    void Submit(const int16_t* data, size_t frames);
    std::vector<int16_t> frame_in_;  // this frame's samples (EndFrame consumes them)
    std::vector<int16_t> stretch_out_;
    std::unique_ptr<AudioCore::TimeStretcher> stretcher_;
    bool stretching_ = false;
    int fast_frames_ = 0;
    std::chrono::steady_clock::time_point last_frame_{};
    float volume_ = 1.0f;
    bool muted_ = false;
};

// Menu sound effects: tiny PCM WAV files kept in memory.
class SoundEffects {
public:
    SoundEffects();
    ~SoundEffects();
    bool Load(const std::string& id, const std::string& wav_path);
    void Play(const std::string& id);
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    void SetVolume(float v);

private:
    struct Sound {
        WAVEFORMATEX format{};
        std::vector<uint8_t> data;
    };
    winrt::com_ptr<IXAudio2> xaudio_;
    IXAudio2MasteringVoice* master_ = nullptr;
    std::mutex mutex_;
    std::vector<std::pair<std::string, Sound>> sounds_;
    std::vector<IXAudio2SourceVoice*> voices_;
    bool enabled_ = true;
    float volume_ = 0.8f;
};

} // namespace onyx::app
