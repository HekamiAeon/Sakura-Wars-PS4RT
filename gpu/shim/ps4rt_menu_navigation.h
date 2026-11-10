// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
#include "imgui_internal.h"

namespace Ps4RtMenuNavigation {
// Read before NewFrame: ImGui handles Cancel during NewFrame and may have
// already returned focus to the parent by the time the menu is drawn.
inline bool ChildPanelFocused() {
    const auto* context = ImGui::GetCurrentContext();
    return context && context->NavWindow &&
           (context->NavWindow->Flags & ImGuiWindowFlags_ChildWindow) != 0;
}
} // namespace Ps4RtMenuNavigation
