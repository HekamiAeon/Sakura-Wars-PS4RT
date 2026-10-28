// SPDX-License-Identifier: GPL-2.0-or-later
// Template-runtime native-scene geometry constants: rendered clip transform at0, affine world
// at16.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace Vulkan::Ps4RtCamera {
struct Camera {
    std::array<float, 12> view{}, inverse_view{};
    std::array<float, 4> projection{};
};

inline bool Read(std::span<const float> data, Camera& result) {
    if (data.size() != 356) return false;
    // Use the transform driving position export. The shared VP at224 can describe
    // a different view. Recover VP = MVP * inverse(world); world is three rows.
    // General affine inverse adapted from the supplied reference CameraMotion.
    for (float value : data.first(28)) if (!std::isfinite(value)) return false;
    const auto m = data.subspan(16,12);
    const double a=m[0], b=m[1], c=m[2], d=m[4], e=m[5], f=m[6], g=m[8], h=m[9], i=m[10];
    const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    const double scale=std::max({std::abs(a),std::abs(b),std::abs(c),std::abs(d),
        std::abs(e),std::abs(f),std::abs(g),std::abs(h),std::abs(i)});
    if (scale == 0 || std::abs(det) <= 1e-12*scale*scale*scale) return false;
    const double s=1/det;
    const std::array<double,9> inverse={
        (e*i-f*h)*s,(c*h-b*i)*s,(b*f-c*e)*s,
        (f*g-d*i)*s,(a*i-c*g)*s,(c*d-a*f)*s,
        (d*h-e*g)*s,(b*g-a*h)*s,(a*e-b*d)*s};
    std::array<double,12> world_inverse{};
    for (unsigned r=0;r<3;++r) {
        for (unsigned k=0;k<3;++k) {
            world_inverse[r*4+k]=inverse[r*3+k];
            world_inverse[r*4+3]-=inverse[r*3+k]*m[k*4+3];
        }
    }
    std::array<double,16> matrix{};
    for (unsigned r=0;r<4;++r)
        for (unsigned k=0;k<4;++k) {
            matrix[r*4+k]=k==3 ? data[r*4+3] : 0;
            for (unsigned j=0;j<3;++j)
                matrix[r*4+k]+=double(data[r*4+j])*world_inverse[j*4+k];
        }
    for (double value : matrix) if (!std::isfinite(value)) return false;
    const auto dot = [&](unsigned a, unsigned b) {
        double sum = 0;
        for (unsigned k = 0; k < 3; ++k) sum += double(matrix[a+k]) * matrix[b+k];
        return sum;
    };
    const double xs = std::sqrt(dot(0,0)), ys = std::sqrt(dot(4,4));
    if (xs < 0.05 || xs > 100 || ys < 0.05 || ys > 100 ||
        std::abs(dot(12,12)-1) > 0.003 || std::abs(dot(0,4)/(xs*ys)) > 0.003 ||
        std::abs(dot(0,12)/xs) > 0.003 || std::abs(dot(4,12)/ys) > 0.003) return false;
    const double zs = dot(8,12), zo = matrix[11] - zs * matrix[15];
    // This profile is finite reversed-Z (near1, far0); reject shadows/UI/unknown projections.
    if (!(zs < 0 && zo > 0)) return false;
    for (unsigned k = 0; k < 3; ++k)
        if (std::abs(matrix[8+k] - zs*matrix[12+k]) > std::max(1e-7, std::abs(zs)*0.003))
            return false;
    const double near_plane = zo/(1-zs), far_plane = -zo/zs;
    if (!(near_plane > 0.001 && far_plane > near_plane && far_plane < 1e8)) return false;
    Camera camera;
    for (unsigned k = 0; k < 4; ++k) {
        camera.view[k] = float(matrix[k]/xs);
        camera.view[4+k] = float(matrix[4+k]/ys);
        camera.view[8+k] = matrix[12+k];
    }
    for (float value : camera.view) if (!std::isfinite(value)) return false;
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c) camera.inverse_view[r*4+c] = camera.view[c*4+r];
        for (unsigned c = 0; c < 3; ++c)
            camera.inverse_view[r*4+3] -= camera.view[c*4+r]*camera.view[c*4+3];
    }
    camera.projection = {float(xs),float(ys),float(zs),float(zo)};
    for (float value : camera.inverse_view) if (!std::isfinite(value)) return false;
    result = camera;
    return true;
}

constexpr bool SceneCopy(unsigned long long vs, unsigned long long ps) {
    return vs == 0x12eb9af0 && ps == 0x13d64dc6;
}
constexpr bool Tonemap(unsigned long long vs, unsigned long long ps) {
    return vs == 0x12eb9af0 && ps == 0xfc17f234;
}
} // namespace Vulkan::Ps4RtCamera
