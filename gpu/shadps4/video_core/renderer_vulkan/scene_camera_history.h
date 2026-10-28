// SPDX-License-Identifier: GPL-2.0-or-later
// Pair a geometry pass's camera with its depth; advance frame history only once.
#pragma once

namespace Vulkan {
template <typename Camera, typename Depth> class SceneCameraHistory {
public:
    bool BeginPass(Depth next) {
        if (next == depth) return false;
        depth = next;
        has_camera = false;
        return true;
    }
    bool HasCamera() const { return has_camera; }
    bool SetCamera(const Camera& value) {
        if (has_camera) return false;
        camera = value;
        has_camera = true;
        return true;
    }
    bool Commit(Camera& current, Camera& previous) {
        if (committed || !has_camera) return false;
        previous = current;
        current = camera;
        committed = true;
        return true;
    }
    void EndFrame() {
        depth = {};
        has_camera = committed = false;
    }
private:
    Depth depth{};
    Camera camera{};
    bool has_camera = false, committed = false;
};
} // namespace Vulkan
