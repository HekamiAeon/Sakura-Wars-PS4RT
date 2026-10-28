// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/scene_camera_history.h"

int main() {
    struct Camera { float position = 0; bool valid = false; };
    Vulkan::SceneCameraHistory<Camera,unsigned> passes;
    Camera current,previous;
    // Auxiliary camera remains still while the main scene moves. Picking the
    // frame's first camera falsely generates zero vectors for the main depth.
    for (unsigned frame = 1; frame <= 4; ++frame) {
        assert(passes.BeginPass(1));
        assert(passes.SetCamera({0,true}));
        assert(passes.BeginPass(2));
        assert(!passes.HasCamera());
        assert(!passes.BeginPass(2));
        assert(passes.SetCamera({float(frame*20),true}));
        assert(!passes.SetCamera({999,true}));
        assert(passes.Commit(current,previous));
        assert(current.position == frame*20);
        assert(previous.position == (frame-1)*20);
        assert(previous.valid == (frame != 1));
        // Motion and display callbacks must not advance history twice.
        assert(!passes.Commit(current,previous));
        assert(previous.position == (frame-1)*20);
        passes.EndFrame();
    }
    // A new depth with no mapped camera must fall back to native rendering;
    // neither the auxiliary camera nor the last frame's camera can fill in.
    passes.BeginPass(1);
    passes.SetCamera({0,true});
    passes.BeginPass(2);
    assert(!passes.HasCamera() && !passes.Commit(current,previous));
    assert(current.position == 80 && previous.position == 60);
    passes.EndFrame();
    assert(passes.BeginPass(2));
    assert(passes.SetCamera({100,true}));
    assert(passes.Commit(current,previous));
    assert(current.position == 100 && previous.position == 80);
    std::puts("PASS: camera follows scene depth across auxiliary passes; history commits once; missing camera rejected");
}
