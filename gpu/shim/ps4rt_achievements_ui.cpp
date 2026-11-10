// SPDX-License-Identifier: GPL-2.0-or-later
// Local achievements in the existing overlay. Trophy conditions remain in the game.
#include "ps4rt_achievements_ui.h"
#include "ps4rt_achievements.h"
#include "ps4rt_png.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>

namespace Ps4RtAchievementsUi {
namespace {
std::vector<Ps4RtAchievements::Achievement> rows;
std::optional<Ps4RtAchievements::Achievement> active;
std::map<std::string, ImTextureData*> icons;
std::atomic<int64_t> until{0};
int64_t refreshed = 0;
size_t icon_bytes = 0;
int filter = 0;
ImGuiID list_window_id = 0;

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
const char* Grade(char grade) {
    switch (grade) { case 'P': return "Platinum"; case 'G': return "Gold";
                    case 'S': return "Silver"; default: return "Bronze"; }
}
ImVec4 Color(char grade) {
    switch (grade) { case 'P': return {0.75f, 0.9f, 1.0f, 1.0f};
    case 'G': return {0.96f, 0.80f, 0.30f, 1.0f};
    case 'S': return {0.82f, 0.85f, 0.91f, 1.0f};
    default: return {0.91f, 0.63f, 0.42f, 1.0f}; }
}
void LoadIcon(const std::string& path) {
    if (path.empty() || icons.contains(path) || icon_bytes >= 64 * 1024 * 1024) return;
    icons[path] = nullptr;
    Ps4RtPng::Image image;
    if (!Ps4RtPng::LoadRgba(path.c_str(), image) || image.width <= 0 || image.height <= 0 ||
        image.width > 1024 || image.height > 1024 || icon_bytes + image.rgba.size() > 64 * 1024 * 1024) return;
    auto* texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, image.width, image.height);
    std::memcpy(texture->GetPixels(), image.rgba.data(), image.rgba.size());
    ImGui::RegisterUserTexture(texture);
    icons[path] = texture;
    icon_bytes += image.rgba.size();
}
bool Icon(const std::string& path, float size) {
    auto found = icons.find(path);
    if (found == icons.end() || !found->second) return false;
    ImGui::Image(found->second->GetTexRef(), ImVec2(size, size));
    return true;
}
std::string Date(uint64_t when) {
    const auto seconds = static_cast<time_t>(when);
    std::tm tm{};
    localtime_r(&seconds, &tm);
    char out[32]; std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M", &tm);
    return out;
}
} // namespace

bool ListFocused() {
    const auto* context = ImGui::GetCurrentContext();
    return context && context->NavWindow && context->NavWindow->ID == list_window_id;
}

bool Pending() { return until.load(std::memory_order_relaxed) > Now() || Ps4RtAchievements::Get().NotificationsPending(); }

void Prepare(bool list_open) {
    const auto now = Now();
    if (now >= until.load(std::memory_order_relaxed)) {
        active = Ps4RtAchievements::Get().PopNotification();
        until.store(active ? now + 5200 : 0, std::memory_order_relaxed);
        if (active) { LoadIcon(active->icon); refreshed = 0; }
    }
    if (list_open && (!refreshed || now - refreshed >= 1000)) {
        rows = Ps4RtAchievements::Get().Snapshot();
        refreshed = now;
    }
    if (list_open) {
        int added = 0;
        for (const auto& row : rows) {
            if ((!row.hidden || row.unlocked_at) && !icons.contains(row.icon)) {
                LoadIcon(row.icon);
                if (++added == 8) break; // avoid decoding a whole catalog on one frame
            }
        }
    }
}

void List(float scale) {
    if (rows.empty()) {
        const auto& problem = Ps4RtAchievements::Get().Problem();
        ImGui::TextWrapped("%s", problem.empty() ? "No achievements are available." : problem.c_str());
        return;
    }
    const auto earned = std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.unlocked_at != 0; });
    ImGui::Text("%zu / %zu unlocked", static_cast<size_t>(earned), rows.size());
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.48f, 0.76f, 0.94f, 1.0f));
    ImGui::ProgressBar(float(earned) / rows.size(), ImVec2(-1, 0));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(180 * scale);
    ImGui::Combo("Show", &filter, "All\0Unlocked\0Locked\0");
    ImGui::Separator();
    if (ImGui::BeginChild("##achievement_list", ImVec2(0, 0))) {
        list_window_id = ImGui::GetCurrentWindow()->ID;
        for (const auto& row : rows) {
            if ((filter == 1 && !row.unlocked_at) || (filter == 2 && row.unlocked_at)) continue;
            ImGui::PushID(row.set.c_str()); ImGui::PushID(row.id);
            const bool hidden = row.hidden && !row.unlocked_at;
            if (hidden || !Icon(row.icon, 56 * scale)) ImGui::Dummy(ImVec2(56 * scale, 56 * scale));
            ImGui::SameLine();
            ImGui::BeginGroup();
            const char* name = hidden ? "Hidden achievement" : row.name.c_str();
            ImGui::Selectable(name, false);
            ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - 8 * scale);
            ImGui::TextWrapped("%s", hidden ? "Unlock this achievement to reveal its details." : row.description.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextColored(Color(row.grade), "%s", Grade(row.grade));
            ImGui::SameLine();
            if (row.unlocked_at) ImGui::TextDisabled("Unlocked  %s", Date(row.unlocked_at).c_str());
            else ImGui::TextDisabled("Locked");
            ImGui::EndGroup();
            ImGui::Separator();
            ImGui::PopID(); ImGui::PopID();
        }
        // Keep directional navigation inside this child and wrap across the
        // entire filtered list, including entries outside the visible viewport.
        ImGui::NavMoveRequestTryWrapping(ImGui::GetCurrentWindow(), ImGuiNavMoveFlags_LoopY);
    }
    ImGui::EndChild();
}

void Notification(float scale) {
    if (!active || Now() >= until.load(std::memory_order_relaxed)) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 18 * scale,
                                  viewport->WorkPos.y + 18 * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(420 * scale, 0));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::Begin("##achievement_notification", nullptr, ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing);
    if (Icon(active->icon, 64 * scale)) ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(Color(active->grade), "Achievement unlocked  /  %s", Grade(active->grade));
    ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - 8 * scale);
    ImGui::TextWrapped("%s", active->name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::End();
}
} // namespace Ps4RtAchievementsUi
