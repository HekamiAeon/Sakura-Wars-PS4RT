// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Optional Sakura Wars menu WAV cues using Star Ocean's SDL3 audio-stream player.
// The original synthesized cues remain the fallback when WAVs are absent.
// Multiple SDL3 streams allow long ringing tails to overlap during fast scrolling.
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Ps4RtMenuSounds {

enum class Cue : unsigned { Navigate, Confirm, Cancel, Open, Close, Count };
constexpr int kSampleRate = 48000;

// Retained as a device-independent synthetic fallback (also used by the unit test).
inline std::vector<float> MakeCue(Cue cue) {
    const float duration = cue == Cue::Navigate ? 0.035f :
                           cue == Cue::Confirm ? 0.105f :
                           cue == Cue::Cancel || cue == Cue::Close ? 0.095f : 0.135f;
    const int count = int(float(kSampleRate) * duration);
    std::vector<float> samples(size_t(count), 0.0f);
    constexpr float kTau = 6.2831853071795864769f;
    float phase = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float progress = float(i) / float(count - 1);
        const float t = float(i) / float(kSampleRate);
        const float attack = std::min(t / 0.004f, 1.0f);
        const float release = (1.0f - progress) * (1.0f - progress);
        float frequency = 0.0f;
        float overtone = 0.0f;
        switch (cue) {
        case Cue::Navigate:
            frequency = 950.0f + 220.0f * progress;
            overtone = 0.20f;
            break;
        case Cue::Confirm:
            frequency = progress < 0.35f ? 685.0f : 920.0f;
            overtone = 0.32f;
            break;
        case Cue::Cancel:
        case Cue::Close:
            frequency = 580.0f - 230.0f * progress;
            overtone = 0.10f;
            break;
        case Cue::Open:
            frequency = 520.0f + 520.0f * progress;
            overtone = 0.30f;
            break;
        default:
            return {};
        }
        phase += kTau * frequency / float(kSampleRate);
        const float tone = std::sin(phase) + overtone * std::sin(2.0f * phase);
        samples[size_t(i)] = 0.32f * attack * release * tone;
    }
    return samples;
}

class Player {
    struct Sample {
        SDL_AudioSpec format{};
        std::vector<Uint8> pcm;
        bool recorded = false;
    };

    std::array<Sample, unsigned(Cue::Count)> samples_{};
    std::array<std::vector<SDL_AudioStream*>, unsigned(Cue::Count)> voices_{};
    std::array<size_t, unsigned(Cue::Count)> next_voice_{};
    SDL_AudioDeviceID device_ = 0;
    Uint64 last_navigation_ms_ = 0;
    float gain_ = 0.0f;
    bool initialized_ = false;

    static constexpr const char* kFilenames[] = {
        "scroll.wav", "open.wav", "close.wav", "open.wav", "close.wav"
    };
    static constexpr unsigned kVoiceCounts[] = {12, 2, 2, 1, 1};

    static Sample Synth(Cue cue) {
        Sample sample;
        const std::vector<float> wave = MakeCue(cue);
        sample.format.format = SDL_AUDIO_F32;
        sample.format.channels = 1;
        sample.format.freq = kSampleRate;
        sample.pcm.resize(wave.size() * sizeof(float));
        if (!wave.empty()) std::memcpy(sample.pcm.data(), wave.data(), sample.pcm.size());
        return sample;
    }

    static bool LoadSample(const char* directory, Cue cue, Sample& out) {
        if (!directory || !*directory) return false;
        const std::string path = std::string(directory) + "/" + kFilenames[unsigned(cue)];
        SDL_AudioSpec spec{};
        Uint8* raw = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV(path.c_str(), &spec, &raw, &length)) return false;
        // Guard against accidentally loading a giant recording instead of one menu cue.
        const bool valid = length > 0 && length <= 2u * 1024u * 1024u &&
                           spec.channels >= 1 && spec.channels <= 2 &&
                           spec.freq >= 8000 && spec.freq <= 192000;
        if (valid) {
            out.format = spec;
            out.pcm.assign(raw, raw + length);
            out.recorded = true;
        } else {
            std::fprintf(stderr, "Overlay sounds: invalid/oversized WAV: %s\n", path.c_str());
        }
        SDL_free(raw);
        return valid;
    }

