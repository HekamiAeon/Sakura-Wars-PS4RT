// SPDX-License-Identifier: GPL-2.0-or-later
// CUSA16429 scene profile for Native AA: the PostEdge composite binds the main camera's view
// constants and draws into the HDR scene color with the scene depth; FSR runs before tonemap.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace Vulkan::SakuraScene {
constexpr unsigned long long PostEdgeFs = 0x7c880249; ///< Scene composite (FxEdgeParameter).
constexpr unsigned long long TonemapFs = 0x23aaa124;  ///< Tonemap seen in the first area (varies).
constexpr unsigned ViewBuffer = 1, ViewBytes = 3792;   ///< PostEdge fragment buffer slot/size.
/// 2D quads (dialogue box, subtitles) drawn into the finished frame before the display pass.
constexpr unsigned long long FrameUiVs = 0xf678d6a5;
/// Texel copy (fetch -> storage write) of the finished frame, with its UI, for the final colour
/// pass (ps 38ddb12f, the brightness curve); also copies it before the UI.
constexpr unsigned long long FrameCopyCs = 0x98b85f76;

struct Camera {
    std::array<float, 12> view{}, inverse_view{}; ///< 3x4 rows, view space +Z forward
    std::array<float, 4> projection{};           ///< x scale, y scale, z scale, z offset
};

/// View constants hold row-vector matrices: world-to-view at float 0, projection at 16.
/// Game view space looks down -Z (clip w = -z); PS4RT cameras look down +Z, so the forward
/// row is the negated third column and depth = zs + zo / z with zs = -P22, zo = P32.
inline bool ReadView(std::span<const float> data, Camera& result, const char** why = nullptr) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (data.size() != ViewBytes / sizeof(float)) return fail("size");
    for (float value : data.first(32))
        if (!std::isfinite(value)) return fail("nonfinite");
    const auto v = [&](unsigned r, unsigned c) { return double(data[r * 4 + c]); };
    const auto p = [&](unsigned r, unsigned c) { return double(data[16 + r * 4 + c]); };
    if (std::abs(v(0, 3)) > 1e-4 || std::abs(v(1, 3)) > 1e-4 || std::abs(v(2, 3)) > 1e-4 ||
        std::abs(v(3, 3) - 1) > 1e-4) return fail("view not affine");
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned b = 0; b < 3; ++b) {
            const double dot = v(0, a) * v(0, b) + v(1, a) * v(1, b) + v(2, a) * v(2, b);
            if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 3e-3) return fail("view not a rotation");
        }
    // A centered perspective with clip w = -z; shadow (orthographic) and UI projections fail.
    const double xs = p(0, 0), ys = p(1, 1), zs = -p(2, 2), zo = p(3, 2);
    if (!(xs > 0.05 && xs < 100 && ys > 0.05 && ys < 100)) return fail("projection scale");
    const double scale = std::max(xs, ys);
    for (auto [r, c] : {std::array{0u, 1u}, {0u, 2u}, {0u, 3u}, {1u, 0u}, {1u, 2u}, {1u, 3u},
                        {2u, 0u}, {2u, 1u}, {3u, 0u}, {3u, 1u}})
        if (std::abs(p(r, c)) > 1e-5 * scale) return fail("projection not centered");
    if (std::abs(p(2, 3) + 1) > 1e-5 || std::abs(p(3, 3)) > 1e-5) return fail("not perspective");
    // Finite reversed depth (near 1, far 0), as the measured scene uses.
    if (!(zs < 0 && zo > 0)) return fail("not finite reversed depth");
    const double near_plane = zo / (1 - zs), far_plane = -zo / zs;
    if (!(near_plane > 0.001 && far_plane > near_plane && far_plane < 1e8)) return fail("planes");
    Camera camera;
    for (unsigned c = 0; c < 4; ++c) {
        camera.view[c] = float(v(c, 0));
        camera.view[4 + c] = float(v(c, 1));
        camera.view[8 + c] = float(-v(c, 2));
    }
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c) camera.inverse_view[r * 4 + c] = camera.view[c * 4 + r];
        for (unsigned c = 0; c < 3; ++c)
            camera.inverse_view[r * 4 + 3] -= camera.view[c * 4 + r] * camera.view[c * 4 + 3];
    }
    camera.projection = {float(xs), float(ys), float(zs), float(zo)};
    result = camera;
    return true;
}

/// PostEdge constants (fragment slot 0, 256 bytes): words 2/3 hold the inverse native size,
/// word 5 enables the normal (surface crease) edges, words 15 and 23 are the two outline color
/// strengths. Mode 1 keeps depth (silhouette) edges only; mode 2 removes the outline color.
constexpr unsigned EdgeBuffer = 0, EdgeBytes = 256;
inline bool ApplyOutlineMode(std::span<const std::uint8_t> bytes,
                             std::array<std::uint8_t, EdgeBytes>& copy, int mode) {
    if (bytes.size() != EdgeBytes || (mode != 1 && mode != 2)) return false;
    float inv_w, inv_h;
    std::memcpy(&inv_w, bytes.data() + 2 * 4, 4);
    std::memcpy(&inv_h, bytes.data() + 3 * 4, 4);
    if (!(inv_w > 0 && inv_w < 0.01f && inv_h > 0 && inv_h < 0.01f)) return false;
    std::memcpy(copy.data(), bytes.data(), EdgeBytes);
    constexpr float zero = 0.0f;
    if (mode == 1) {
        std::memcpy(copy.data() + 5 * 4, &zero, 4);
    } else {
        std::memcpy(copy.data() + 15 * 4, &zero, 4);
        std::memcpy(copy.data() + 23 * 4, &zero, 4);
    }
    return true;
}

} // namespace Vulkan::SakuraScene
