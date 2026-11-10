// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Ps4RtAchievementsUi {
bool Pending();
// Read before NewFrame: NavCancel may already leave this child during NewFrame.
bool ListFocused();
// Present thread, before ImGui::NewFrame; retains icons for the overlay lifetime.
void Prepare(bool list_open);
void List(float scale);
void Notification(float scale);
}
