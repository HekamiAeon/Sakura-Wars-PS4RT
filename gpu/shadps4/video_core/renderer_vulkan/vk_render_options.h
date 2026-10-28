// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Vulkan::RenderOptions {

struct Flags { bool shading{}, msdepth{}, dref{}, bias{}; bool Any() const { return false; } };
inline const Flags& Current() { static const Flags flags{}; return flags; }

/// Bits for the knobs that change recompiled SPIR-V: `dref` here and `shaderext` of
/// vk_amd_paths.h.
inline std::uint32_t ShaderBinarySalt(bool dref, bool shaderext) {
    return (dref ? 1u : 0u) | (shaderext ? 2u : 0u);
}

/// The shader binary cache version for a salt: distinct from every plain version bump.
inline std::uint32_t ShaderBinaryVersion(std::uint32_t base, std::uint32_t salt) {
    return base + 16u * salt;
}

}
