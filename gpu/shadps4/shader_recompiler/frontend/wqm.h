// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>
#include "shader_recompiler/frontend/instruction.h"

namespace Shader::Gcn {
// Lower the restricted fragment cutout idiom to speculative read-only shading
// followed by the original masked export. This is not general WQM emulation.
u32 RemoveWqmEmptyWaveBranches(std::span<GcnInst> instructions);
} // namespace Shader::Gcn