public:
    void Init() {
        if (initialized_) return;
        initialized_ = true;
        if (const char* setting = std::getenv("PS4RT_MENU_SOUNDS");
            setting && (!std::strcmp(setting, "0") || !std::strcmp(setting, "off"))) {
            std::printf("Overlay sounds: disabled by PS4RT_MENU_SOUNDS\n");
            return;
        }
        const char* volume = std::getenv("PS4RT_MENU_SOUND_VOLUME");
        gain_ = volume ? float(std::clamp(std::atoi(volume), 0, 100)) / 100.0f : 0.65f;
        if (gain_ <= 0.0f) return;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "Overlay sounds: SDL audio init failed: %s\n", SDL_GetError());
            return;
        }

        const char* directory = std::getenv("PS4RT_MENU_SOUND_DIR");
        unsigned recorded_count = 0;
        for (unsigned i = 0; i < unsigned(Cue::Count); ++i) {
            if (LoadSample(directory, Cue(i), samples_[i])) {
                ++recorded_count;
            } else {
                samples_[i] = Synth(Cue(i));
            }
        }
        device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!device_) {
            std::fprintf(stderr, "Overlay sounds: could not open audio device: %s\n", SDL_GetError());
            return;
        }

        for (unsigned i = 0; i < unsigned(Cue::Count); ++i) {
            const Sample& sample = samples_[i];
            auto& voices = voices_[i];
            for (unsigned j = 0; j < kVoiceCounts[i]; ++j) {
                SDL_AudioStream* voice = SDL_CreateAudioStream(&sample.format, nullptr);
                if (!voice) break;
                if (!SDL_BindAudioStream(device_, voice)) {
                    SDL_DestroyAudioStream(voice);
                    break;
                }
                // Repeated navigation chimes overlap; reduce their gain slightly.
                SDL_SetAudioStreamGain(voice, gain_ * (i == unsigned(Cue::Navigate) ? 0.80f : 1.0f));
                voices.push_back(voice);
            }
        }

        unsigned streams = 0;
        for (const auto& voices : voices_) streams += unsigned(voices.size());
        if (!streams) {
            std::fprintf(stderr, "Overlay sounds: failed to bind audio streams: %s\n", SDL_GetError());
            SDL_CloseAudioDevice(device_);
            device_ = 0;
            return;
        }
        SDL_ResumeAudioDevice(device_);
        std::printf("Overlay sounds: ready (%u WAV cues, %u voices, volume %d%%)\n",
                    recorded_count, streams, int(gain_ * 100.0f));
    }

    void Play(Cue cue) {
        const unsigned index = unsigned(cue);
        if (index >= unsigned(Cue::Count) || !device_) return;
        if (cue == Cue::Navigate) {
            const Uint64 now = SDL_GetTicks();
            // Don't let analog jitter or frame-rate variation create an audio storm.
            // Shorter than ImGui's held-key repeat interval so each repeat can sound.
            if (last_navigation_ms_ && now - last_navigation_ms_ < 40) return;
            last_navigation_ms_ = now;
        }
        auto& voices = voices_[index];
        const Sample& sample = samples_[index];
        if (voices.empty() || sample.pcm.empty()) return;

        // Rotate through voices so tails overlap instead of truncating one another.
        SDL_AudioStream* stream = voices[next_voice_[index]++ % voices.size()];
        SDL_ClearAudioStream(stream); // clears only a voice last used many repeats ago
        SDL_PutAudioStreamData(stream, sample.pcm.data(), int(sample.pcm.size()));
    }
};

} // namespace Ps4RtMenuSounds
