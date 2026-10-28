// SPDX-License-Identifier: GPL-2.0-or-later
// Owned finite reversed-Z camera fixtures; no captured game constants.
#include <cassert>
#include <cstdio>
#include <limits>
#include "video_core/renderer_vulkan/ps4rt_camera.h"

int main() {
    using namespace Vulkan::Ps4RtCamera;
    constexpr float near_plane = 2, far_plane = 20000;
    constexpr float zs = -near_plane/(far_plane-near_plane);
    constexpr float zo = near_plane*far_plane/(far_plane-near_plane);
    constexpr float xs = 1.25f, ys = 2.25f;
    const float c = std::cos(0.37f), s = std::sin(0.37f);
    const std::array<float,12> view{c,0,s,13, 0,1,0,-7, -s,0,c,29};
    std::array<float,356> data{};
    const auto geometry = [&](const std::array<float,12>& camera_view,
                              const std::array<float,12>& world) {
        std::array<double,16> vp{};
        for (unsigned k=0;k<4;++k) {
            vp[k]=camera_view[k]*xs;
            vp[4+k]=camera_view[4+k]*ys;
            vp[8+k]=camera_view[8+k]*zs+(k==3 ? zo : 0);
            vp[12+k]=camera_view[8+k];
        }
        std::copy(world.begin(),world.end(),data.begin()+16);
        for (unsigned r=0;r<4;++r)
            for (unsigned c=0;c<4;++c) {
                double value=c==3 ? vp[r*4+3] : 0;
                for (unsigned k=0;k<3;++k) value+=vp[r*4+k]*world[k*4+c];
                data[r*4+c]=float(value);
            }
    };
    const std::array<float,12> identity{1,0,0,0, 0,1,0,0, 0,0,1,0};
    const std::array<float,12> world{0,0,2,137, 0,3,0,-59, -.5f,0,0,83};
    geometry(view,world);
    for (unsigned i = 0; i < 4; ++i) {
        data[224+i] = view[i]*xs;
        data[228+i] = view[4+i]*ys;
        data[232+i] = view[8+i]*zs + (i == 3 ? zo : 0);
        data[236+i] = view[8+i];
    }
    // Other packed constants may look like NaN; inspect only the camera region.
    data[70] = std::numeric_limits<float>::quiet_NaN();
    Camera camera;
    assert(Read(data,camera));
    for (unsigned i = 0; i < 12; ++i) assert(std::abs(camera.view[i]-view[i]) < 1e-4);
    assert(std::abs(camera.projection[3]/(1-camera.projection[2])-near_plane) < 1e-4);
    assert(std::abs(-camera.projection[3]/camera.projection[2]-far_plane) < 0.02);
    // Verify inverse rotation and translation independently by a point roundtrip.
    const std::array<float,3> point{17,-31,59};
    std::array<float,3> transformed{}, restored{};
    for (unsigned r = 0; r < 3; ++r) {
        transformed[r] = camera.view[r*4+3];
        for (unsigned k = 0; k < 3; ++k) transformed[r] += camera.view[r*4+k]*point[k];
    }
    for (unsigned r = 0; r < 3; ++r) {
        restored[r] = camera.inverse_view[r*4+3];
        for (unsigned k = 0; k < 3; ++k) restored[r] += camera.inverse_view[r*4+k]*transformed[k];
        assert(std::abs(restored[r]-point[r]) < 1e-4);
    }
    // Keep the shared VP cached while the transform driving the GPU moves.
    auto moved=view;
    moved[3]+=41; moved[7]-=17; moved[11]+=31;
    geometry(moved,world);
    assert(Read(data,camera));
    for (unsigned i=0;i<12;++i) assert(std::abs(camera.view[i]-moved[i])<2e-4);
    const std::array<float,12> reflected{-2,0,0,-91, 0,.75f,0,37, 0,0,1.5f,15};
    geometry(moved,reflected);
    data[235]=std::numeric_limits<float>::quiet_NaN();
    assert(Read(data,camera));
    for (unsigned i=0;i<12;++i) assert(std::abs(camera.view[i]-moved[i])<2e-4);
    geometry(view,identity);
    assert(!Read(std::span(data).first(355),camera));
    for (unsigned offset : {0u,4u,8u,12u,16u,20u,24u}) {
        auto invalid = data;
        invalid[offset] = std::numeric_limits<float>::infinity();
        assert(!Read(invalid,camera));
    }
    auto invalid = data;
    invalid[12] *= 2;
    assert(!Read(invalid,camera));
    invalid = data;
    invalid[1] = 1;
    assert(!Read(invalid,camera));
    invalid = data;
    invalid[11] = -1; // This template expects the working reversed-depth projection layout.
    assert(!Read(invalid,camera));
    invalid=data;
    std::fill_n(invalid.begin()+16,12,0.f);
    assert(!Read(invalid,camera));
    invalid = {};
    std::copy(identity.begin(),identity.end(),invalid.begin()+16);
    invalid[0] = invalid[5] = invalid[10] = invalid[15] = 1; // UI orthographic.
    assert(!Read(invalid,camera));
    assert(SceneCopy(0x12eb9af0,0x13d64dc6));
    assert(Tonemap(0x12eb9af0,0xfc17f234));
    assert(!SceneCopy(0x59cb445a,0xf8591c05));
    assert(!SceneCopy(0x12eb9af0,0xfc17f234));
    std::puts("PASS: rendered camera, stale shared VP, affine scale/reflection, reversed depth and guards");
}
