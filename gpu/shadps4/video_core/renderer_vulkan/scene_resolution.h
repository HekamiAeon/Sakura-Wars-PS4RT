// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
/// The game's own sizes this run: the 3D scene (the engine's --renderres) and the frame the HUD,
/// dialogue and 2D screens draw in (its --res, the display buffer). 1920x1080 at the game's own
/// resolution; the renderer sets both once at start from the menu's Internal resolution
/// (Ps4RtSettings::SceneSize, GameDisplaySize). They differ only with manual engine options.
inline Size game_scene{}, game_frame{};
inline bool IsGameScene(uint32_t width, uint32_t height) {
    return width == game_scene.width && height == game_scene.height;
}
inline bool IsGameFrame(uint32_t width, uint32_t height) {
    return width == game_frame.width && height == game_frame.height;
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
