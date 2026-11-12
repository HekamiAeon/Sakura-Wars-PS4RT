// SPDX-License-Identifier: GPL-2.0-or-later
// The pipeline-binary disk cache choice: off on NVIDIA by default, PS4RT_PIPELINE_BINARIES wins.
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_pipeline_binary_policy.h"

using Vulkan::ChoosePipelineBinaries;
using Vulkan::PipelineBinaryChoice;

int main() {
    constexpr std::uint32_t amd = 0x1002, intel = 0x8086, nvidia = 0x10de;
    // Unset (or empty): on, except NVIDIA.
    assert(ChoosePipelineBinaries(nullptr, amd) == PipelineBinaryChoice::On);
    assert(ChoosePipelineBinaries(nullptr, intel) == PipelineBinaryChoice::On);
    assert(ChoosePipelineBinaries(nullptr, nvidia) == PipelineBinaryChoice::OffOnNvidia);
    assert(ChoosePipelineBinaries("", nvidia) == PipelineBinaryChoice::OffOnNvidia);
    assert(ChoosePipelineBinaries("", amd) == PipelineBinaryChoice::On);
    // 0: off on any GPU.
    assert(ChoosePipelineBinaries("0", amd) == PipelineBinaryChoice::OffBySetting);
    assert(ChoosePipelineBinaries("0", nvidia) == PipelineBinaryChoice::OffBySetting);
    // Any other value forces it on, NVIDIA included.
    assert(ChoosePipelineBinaries("1", nvidia) == PipelineBinaryChoice::On);
    assert(ChoosePipelineBinaries("1", amd) == PipelineBinaryChoice::On);
    std::puts("pipeline binary policy: ok");
    return 0;
}
