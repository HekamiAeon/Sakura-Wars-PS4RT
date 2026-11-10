// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual ImGui child navigation with SDL-equivalent gamepad events.
// No graphics device or original game assets are needed.
#include "ps4rt_achievements_ui.h"
#include "ps4rt_trophy.h"
#include "ps4rt_menu_navigation.h"
#include "ps4rt_png.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

// Navigation fixtures have no icons. PNG decoding is independent of focus.
namespace Ps4RtPng { bool LoadRgba(const char*, Image&) { return false; } }

static ImGuiWindow* Child();
static bool enter_list = false;
static bool graphics_page = false;
static bool enter_graphics = false;

static void Frame() {
    Ps4RtAchievementsUi::Prepare(true);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(700, 360)); // most of the 25 entries are off-screen
    ImGui::Begin("Navigation", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    if (enter_list) {
        auto* child = Child();
        assert(child);
        ImGui::FocusWindow(child);
        ImGui::NavInitWindow(child, true);
        enter_list = false;
    }
    if (graphics_page) {
        if (ImGui::BeginChild("##graphics_body", ImVec2(0, -40))) {
            if (enter_graphics) {
                ImGui::FocusWindow(ImGui::GetCurrentWindow());
                ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
                enter_graphics = false;
            }
            static bool sharpen = false;
            ImGui::Checkbox("Sharpening", &sharpen);
        }
        ImGui::EndChild();
        ImGui::Button("Outside");
    } else {
        Ps4RtAchievementsUi::List(1.0f);
    }
    ImGui::End();
    ImGui::Render();
}
static void Press(ImGuiKey key) {
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(key, true); Frame();
    io.AddKeyEvent(key, false); Frame();
    Frame(); Frame(); // apply both ordinary and forwarded wrap requests
}
static ImGuiWindow* Child() {
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (std::string_view(window->Name).find("##achievement_list") != std::string_view::npos) return window;
    return nullptr;
}
static void EnterList() {
    enter_list = true;
    Frame(); Frame();
    assert(Ps4RtAchievementsUi::ListFocused());
}
int main() {
    namespace fs = std::filesystem;
    char tmp[] = "/tmp/ps4rt-achievement-nav-XXXXXX";
    const fs::path root = mkdtemp(tmp);
    const auto catalog = root / "catalog.tsv", state = root / "state";
    {
        std::ofstream out(catalog);
        out << "PS4RT-TROPHIES\t1\nS\t0\tNPWR00003_00\tNavigation\tSynthetic\n";
        for (int i = 0; i < 25; ++i)
            out << "T\t0\t" << i << "\tB\t-1\t1\t1\tAward " << i << "\tSynthetic description\n";
    }
    setenv("PS4RT_TROPHY_CATALOG", catalog.c_str(), 1);
    setenv("PS4RT_ACHIEVEMENTS_DIR", state.c_str(), 1);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(1);
    Frame(); Frame(); EnterList();
    const auto first = ImGui::GetCurrentContext()->NavId;
    assert(first);
    std::set<ImGuiID> visited{first};
    for (int i = 0; i < 24; ++i) {
        Press(ImGuiKey_GamepadDpadDown);
        assert(Ps4RtAchievementsUi::ListFocused());
        visited.insert(ImGui::GetCurrentContext()->NavId);
    }
    assert(visited.size() == 25); // reaches entries beyond the initial viewport
    const auto last = ImGui::GetCurrentContext()->NavId;
    assert(last != first && Child()->Scroll.y > 0);
    Press(ImGuiKey_GamepadDpadDown);
    assert(Ps4RtAchievementsUi::ListFocused() && ImGui::GetCurrentContext()->NavId == first);
    assert(Child()->Scroll.y < 1);
    Press(ImGuiKey_GamepadDpadUp);
    assert(Ps4RtAchievementsUi::ListFocused() && ImGui::GetCurrentContext()->NavId == last);
    Press(ImGuiKey_GamepadDpadDown);
    assert(ImGui::GetCurrentContext()->NavId == first);
    // Held buttons use ImGui's repeat path and must remain in the list, too.
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
    for (int i = 0; i < 160; ++i) { Frame(); assert(Ps4RtAchievementsUi::ListFocused()); }
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false); Frame(); Frame();

    // Capture focus before NewFrame, exactly as the overlay close handler does.
    assert(Ps4RtAchievementsUi::ListFocused());
    assert(Ps4RtMenuNavigation::ChildPanelFocused());
    auto* parent = Child()->ParentWindow;
    Press(ImGuiKey_GamepadFaceRight);
    assert(!Ps4RtAchievementsUi::ListFocused());
    assert(!Ps4RtMenuNavigation::ChildPanelFocused());
    assert(ImGui::GetCurrentContext()->NavWindow == parent);
    Press(ImGuiKey_GamepadDpadUp);
    assert(ImGui::GetCurrentContext()->NavId == parent->GetID("Show"));

    // Change the real filter combo with the controller, then wrap over only the
    // two matching entries, not hidden items excluded by the filter.
    int context, handle, platinum;
    assert(ps4rt_trophy_create_context(&context, 1, 0, 0) == 0);
    assert(ps4rt_trophy_create_handle(&handle) == 0);
    assert(ps4rt_trophy_register(context, handle, 0) == 0);
    assert(ps4rt_trophy_unlock(context, handle, 4, &platinum) == 0);
    assert(ps4rt_trophy_unlock(context, handle, 17, &platinum) == 0);
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadFaceDown);
    EnterList();
    const auto filtered_first = ImGui::GetCurrentContext()->NavId;
    Press(ImGuiKey_GamepadDpadDown);
    assert(ImGui::GetCurrentContext()->NavId != filtered_first);
    Press(ImGuiKey_GamepadDpadDown);
    assert(ImGui::GetCurrentContext()->NavId == filtered_first);
    Press(ImGuiKey_Escape);
    assert(!Ps4RtAchievementsUi::ListFocused());
    // The same back policy applies to the Graphics form: the first Circle
    // leaves its child, then the footer controls are reachable at parent level.
    graphics_page = true;
    Frame(); Frame();
    enter_graphics = true;
    Frame(); Frame();
    assert(Ps4RtMenuNavigation::ChildPanelFocused());
    assert(!Ps4RtAchievementsUi::ListFocused());
    Press(ImGuiKey_GamepadFaceRight);
    assert(!Ps4RtMenuNavigation::ChildPanelFocused());
    Press(ImGuiKey_GamepadDpadDown);
    assert(ImGui::GetCurrentContext()->NavId == ImGui::FindWindowByName("Navigation")->GetID("Outside"));
    ImGui::DestroyContext();
    fs::remove_all(root);
    std::puts("PASS: real ImGui gamepad navigation, off-screen entries, circular wrapping, held D-pad, cancel, filtered list and Graphics child back");
}
