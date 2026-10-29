// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include "ps4rt_settings.h"
#include "ps4rt_menu_input.h"
#include "ps4rt_menu_sounds.h"
#include "ps4rt_graphics.h"
#include "ps4rt_perf_stats.h"

extern "C" void runtime_restart(void); // src/loader.c: starts this launch again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"


// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global ps4rt_font_ttf\n"
    "ps4rt_font_ttf:\n"
    ".incbin \"" PS4RT_FONT_PATH "\"\n"
    ".global ps4rt_font_ttf_end\n"
    "ps4rt_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden ps4rt_font_ttf\n"
    ".global ps4rt_font_ttf\n"
    "ps4rt_font_ttf:\n"
    ".incbin \"" PS4RT_FONT_PATH "\"\n"
    ".hidden ps4rt_font_ttf_end\n"
    ".global ps4rt_font_ttf_end\n"
    "ps4rt_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char ps4rt_font_ttf[];
extern "C" const unsigned char ps4rt_font_ttf_end[];

namespace Ps4RtOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
// A short message in a corner (saved screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
bool dirty = false; // settings changed while open: saved on close
bool focus_graphics = false;
Ps4RtMenuInput::Controllers menu_controllers;
Ps4RtMenuSounds::Player menu_sounds;
std::unordered_map<SDL_JoystickID, std::array<int, 2>> menu_stick_directions;
bool cancel_was_editing = false;
// Circle/B can cancel a widget and close the menu in the same frame.
// Suppress the second close cue only when a cancel cue already played.
bool cancel_sound_just_played = false;
// The installer runs out-of-process. Existing add-on mounts are immutable until restart.
std::atomic<int> dlc_installer_state{0}; // 0 idle, 1 running, 2 unchanged, 3 added, -1 failed

void OpenDlcInstaller() {
#ifndef _WIN32
    const char* script = std::getenv("PS4RT_DLC_INSTALL_SCRIPT");
    const char* game = std::getenv("PS4RT_ORIGINAL_GAME_DIR");
    if (!script || !*script || !game || !*game) {
        dlc_installer_state = -1;
        return;
    }
    if (dlc_installer_state == 1) return;
    dlc_installer_state = 1;
    char* const args[] = {
        const_cast<char*>("python3"), const_cast<char*>(script),
        const_cast<char*>("--game"), const_cast<char*>(game),
        const_cast<char*>("--install-dlc"), nullptr
    };
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, "python3", nullptr, nullptr, args, environ);
    if (rc != 0) {
        std::fprintf(stderr, "DLC: cannot start installer (errno %d)\n", rc);
        dlc_installer_state = -1;
        return;
    }
    std::thread([pid] {
        int status = 0;
        const pid_t result = waitpid(pid, &status, 0);
        // Internal installer exit 10 means new DLC was added; 0 is skip/no change.
        dlc_installer_state = result != pid || !WIFEXITED(status) ? -1 :
                              WEXITSTATUS(status) == 10 ? 3 :
                              WEXITSTATUS(status) == 0 ? 2 : -1;
    }).detach();
#else
    dlc_installer_state = -1;
#endif
}

