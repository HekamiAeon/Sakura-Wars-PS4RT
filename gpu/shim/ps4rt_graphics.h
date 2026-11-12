// SPDX-License-Identifier: GPL-2.0-or-later
// ps4rt: graphics options of the in-game menu (Insert / R3+L2), kept in ps4rt.ini. The live ones
// apply on the next frame; the startup ones (patches, GPU readback modes) are read by run.py and
// apply through "Apply and restart". The launcher reads and writes the same keys.
#pragma once

#include <array>
#include <string_view>

namespace Ps4RtGraphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, Fullscreen = 2, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {
    "windowed", "borderless", "fullscreen"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {
    "Windowed", "Borderless", "Fullscreen"};

/// Present rate limits; 0 = unlimited.
inline constexpr std::array<int, 5> FpsLimits = {30, 60, 120, 240, 0};

/// Internal render resolutions (applied on restart; the Code Fairy port's list and keys): "native"
/// is the game's own 1080p. The launcher (scripts/ps4rt_launch.py) maps the same keys to
/// PS4RT_INTERNAL_RES; the loader hands the engine its own --res/--renderres options and sizes the
/// render target pool, GPU heap and direct memory for it. Wider than 16:9 is experimental.
inline constexpr std::array<std::string_view, 10> Resolutions = {
    "native", "720p", "540p", "1440p", "2160p", "2560x1080", "3440x1440", "3840x1440", "3840x1080", "5120x1440"};
/// "540p" renders at 1024x576 as in the Code Fairy port (a height that is a multiple of 8).
inline constexpr std::array<int, 10> ResolutionWidths = {1920, 1280, 1024, 2560, 3840, 2560, 3440, 3840, 3840, 5120};
inline constexpr std::array<int, 10> ResolutionHeights = {1080, 720, 576, 1440, 2160, 1080, 1440, 1440, 1080, 1440};
/// Menu order (16:9, 21:9, 24:9, 32:9) and labels.
inline constexpr std::array<int, 10> ResolutionMenuOrder = {2, 1, 0, 3, 4, 5, 6, 7, 8, 9};
inline constexpr std::array<const char*, 10> ResolutionMenuLabels = {
    "1024 x 576", "1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160 (4K)", "2560 x 1080",
    "3440 x 1440", "3840 x 1440", "3840 x 1080", "5120 x 1440"};
struct ResolutionMenuGroup {
    const char* label;
    int first;
    int count;
};
inline constexpr std::array<ResolutionMenuGroup, 4> ResolutionMenuGroups = {{
    {"16:9", 0, 5}, {"21:9", 5, 2}, {"24:9", 7, 1}, {"32:9", 8, 2}}};
/// Engine frame rate ceilings: 60 is the game's own, 120 and 240 are frame rate patches.
inline constexpr std::array<int, 3> EngineFps = {60, 120, 240};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Windowed);
}
/// A listed limit, else unlimited.
inline int ParseFpsLimit(int value) {
    return IndexOf(FpsLimits, value, -1) >= 0 ? value : 0;
}
/// Index into Resolutions; unknown text is native.
inline int ParseResolution(std::string_view text) {
    return IndexOf(Resolutions, text, 0);
}
/// A listed ceiling, else 120 (the default patch).
inline int ParseEngineFps(int value) {
    return IndexOf(EngineFps, value, -1) >= 0 ? value : 120;
}

/// Anisotropic filtering forced on the game's mipmapped textures; 0 = the game's own. Samplers
/// are cached, so it applies on restart.
inline constexpr std::array<int, 5> Anisotropies = {0, 2, 4, 8, 16};
inline int ParseAnisotropy(int value) {
    return IndexOf(Anisotropies, value, -1) >= 0 ? value : 0;
}

/// The game's own frame rate (its simulation step and flip interval), set through a per-game
/// hook (ps4rt_framerate.h); the menu lists the rates up to the hook's maximum. Live.
inline constexpr std::array<int, 4> Framerates = {30, 60, 90, 120};
inline int ParseFramerate(int value) {
    return IndexOf(Framerates, value, -1) >= 0 ? value : 30;
}

/// Frames the GPU command thread may queue ahead of the GPU; 0 = unbounded.
inline constexpr std::array<int, 3> FramesQueued = {1, 2, 0};
inline int ParseFramesQueued(int value) {
    return IndexOf(FramesQueued, value, -1) >= 0 ? value : 1;
}
/// RCAS strength in percent; invalid values fall back to 75.
inline int ParseRcasStrength(int value) {
    return value >= 0 && value <= 100 ? value : 75;
}
/// FSR RCAS attenuation for a strength: 0 is the sharpest, 2 the softest.
inline float RcasAttenuation(int strength) {
    return 2.0f * (1.0f - float(ParseRcasStrength(strength)) / 100.0f);
}
/// Whether the FSR pass runs: FSR 1 when the window is larger than the game's image, RCAS
/// alone when they are the same size. A smaller window is left to the regular scaling.
inline bool FsrPassRuns(bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool upscale = in_w < out_w && in_h < out_h;
    const bool same = in_w == out_w && in_h == out_h;
    return (fsr1 && upscale) || (rcas && same);
}

/// What the present limit really allows: the engine ceiling caps it.
inline int EffectiveFps(int limit, int engine_fps) {
    return limit == 0 || limit > engine_fps ? engine_fps : limit;
}

/// The present thread's frame limit: never above the engine rate the patches applied, since the
/// engine steps a fixed time per frame and faster flips speed the game up. 0 stays vblank paced.
inline int PresentLimit(int limit, int engine_fps) {
    return limit == 0 ? 0 : EffectiveFps(limit, engine_fps);
}

} // namespace Ps4RtGraphics
