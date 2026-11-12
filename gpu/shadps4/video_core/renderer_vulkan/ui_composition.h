// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include "video_core/renderer_vulkan/scene_resolution.h"

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

// A viewport covering the game's frame (the HUD's display buffer).
inline bool NativeViewport(float width, float height) {
    return std::abs(std::abs(width) - float(SceneResolution::game_frame.width)) < 0.5f &&
           std::abs(std::abs(height) - float(SceneResolution::game_frame.height)) < 0.5f;
}
// A viewport covering the 3D scene (the same as the frame unless manual engine options differ).
inline bool SceneViewport(float width, float height) {
    return std::abs(std::abs(width) - float(SceneResolution::game_scene.width)) < 0.5f &&
           std::abs(std::abs(height) - float(SceneResolution::game_scene.height)) < 0.5f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    return {float(output_width) / float(native_coordinates ? SceneResolution::game_frame.width : guest_width),
            float(output_height) / float(native_coordinates ? SceneResolution::game_frame.height : guest_height)};
}

// Native UI shaders address guest-size masks with FragCoord (the Teletron phone and the
// minimap). The display pass instead uses physical pixels to fetch the output-size frame,
// so it must keep its unscaled FragCoord even though its attachment is redirected too.
// The recompiler converts FragCoord by 1920x1080 / this size (shader_recompiler
// emit_spirv_context_get_set.cpp), so the output size is given in 1920x1080 units of the
// game's frame: exactly the output size for a 1920x1080 frame, else output x 1920 / frame
// (3840x1080 to 5120x1440: 2560x1440), rounded (within a pixel at the far edge).
inline uint32_t FragmentSize(bool native_ui, uint32_t guest_width, uint32_t guest_height,
                             uint32_t output_width, uint32_t output_height) {
    if (!native_ui || !SceneResolution::IsGameFrame(guest_width, guest_height) || !guest_width ||
        !guest_height)
        return 0;
    const uint64_t w = (uint64_t(output_width) * 1920 + guest_width / 2) / guest_width;
    const uint64_t h = (uint64_t(output_height) * 1080 + guest_height / 2) / guest_height;
    if (!w || !h || w > 65535 || h > 65535) return 0;
    return uint32_t(w) | (uint32_t(h) << 16);
}

} // namespace Vulkan::UiComposition
