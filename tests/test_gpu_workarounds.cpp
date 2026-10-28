// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "ps4rt_gpu_workarounds.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

int main() {
    using namespace AmdGpu;
    using namespace Vulkan::LiverpoolToVK;
    using namespace Ps4RtGpuWorkarounds;
    Configure("");
    assert(Current().preserve_rgba8_cpu_writes);
    assert(!Current().preserve_transition_snapshot);
    assert(TrySurfaceFormat(DataFormat::FormatBc6, NumberFormat::Float) == vk::Format::eUndefined);
    Configure("unsigned", true, true, true);
    assert(Current().preserve_transition_snapshot);
    assert(Current().preserve_wqm_helpers);
    assert(Current().fixed_grass_view_lighting);
    assert(TrySurfaceFormat(DataFormat::FormatBc6, NumberFormat::Float) == vk::Format::eBc6HUfloatBlock);
    Configure("signed");
    assert(TrySurfaceFormat(DataFormat::FormatBc6, NumberFormat::Float) == vk::Format::eBc6HSfloatBlock);
    assert(TrySurfaceFormat(DataFormat::FormatBc7, NumberFormat::Float) == vk::Format::eUndefined);
    assert(TrySurfaceFormat(DataFormat::FormatBc6, NumberFormat::Uint) == vk::Format::eUndefined);
    std::puts("PASS: PS4RT inherited GPU workaround configuration");
}