void ShowDlcInstallerStatus() {
    const int state = dlc_installer_state.load();
    if (state == 1) {
        ImGui::TextDisabled("DLC installer open. Finish selecting packages in the file dialog.");
    } else if (state == 2) {
        ImGui::TextDisabled("No new DLC installed. Continue playing normally.");
    } else if (state == 3) {
        ImGui::TextWrapped("New DLC installed. Save your game before restarting to activate it.");
        if (ImGui::Button("Restart to load DLC..."))
            ImGui::OpenPopup("Restart for DLC?");
        if (ImGui::BeginPopupModal("Restart for DLC?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Restart now to load your DLC? Unsaved game progress will be lost.");
            ImGui::Spacing();
            if (ImGui::Button("Cancel") ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Restart now")) {
                Ps4RtSettings::Save();
                dirty = false;
                runtime_restart();
            }
            ImGui::EndPopup();
        }
    } else if (state == -1) {
        ImGui::TextWrapped("DLC setup unavailable or failed. Check the launch terminal for details.");
    }
}
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<Ps4RtPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
// Sakura Wars art is decoded and uploaded once using the save-picture Vulkan texture path.
ImTextureData* menu_art_texture = nullptr;
ImVec2 menu_art_size{};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    // Closed-menu releases are not routed to ImGui. Discard held/queued input
    // across transitions so reopening cannot repeat an old direction or confirm.
    ImGui::GetIO().ClearEventsQueue();
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
    ImGui::ClosePopupsOverWindow(nullptr, false);
    if (value) {
        menu_sounds.Play(Ps4RtMenuSounds::Cue::Open);
    } else if (!cancel_sound_just_played) {
        menu_sounds.Play(Ps4RtMenuSounds::Cue::Close);
    }
    cancel_sound_just_played = false;
    if (value) {
        requested_tab = Ps4RtSettings::Get().menu_tab;
        focus_graphics = true;
    } else {
        menu_controllers.Release();
        menu_stick_directions.clear();
    }
    if (!value && dirty) {
        dirty = false;
        Ps4RtSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text follow PS4RT_GLYPHS (run.py sets xbox or nintendo when
// the glyph feature is on); unset is PlayStation. Read once: the environment is fixed for a run.
static const char* PadText(const char* playstation, const char* xbox, const char* nintendo) {
    static const char* style = std::getenv("PS4RT_GLYPHS");
    if (!style) return playstation;
    return !std::strcmp(style, "xbox") ? xbox : !std::strcmp(style, "nintendo") ? nintendo : playstation;
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case Ps4RtSettings::UpscalerFsr3: return "FSR 3.1";
    case Ps4RtSettings::UpscalerFsr4: return "FSR 4";
    case Ps4RtSettings::UpscalerFsr411: return "FSR 4.1.1";
    case Ps4RtSettings::UpscalerTaa: return "TAA";
    case Ps4RtSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = Ps4RtPerfStats;
    const auto& s = Ps4RtSettings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

void GameTab() {
    auto& s = Ps4RtSettings::Get();
    ImGui::SeparatorText("Controller");
    if (BeginRows("##controller")) {
        SliderRow("Vibration", s.vibration, 0, 100, "%d%%",
                  "Strength of controller vibration. 0% turns it off.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Additional content");
    ImGui::BeginDisabled(dlc_installer_state == 1);
    if (ImGui::Button("Install DLC..."))
        OpenDlcInstaller();
    ImGui::EndDisabled();
    ShowDlcInstallerStatus();
}

// The game's own frame rate, when the game has a frame rate hook (ps4rt_framerate.h): the rates
// up to its maximum. Live.
void FramerateRow() {
    namespace G = Ps4RtGraphics;
    auto& s = Ps4RtSettings::Get();
    if (s.framerate_max <= G::Framerates[0]) return;
    static const char* const framerate_labels[] = {"30 (original)", "60", "90", "120"};
    int count = 0;
    while (count < int(G::Framerates.size()) && G::Framerates[count] <= s.framerate_max) ++count;
    ChoiceRow("Framerate", s.framerate, framerate_labels, G::Framerates.data(), count,
              "The game's own frame rate; game speed stays normal. Applies live. If an area "
              "misbehaves at a higher rate, switch back to 30. When the PC cannot hold the "
              "chosen rate, the game runs slower instead of skipping frames.");
}

// The game's post effects turned down, when the game has the hook (ps4rt_effects.h). Live.
void GraphicsPresetRow() {
    auto& s = Ps4RtSettings::Get();
    if (!s.graphics_preset_available) return;
    static const char* const labels[] = {"Max (original)", "High", "Medium", "Lowest"};
    ChoiceRow("Graphics preset", s.graphics_preset, labels, nullptr, 4,
              "For slower GPUs such as the Steam Deck. High turns off contact shadows and bloom; "
              "Medium also uses FXAA instead of SMAA, a lower-resolution depth of field and "
              "half the dash motion blur samples; "
              "Lowest also turns off anti-aliasing, the outlines and depth of field (about a "
              "third faster on weak GPUs; some battle effects such as sword trails go with "
              "depth of field). Applies live.");
}

void CompactGraphics() {
    auto& s = Ps4RtSettings::Get();
    if (BeginRows("##scene_image")) {
        static const char* const outputs[] = {"1920 x 1080", "2560 x 1440", "3840 x 2160 (4K)"};
        static constexpr int output_values[] = {1, 2, 3};
        Row("Output resolution");
        if (focus_graphics) {
            ImGui::SetKeyboardFocusHere();
            focus_graphics = false;
        }
        Choice("##value",s.output_res,outputs,output_values,3);
        Help("Fixed 16:9 output fitted to the window. The game still renders internally at 1080p.");
        ImGui::PopID();
        ChoiceRow("Display mode", s.display_mode, Ps4RtGraphics::DisplayModeNames.data(),
                  nullptr, Ps4RtGraphics::DisplayModeCount,
                  "Windowed, borderless window or fullscreen. Applies immediately.");
        static const char* const providers[] = {"Off / Original", "FSR 3.1", "FSR 4 v07"};
        static constexpr int provider_values[] = {Ps4RtSettings::UpscalerOff,
            Ps4RtSettings::UpscalerFsr3, Ps4RtSettings::UpscalerFsr4};
        // Commit the provider together with its required motion/jitter settings.
        std::atomic<int> provider{s.upscaler.load()};
        ChoiceRow("Upscaling / anti-aliasing", provider, providers, provider_values,
                  s.fsr4_supported ? 3 : 2,
                  "FSR reconstructs the 1080p scene before the HUD. Off uses the original image with ordinary display scaling.");
        if (provider != s.upscaler)
            Ps4RtSettings::SetSceneUpscaler(provider, false);
        const bool temporal = s.upscaler != Ps4RtSettings::UpscalerOff;
        const int output = s.output_res;
        InfoRow("Quality preset", !temporal || s.temporal_problem ? "Original (1080p input)" :
            output == 1 ? "Native AA (1080p input)" :
            output == 3 ? "Performance (1080p input)" : "Custom 1.33x (1080p input)");
        static const char* const anisotropy_labels[] = {"Game default", "2x", "4x", "8x", "16x"};
        ChoiceRow("Anisotropic filtering", s.anisotropy, anisotropy_labels,
                  Ps4RtGraphics::Anisotropies.data(), int(Ps4RtGraphics::Anisotropies.size()),
                  "Sharpens textures viewed at an angle. Works with FSR. Applies after restarting the game.");
        FramerateRow();
        static const char* const outline_labels[] = {"Native", "Off"};
        static constexpr int outline_values[] = {0, 2};
        ChoiceRow("Outlines", s.outlines, outline_labels, outline_values, 2,
                  "The game's ink outlines on walls and surfaces. Off removes them, including the lines on surface creases (walls, mouldings) that can break up and flicker; characters keep their own outlines. Applies live.");
        GraphicsPresetRow();
        Row("Sharpening");
        int strength = s.sharpen ? int(s.sharpness * 100.0f + 0.5f) : 0;
        if (ImGui::SliderInt("##value", &strength, 0, 100, "%d%%")) {
            s.sharpness = float(strength) / 100.0f;
            s.sharpen = strength != 0;
            s.rcas = !temporal && strength != 0;
            s.rcas_strength = strength;
            dirty = true;
        }
        Help("Optional sharpening. 0% disables it; larger values can add halos around edges.");
        ImGui::PopID();
        ImGui::EndTable();
    }
    {
        ImGui::TextDisabled("Display mode, resolution, FSR and sharpening apply live. Choices save on close.");
        if (s.anisotropy != s.startup_anisotropy)
            ImGui::TextWrapped("Restart the game to apply the selected anisotropic filtering.");
        else
            ImGui::TextDisabled("Anisotropic filtering changes require a game restart.");
        ImGui::TextDisabled("Render scale follows the fixed 1080p game input.");
        if (s.upscaler != Ps4RtSettings::UpscalerOff)
            if (const char* problem = s.temporal_problem.load()) ImGui::TextWrapped("%s", problem);
        if (!s.fsr4_supported) ImGui::TextWrapped("FSR 4 v07 requires GPU features unavailable on this device. FSR 3.1 remains available.");
        if (const char* problem = s.fsr4_problem.load()) ImGui::TextWrapped("%s", problem);
    }
}

void ImageTab() {
    namespace G = Ps4RtGraphics;
    auto& s = Ps4RtSettings::Get();
    ImGui::SeparatorText("Display");
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    ImGui::SeparatorText("Sharpening");
    if (BeginRows("##scaling")) {
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "No effect: the game's image is smaller than the window."
                                             : "Sharpens the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Textures (applied on restart)");
    if (BeginRows("##textures")) {
        static const char* const anisotropy_labels[] = {"Game default", "2x", "4x", "8x", "16x"};
        ChoiceRow("Anisotropic filtering", s.anisotropy, anisotropy_labels, G::Anisotropies.data(),
                  int(G::Anisotropies.size()),
                  "Sharper floors and walls seen at an angle. Little cost on most GPUs.");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("F12 saves a screenshot to the user\\screenshots folder.");
    ImGui::SeparatorText("Information");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Game image", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window", text);
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = Ps4RtGraphics;
    auto& s = Ps4RtSettings::Get();
    ImGui::SeparatorText("Live");
    if (BeginRows("##live")) {
        FramerateRow();
        GraphicsPresetRow();
        static const char* const fps_labels[] = {"30", "60", "120", "240", "Unlimited"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. The engine frame rate (below) is the highest it can go.");
        const int effective = G::EffectiveFps(s.fps_limit, s.applied_engine_fps);
        if (s.fps_limit == 0 || effective < s.fps_limit) {
            char text[48];
            std::snprintf(text, sizeof(text), "%d FPS (engine frame rate)", effective);
            InfoRow("Effective limit", text);
        }
        if (s.applied_engine_fps < s.startup_engine_fps) {
            InfoRow("Engine frame rate", "60 FPS: the frame rate patch is not available for this game build");
        }
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Applied on restart");
    if (BeginRows("##restart")) {
        static const char* const resolution_labels[] = {"Native (1080p)", "480p", "720p", "1440p", "1800p", "4K"};
        ChoiceRow("Render resolution", s.render_resolution, resolution_labels, nullptr, int(G::Resolutions.size()),
                  "The resolution the game draws at. Higher is sharper and slower.");
        static const char* const engine_labels[] = {"60 (original)", "120", "240"};
        ChoiceRow("Engine frame rate", s.engine_fps, engine_labels, G::EngineFps.data(), int(G::EngineFps.size()),
                  "The highest frame rate the game itself runs at.");
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = Ps4RtPerfStats;
    auto& s = Ps4RtSettings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText("What to show");
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (game / total)", "VRAM (used / available)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(names[item], &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Preview");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? "(nothing selected)" : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Appearance");
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.");
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.");
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("About");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), "loading %u%%", u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "ready");
        }
        InfoRow("Shader cache", text);
        ImGui::EndTable();
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = Ps4RtSettings::Get();
    return (s.render_resolution != s.startup_render_resolution) + (s.engine_fps != s.startup_engine_fps) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback) +
           (s.anisotropy != s.startup_anisotropy);
}

// The portrait stays within the menu window, behind all settings widgets.
// Preserve the supplied artwork's aspect ratio while center-cropping to the panel.
void DrawMenuArtInWindow() {
    if (!menu_art_texture || menu_art_size.x <= 0.0f || menu_art_size.y <= 0.0f) return;
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2 area_min = ImGui::GetCursorScreenPos();
    const float margin = 12.0f * base_scale;
    const ImVec2 area_max(window_pos.x + window_size.x - margin,
                          window_pos.y + window_size.y - margin);
    const float width = area_max.x - area_min.x, height = area_max.y - area_min.y;
    if (width <= 0.0f || height <= 0.0f) return;
    ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
    const float image_ratio = menu_art_size.x / menu_art_size.y;
    const float panel_ratio = width / height;
    if (image_ratio > panel_ratio) {
        const float crop = (1.0f - panel_ratio / image_ratio) * 0.5f;
        uv0.x = crop;
        uv1.x = 1.0f - crop;
    } else {
        const float crop = (1.0f - image_ratio / panel_ratio) * 0.5f;
        uv0.y = crop;
        uv1.y = 1.0f - crop;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(area_min, area_max, true);
    draw->AddImage(menu_art_texture->GetTexRef(), area_min, area_max,
                   uv0, uv1, IM_COL32(255, 255, 255, 225));
    // Wine-plum tint protects text contrast while retaining the portrait's pink highlights.
    draw->AddRectFilled(area_min, area_max, IM_COL32(30, 8, 29, 153));
    draw->AddRectFilledMultiColor(area_min, area_max,
                                 IM_COL32(28, 6, 27, 114), IM_COL32(45, 9, 35, 24),
                                 IM_COL32(24, 5, 23, 102), IM_COL32(24, 6, 28, 124));
    draw->PopClipRect();
}

void Menu() {
    auto& s = Ps4RtSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Match Star Ocean's overlay dimensions, including its compact mode.
    const float target_width = (s.CompactGraphicsMenu() ? 1080.0f : 1120.0f) * base_scale;
    const float target_height = (s.CompactGraphicsMenu() ? 650.0f : 760.0f) * base_scale;
    ImGui::SetNextWindowSize(ImVec2(std::min(target_width, viewport->WorkSize.x * 0.88f),
                                   std::min(target_height, viewport->WorkSize.y * 0.82f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // ps4rt: the game's own title (param.sfo), not a fixed one.
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return std::string(title.empty() ? "PS4RT" : title) + PadText("  (Insert / R3+L2)", "  (Insert / RS+LT)", "  (Insert / RS+ZL)");
    }();
    if (!ImGui::Begin(heading.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse |
            (s.CompactGraphicsMenu() ? ImGuiWindowFlags_NoResize : 0))) {
        ImGui::End();
        return;
    }
    DrawMenuArtInWindow();
    footer_help = nullptr;
    if (s.CompactGraphicsMenu()) {
        CompactGraphics();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", footer_help ? footer_help :
            PadText("D-pad: navigate. Cross: select. Circle: back.",
                    "D-pad: navigate. A: select. B: back.",
                    "D-pad: navigate. B: select. A: back."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Close")) keep_open = false;
        ImGui::SameLine();
        if (ImGui::Button("Exit Game"))
            ImGui::OpenPopup("Exit game?");
        ImGui::SameLine();
        if (ImGui::Button("Restart Game"))
            ImGui::OpenPopup("Restart game?");
        ImGui::SameLine();
        ImGui::BeginDisabled(dlc_installer_state == 1);
        if (ImGui::Button("Install DLC..."))
            OpenDlcInstaller();
        ImGui::EndDisabled();
        ShowDlcInstallerStatus();
        // Starts the game again from boot (same launch), for scenes that are hard to leave.
        if (ImGui::BeginPopupModal("Restart game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Restart the game from the start? Unsaved game progress will be lost.");
            ImGui::Spacing();
            if (ImGui::Button("Cancel") ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Restart")) {
                // Graphics settings only; this is not an in-game save.
                Ps4RtSettings::Save();
                dirty = false;
                runtime_restart();
            }
            ImGui::EndPopup();
        }
        // Confirm explicitly: closing the host process does not save in-game progress.
        if (ImGui::BeginPopupModal("Exit game?", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Exit to desktop? Unsaved game progress will be lost.");
            ImGui::Spacing();
            if (ImGui::Button("Cancel") ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Exit to desktop")) {
                // Use the existing window-close path, not an abrupt renderer-thread exit.
                // Save runtime graphics settings first; this is not an in-game save.
                Ps4RtSettings::Save();
                dirty = false;
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                if (SDL_PushEvent(&quit)) {
                    ImGui::CloseCurrentPopup();
                } else {
                    std::fprintf(stderr, "PS4RT: could not queue exit event: %s\n", SDL_GetError());
                }
            }
            ImGui::EndPopup();
        }
        // Circle first cancels a popup or slider; another press closes the panel.
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) &&
            !cancel_was_editing && !ImGui::IsPopupOpen("Exit game?") &&
            !ImGui::IsPopupOpen("Restart game?") &&
            !ImGui::IsPopupOpen("Restart for DLC?"))
            keep_open = false;
        ImGui::End();
        if (!keep_open) SetOpen(false);
        return;
    }
    constexpr int TabCount = 4;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Game", "Image", "Performance", "Overlay"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                switch (tab) {
                case 0: GameTab(); break;
                case 1: ImageTab(); break;
                case 2: PerformanceTab(); break;
                default: OverlayTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", footer_help ? footer_help : PadText("L1 / R1 or Q / E: switch tabs. Settings are saved in ps4rt.ini.", "LB / RB or Q / E: switch tabs. Settings are saved in ps4rt.ini.", "L / R or Q / E: switch tabs. Settings are saved in ps4rt.ini."));
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("PS4RT_RESTART_COMMAND"));
        if (ImGui::Button("Apply and restart")) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            Ps4RtSettings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    const float close_width = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("%s", PadText("Controller: Cross (A) = OK, Circle (B) = cancel", "Controller: A = OK, B = cancel", "Controller: B = OK, A = cancel"));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", PadText("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel", "Up/Down or D-pad: choose   Enter or A: OK   Esc or B: cancel", "Up/Down or D-pad: choose   Enter or B: OK   Esc or A: cancel"));
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = Ps4RtSettings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

void Notice() {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

} // namespace

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ps4rt";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 7.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ScrollbarRounding = 7.0f;
    style.WindowBorderSize = 1.2f;
    style.FrameBorderSize = 0.7f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.045f, 0.115f, 0.82f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.032f, 0.085f, 0.42f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.055f, 0.13f, 0.95f);
    colors[ImGuiCol_Border] = ImVec4(1.0f, 0.96f, 0.98f, 0.88f);
    colors[ImGuiCol_Text] = ImVec4(1.0f, 0.97f, 0.99f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.88f, 0.76f, 0.84f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.22f, 0.06f, 0.17f, 0.95f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.40f, 0.08f, 0.24f, 0.98f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.28f, 0.09f, 0.23f, 0.90f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.52f, 0.18f, 0.36f, 0.96f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.70f, 0.23f, 0.47f, 0.98f);
    colors[ImGuiCol_Button] = ImVec4(0.39f, 0.11f, 0.27f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.69f, 0.22f, 0.46f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.83f, 0.31f, 0.54f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.50f, 0.15f, 0.33f, 0.88f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.75f, 0.27f, 0.50f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.86f, 0.36f, 0.59f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.79f, 0.91f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.96f, 0.64f, 0.78f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.84f, 0.93f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.95f, 0.84f, 0.92f, 0.56f);
    colors[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.89f, 0.96f, 1.0f);

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(ps4rt_font_ttf),
                                   int(ps4rt_font_ttf_end - ps4rt_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    // Optional single menu-art texture, using the same Vulkan backend as save thumbnails.
    if (const char* path = std::getenv("PS4RT_OVERLAY_BACKGROUND"); path && *path) {
        Ps4RtPng::Image art;
        if (Ps4RtPng::LoadRgba(path, art) && !art.rgba.empty() &&
            art.width > 0 && art.height > 0 && art.width <= 4096 && art.height <= 4096) {
            menu_art_texture = IM_NEW(ImTextureData)();
            menu_art_texture->Create(ImTextureFormat_RGBA32, art.width, art.height);
            std::memcpy(menu_art_texture->GetPixels(), art.rgba.data(), art.rgba.size());
            ImGui::RegisterUserTexture(menu_art_texture);
            menu_art_size = ImVec2(float(art.width), float(art.height));
            std::printf("Overlay: loaded Sakura menu art %dx%d\n", art.width, art.height);
        } else {
            std::fprintf(stderr, "Overlay: Sakura menu art unavailable or invalid: %s\n", path);
        }
    }
    menu_sounds.Init(); // SDL3 audio failure does not prevent gameplay or opening the menu.
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const Ps4RtPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (is_open && event.key.key == SDLK_ESCAPE &&
            (ImGui::IsPopupOpen("Exit game?") || ImGui::IsPopupOpen("Restart game?") ||
             ImGui::IsPopupOpen("Restart for DLC?"))) {
            // Let Escape cancel the exit or restart confirmation instead of closing the panel.
            if (down && !event.key.repeat) menu_sounds.Play(Ps4RtMenuSounds::Cue::Cancel);
            io.AddKeyEvent(ImGuiKey_Escape, down);
            return true;
        }
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        if (down && !event.key.repeat) {
            using Cue = Ps4RtMenuSounds::Cue;
            switch (event.key.key) {
            case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT:
            case SDLK_TAB: case SDLK_Q: case SDLK_E:
                menu_sounds.Play(Cue::Navigate); break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                menu_sounds.Play(Cue::Confirm); break;
            case SDLK_BACKSPACE:
                menu_sounds.Play(Cue::Cancel); break;
            default: break;
            }
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // R3+L2 opens the menu; L3+R3 may be reserved for in-game actions.
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            if (menu_controllers.RightStick(event.gbutton.which, down)) {
                if (is_open && !menu_controllers.Accept(event.gbutton.which)) return true;
                SetOpen(!is_open);
                if (!is_open) menu_controllers.Focus(event.gbutton.which);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (!menu_controllers.Accept(event.gbutton.which, down)) return true;
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        if (down) {
            using Cue = Ps4RtMenuSounds::Cue;
            switch (button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                menu_sounds.Play(Cue::Navigate); break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
                menu_sounds.Play(Cue::Confirm); break;
            case SDL_GAMEPAD_BUTTON_EAST:
                menu_sounds.Play(Cue::Cancel);
                cancel_sound_just_played = true;
                break;
            default: break;
            }
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            if (menu_controllers.Trigger(event.gaxis.which, event.gaxis.value)) {
                if (is_open && !menu_controllers.Accept(event.gaxis.which)) return true;
                SetOpen(!is_open);
                if (!is_open) menu_controllers.Focus(event.gaxis.which);
                return true;
            }
        }
        if (!is_open) return false;
        if ((event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ||
             event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) &&
            menu_controllers.Accept(event.gaxis.which, std::abs(int(event.gaxis.value)) > 9000)) {
            const bool horizontal = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            // Play a tick only when the analog stick crosses into a direction.
            int& previous = menu_stick_directions[event.gaxis.which][horizontal ? 0 : 1];
            const int value = int(event.gaxis.value);
            const int direction = value > 19000 ? 1 : value < -19000 ? -1 :
                                  std::abs(value) < 9000 ? 0 : previous;
            if (direction != 0 && direction != previous)
                menu_sounds.Play(Ps4RtMenuSounds::Cue::Navigate);
            previous = direction;
            const float negative = Ps4RtMenuInput::Axis(event.gaxis.value, false);
            const float positive = Ps4RtMenuInput::Axis(event.gaxis.value, true);
            io.AddKeyAnalogEvent(horizontal ? ImGuiKey_GamepadLStickLeft : ImGuiKey_GamepadLStickUp,
                                 negative > 0.0f, negative);
            io.AddKeyAnalogEvent(horizontal ? ImGuiKey_GamepadLStickRight : ImGuiKey_GamepadLStickDown,
                                 positive > 0.0f, positive);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        menu_controllers.Remove(event.gdevice.which);
        menu_stick_directions.erase(event.gdevice.which);
        io.ClearInputKeys();
        return is_open;
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        if (event.wheel.x != 0.0f || event.wheel.y != 0.0f)
            menu_sounds.Play(Ps4RtMenuSounds::Cue::Navigate);
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = Ps4RtSettings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    Ps4RtSettings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

bool Visible() {
    const auto& s = Ps4RtSettings::Get();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           choice_retiring || s.show_fps || NoticeActive());
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<Ps4RtPng::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    cancel_was_editing = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetActiveID();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
        // Cancel without closing (e.g. a dropdown) must not silence a later close.
        cancel_sound_just_played = false;
        // ImGui synthesizes held-key repeats, including after SDL's first button-down.
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true) ||
            ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ||
            ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
            menu_sounds.Play(Ps4RtMenuSounds::Cue::Navigate);
    }
    if (Ps4RtSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (NoticeActive()) {
        Notice();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace Ps4RtOverlay
