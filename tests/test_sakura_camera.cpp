// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "video_core/renderer_vulkan/sakura_scene.h"

using Vulkan::SakuraScene::Camera;
using Vulkan::SakuraScene::ReadView;

namespace {
// Row-vector view/projection like the game's view constants (synthetic, not game data).
std::vector<float> Constants(double yaw, double pitch, const double eye[3]) {
    std::vector<float> d(Vulkan::SakuraScene::ViewBytes / 4, 0.0f);
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // Camera axes in world space: right, up, back (+Z behind the camera).
    const double right[3] = {cy, 0, -sy};
    const double up[3] = {sy * sp, cp, cy * sp};
    const double back[3] = {sy * cp, -sp, cy * cp};
    const double* axes[3] = {right, up, back};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) d[i * 4 + j] = float(axes[j][i]);
        d[12 + j] = float(-(axes[j][0] * eye[0] + axes[j][1] * eye[1] + axes[j][2] * eye[2]));
    }
    d[15] = 1;
    const double n = 0.2, f = 1000;
    d[16 + 0] = 1.5455f;
    d[16 + 5] = 2.7475f;
    d[16 + 10] = float(n / (f - n));     // reversed depth: near 1, far 0
    d[16 + 11] = -1;
    d[16 + 14] = float(f * n / (f - n));
    return d;
}
} // namespace

int main() {
    const double eye[3] = {6.0, 0.8, 6.0};
    auto d = Constants(0.3, -0.15, eye);
    Camera c;
    assert(ReadView(d, c));
    // Inverse view returns the eye position.
    assert(std::abs(c.inverse_view[3] - eye[0]) < 1e-4 && std::abs(c.inverse_view[7] - eye[1]) < 1e-4 &&
           std::abs(c.inverse_view[11] - eye[2]) < 1e-4);
    const double near_plane = c.projection[3] / (1 - c.projection[2]);
    const double far_plane = -c.projection[3] / c.projection[2];
    assert(std::abs(near_plane - 0.2) < 1e-4 && std::abs(far_plane - 1000) < 1);
    // Converted camera reproduces the game's NDC and depth for world points in front.
    const double points[][3] = {{3, 1, 1}, {4.5, 2, -3}, {0, 0.5, -20}};
    for (const auto& w : points) {
        double game_view[3]{}, out[4]{};
        for (int j = 0; j < 3; ++j) {
            game_view[j] = d[12 + j];
            for (int i = 0; i < 3; ++i) game_view[j] += w[i] * double(d[i * 4 + j]);
        }
        for (int j = 0; j < 4; ++j) {
            out[j] = d[16 + 12 + j];
            for (int i = 0; i < 3; ++i) out[j] += game_view[i] * double(d[16 + i * 4 + j]);
        }
        assert(out[3] > 0);
        double view[3]{};
        for (int r = 0; r < 3; ++r)
            view[r] = c.view[r * 4] * w[0] + c.view[r * 4 + 1] * w[1] + c.view[r * 4 + 2] * w[2] +
                      c.view[r * 4 + 3];
        assert(std::abs(view[0] * c.projection[0] / view[2] - out[0] / out[3]) < 1e-4);
        assert(std::abs(view[1] * c.projection[1] / view[2] - out[1] / out[3]) < 1e-4);
        assert(std::abs(c.projection[2] + c.projection[3] / view[2] - out[2] / out[3]) < 1e-5);
    }
    // Rejections: wrong size, off-center projection, orthographic, non-rotation view.
    const char* why = nullptr;
    assert(!ReadView(std::span(d).first(100), c, &why) && why);
    auto bad = d; bad[16 + 8] = 0.1f; assert(!ReadView(bad, c));
    bad = d; bad[16 + 11] = 0; bad[16 + 15] = 1; assert(!ReadView(bad, c));
    bad = d; bad[0] *= 2; assert(!ReadView(bad, c));
    bad = d; bad[16 + 10] = 0; assert(!ReadView(bad, c)); // infinite far: no far plane

    // Outline modes change only the selected words of a copy.
    std::array<std::uint8_t, 256> edge{}, out{};
    const auto put = [&](unsigned word, float value) { std::memcpy(edge.data() + word * 4, &value, 4); };
    for (unsigned i = 0; i < 64; ++i) put(i, 0.5f + float(i));
    put(2, 1.f / 1920.f);
    put(3, 1.f / 1080.f);
    const auto changed = [&] {
        std::vector<unsigned> words;
        for (unsigned i = 0; i < 64; ++i)
            if (std::memcmp(edge.data() + i * 4, out.data() + i * 4, 4)) words.push_back(i);
        return words;
    };
    assert(Vulkan::SakuraScene::ApplyOutlineMode(edge, out, 1) && changed() == std::vector<unsigned>{5});
    assert(Vulkan::SakuraScene::ApplyOutlineMode(edge, out, 2) &&
           (changed() == std::vector<unsigned>{15, 23}));
    assert(!Vulkan::SakuraScene::ApplyOutlineMode(edge, out, 0));
    assert(!Vulkan::SakuraScene::ApplyOutlineMode(std::span(edge).first(128), out, 1));
    put(2, 1.0f); // not an inverse size
    assert(!Vulkan::SakuraScene::ApplyOutlineMode(edge, out, 1));
}
