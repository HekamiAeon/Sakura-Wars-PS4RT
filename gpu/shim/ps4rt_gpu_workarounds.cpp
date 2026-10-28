// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_gpu_workarounds.h"

namespace Ps4RtGpuWorkarounds {
namespace {
GpuWorkarounds workarounds{};
}

void Configure(std::string_view bc6_diagnostic, bool wqm_diagnostic,
               bool fixed_grass_view_lighting, bool preserve_transition_snapshot) {
    workarounds = {};
    // BC6/Float and transition behavior come from the working native-port baseline.
    // Transition snapshots are rendered on the GPU, then the guest writes their
    // alpha bytes on the CPU. Preserve the rendered RGB before that partial write.
    workarounds.preserve_rgba8_cpu_writes = true;
    workarounds.preserve_wqm_helpers = wqm_diagnostic;
    workarounds.fixed_grass_view_lighting = fixed_grass_view_lighting;
    workarounds.preserve_transition_snapshot = preserve_transition_snapshot;
    if (bc6_diagnostic == "unsigned") {
        workarounds.bc6_float = Bc6FloatDecode::Unsigned;
    } else if (bc6_diagnostic == "signed") {
        workarounds.bc6_float = Bc6FloatDecode::Signed;
    }
}

const GpuWorkarounds& Current() {
    return workarounds;
}
} // namespace Ps4RtGpuWorkarounds
