// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

namespace Ps4RtGpuWorkarounds {

enum class Bc6FloatDecode { Reject, Unsigned, Signed };

struct GpuWorkarounds {
    Bc6FloatDecode bc6_float = Bc6FloatDecode::Reject;
    bool preserve_rgba8_cpu_writes = false;
    bool preserve_wqm_helpers = false;
    bool fixed_grass_view_lighting = false;
    bool preserve_transition_snapshot = false;
};

// Initialize inherited native-port workarounds before GPU threads start.
// Future ports can change or disable these settings as evidence requires.
void Configure(std::string_view bc6_diagnostic,
               bool wqm_diagnostic = false, bool fixed_grass_view_lighting = false,
               bool preserve_transition_snapshot = false);
const GpuWorkarounds& Current();

} // namespace Ps4RtGpuWorkarounds
