// SPDX-License-Identifier: GPL-2.0-or-later
// Sakura Wars portable setup: SDL3, Dear ImGui, desktop chooser, and progress.
// No Tkinter, Zenity, KDialog or other system GUI toolkit is required.
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "ps4rt_png.h"

namespace fs = std::filesystem;

static bool regular_pkg(const fs::path& path) {
    std::error_code ec;
    if (fs::is_symlink(path, ec) || !fs::is_regular_file(path, ec)) return false;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".pkg";
}

static fs::path initial_directory() {
    const char* env = std::getenv("PS4RT_SETUP_START_DIR");
    const char* home = std::getenv("HOME");
    fs::path path = env && *env ? env : home && *home ? home : "/";
    std::error_code ec;
    return fs::is_directory(path, ec) ? fs::absolute(path, ec) : fs::path("/");
}

// Load the project's existing scalable font at a slightly larger size.
// ImGui's built-in font is 13px and becomes pixelated when enlarged.
static void configure_setup_font() {
    const char* base = SDL_GetBasePath();
    const fs::path binary_dir = base && *base ? fs::path(base) : fs::current_path();
    const fs::path paths[] = {
        binary_dir / "../gpu/third_party/fonts/DejaVuSans.ttf",
        binary_dir / "gpu/third_party/fonts/DejaVuSans.ttf",
        binary_dir / "assets/fonts/DejaVuSans.ttf",
        binary_dir / "fonts/DejaVuSans.ttf",
    };
    std::error_code ec;
    for (const auto& path : paths) {
        if (!fs::is_regular_file(path, ec)) continue;
        if (ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f)) {
            std::fprintf(stderr, "Sakura Wars setup font: %s\n", path.string().c_str());
            return;
        }
    }
    // Nonfatal fallback for an incomplete portable archive.
    ImGui::GetStyle().FontScaleMain = 1.16f;
}

// Use the same Sakura Wars image and FFmpeg PNG loader as the in-game overlay.
// Missing image is nonfatal: the plain plum background is a safe fallback.
static SDL_Texture* load_setup_art(SDL_Renderer* renderer) {
    const char* custom = std::getenv("PS4RT_OVERLAY_BACKGROUND");
    const char* base = SDL_GetBasePath();
    const fs::path binary_dir = base && *base ? fs::path(base) : fs::current_path();
    std::vector<fs::path> candidates;
    if (custom && *custom) candidates.emplace_back(custom);
    candidates.emplace_back(binary_dir / "../gpu/assets/sakura_wars_overlay.png");
    candidates.emplace_back(binary_dir / "gpu/assets/sakura_wars_overlay.png");
    candidates.emplace_back(binary_dir / "assets/sakura_wars_overlay.png");
    candidates.emplace_back(binary_dir / "../assets/sakura_wars_overlay.png");

    std::error_code ec;
    for (const auto& path : candidates) {
        if (!fs::is_regular_file(path, ec)) continue;
        Ps4RtPng::Image image;
        if (!Ps4RtPng::LoadRgba(path.string().c_str(), image)) continue;
        SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                SDL_TEXTUREACCESS_STATIC,
                                                image.width, image.height);
        if (!texture) continue;
        if (!SDL_UpdateTexture(texture, nullptr, image.rgba.data(),
                               image.width * 4)) {
            SDL_DestroyTexture(texture);
            continue;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        std::fprintf(stderr, "Sakura Wars setup art: %s\n", path.string().c_str());
        return texture;
    }
    std::fprintf(stderr, "Sakura Wars setup: PNG background not found; using plum fallback.\n");
    return nullptr;
}

static void draw_setup_background(SDL_Renderer* renderer, SDL_Texture* art) {
    SDL_SetRenderDrawColor(renderer, 33, 15, 31, 255);
    SDL_RenderClear(renderer);
    int width = 0, height = 0;
    SDL_GetRenderOutputSize(renderer, &width, &height);
    if (width <= 0 || height <= 0) return;
    if (art) {
        float image_w = 0, image_h = 0;
        if (SDL_GetTextureSize(art, &image_w, &image_h) && image_w > 0 && image_h > 0) {
            // Center-crop to fill the setup window without stretching the artwork.
            const float scale = std::max(float(width) / image_w, float(height) / image_h);
            const float visible_w = float(width) / scale;
            const float visible_h = float(height) / scale;
            SDL_FRect source{(image_w - visible_w) * .5f,
                             (image_h - visible_h) * .5f, visible_w, visible_h};
            SDL_FRect dest{0, 0, float(width), float(height)};
            SDL_RenderTexture(renderer, art, &source, &dest);
        }
    }
    // Keep the illustration visible while protecting text contrast.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 27, 9, 27, 135);
    SDL_RenderFillRect(renderer, nullptr);
    // The readability overlay must cover the entire renderer surface.
    // A former 78%-width rectangle created a visible vertical seam.
    const SDL_FRect text_panel{0, 0, float(width), float(height)};
    SDL_SetRenderDrawColor(renderer, 24, 7, 24, 125);
    SDL_RenderFillRect(renderer, &text_panel);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// SDL can invoke the native dialog callback on a different thread.
struct NativePicker {
    std::mutex mutex;
    bool waiting = false;    // main thread only
    bool completed = false;  // protected by mutex
    bool failed = false;     // protected by mutex
    std::vector<std::string> paths; // protected by mutex
    std::string start_dir; // persists until the callback
};

static const SDL_DialogFileFilter pkg_filters[] = {{"PS4 PKG files", "pkg"}};
static void SDLCALL picker_callback(void* userdata, const char* const* paths, int) {
    auto& state = *static_cast<NativePicker*>(userdata);
    std::lock_guard lock(state.mutex);
    state.failed = paths == nullptr;
    state.paths.clear();
    if (paths) for (int i = 0; paths[i]; ++i) state.paths.emplace_back(paths[i]);
    state.completed = true;
}

struct Copy {
    const char* category;
    const char* title;
    const char* description;
    const char* action;
    bool multiple;
};

static Copy get_copy(const std::string& mode) {
    if (mode == "update")
        return {"GAME UPDATE / CUSA16429", "Version 1.01 required",
                "Sakura Wars v1.00 was detected. This native port requires v1.01.\n"
                "Select your v1.01 update PKG. This updates your extracted\n"
                "CUSA folder in place, replacing v1.00 files with v1.01.\n"
                "Your original PKG and game saves are not modified.", "Apply Update", false};
    if (mode == "dlc")
        return {"OPTIONAL DLC / CUSA16429", "Add extra content?",
                "Select any DLC PKGs you own, or skip this optional step.\n"
                "You can add DLC later from the in-game overlay.",
                "Install DLC", true};
    return {"ADDITIONAL CONTENT / CUSA16429", "Install DLC",
            "Select DLC PKGs you own. Existing add-ons will be preserved.\n"
            "Save and restart the game to activate new DLC.",
            "Install DLC", true};
}

struct Progress {
    std::string state = "working";
    int completed = 0;
    int total = 5;
    std::string message = "Starting update...";
};

static Progress read_progress(const fs::path& path, Progress previous) {
    std::ifstream file(path);
    if (!file) return previous;
    std::string line;
    if (!std::getline(file, line)) return previous;
    std::istringstream fields(line);
    std::string state, done, total, message;
    if (!std::getline(fields, state, '\t') ||
        !std::getline(fields, done, '\t') ||
        !std::getline(fields, total, '\t') ||
        !std::getline(fields, message)) return previous;
    if (state != "working" && state != "success" && state != "error") return previous;
    try {
        const int finished = std::stoi(done);
        const int stages = std::stoi(total);
        if (stages < 1 || stages > 10000 || finished < 0 || finished > stages)
            return previous;
        return {state, finished, stages, message.substr(0, 400)};
    } catch (...) {
        return previous;
    }
}

int main(int argc, char** argv) {
    const bool progress_mode = argc == 4 && std::strcmp(argv[1], "--mode") == 0 &&
                               (std::strcmp(argv[2], "progress") == 0 ||
                                std::strcmp(argv[2], "progress-dlc") == 0);
    const bool select_mode = argc == 3 && std::strcmp(argv[1], "--mode") == 0 &&
                             (std::strcmp(argv[2], "update") == 0 ||
                              std::strcmp(argv[2], "dlc") == 0 ||
                              std::strcmp(argv[2], "dlc_later") == 0);
    if (!progress_mode && !select_mode) {
        std::fprintf(stderr, "Usage: ps4rt-setup --mode update|dlc|dlc_later OR --mode progress|progress-dlc /path/status\n");
        return 2;
    }
    const std::string mode = argv[2];
    const bool dlc_progress_mode = mode == "progress-dlc";
    const Copy copy = progress_mode ? get_copy(dlc_progress_mode ? "dlc" : "update")
                                    : get_copy(mode);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window* window = SDL_CreateWindow("Sakura Wars | Update and DLC Setup",
                                          720, 400, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "Setup window failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "Setup renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 2;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    configure_setup_font();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 9.0f;
    style.FrameRounding = 5.0f;
    // The main ImGui window is transparent; SDL draws the shared artwork behind it.
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.14f, .08f, .15f, 0);
    style.Colors[ImGuiCol_Button] = ImVec4(.58f, .21f, .40f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.77f, .33f, .55f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(.88f, .41f, .60f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.24f, .14f, .23f, 1);
    // ProgressBar uses PlotHistogram for its filled portion; keep the Sakura pink UI.
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(.18f, .78f, .42f, 1);
    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer) ||
        !ImGui_ImplSDLRenderer3_Init(renderer)) {
        std::fprintf(stderr, "Unable to initialize setup GUI.\n");
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 2;
    }

    SDL_Texture* background_art = load_setup_art(renderer);
    NativePicker picker;
    std::set<fs::path> chosen;
    fs::path recent_folder = initial_directory();
    std::string error;
    bool manual_entry = false;
    char manual_path[4096] = {};
    bool done = false, accepted = false;
    bool close_after_picker = false;
    Progress progress;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                // Do not deallocate the callback state before SDL responds.
                if (picker.waiting) close_after_picker = true;
                else done = true;
            }
        }
        if (picker.waiting) {
            bool completed = false, failed = false;
            std::vector<std::string> files;
            {
                std::lock_guard lock(picker.mutex);
                if (picker.completed) {
                    completed = true;
                    failed = picker.failed;
                    files = std::move(picker.paths);
                    picker.completed = false;
                }
            }
            if (completed) {
                picker.waiting = false;
                if (close_after_picker) {
                    done = true;
                } else if (failed) {
                    manual_entry = true;
                    error = "Desktop chooser unavailable. Enter a PKG path below.";
                } else if (!files.empty()) {
                    std::vector<fs::path> validated;
                    for (const auto& name : files) {
                        const fs::path source(name);
                        if (!regular_pkg(source)) {
                            error = "Selected file is not a readable .pkg.";
                            validated.clear();
                            break;
                        }
                        std::error_code ec;
                        fs::path full = fs::absolute(source, ec);
                        if (ec) {
                            error = "Couldn't resolve the selected file path.";
                            validated.clear();
                            break;
                        }
                        validated.push_back(full);
                    }
                    if (!validated.empty()) {
                        if (!copy.multiple) chosen.clear();
                        for (const auto& file : validated) chosen.insert(file);
                        recent_folder = validated.front().parent_path();
                        error.clear();
                    }
                }
            }
        }
        if (done) break;
        if (progress_mode) progress = read_progress(argv[3], std::move(progress));

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("##SakuraSetup", nullptr, ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored({.98f, .55f, .75f, 1}, "SAKURA WARS  /  PC SETUP");
        ImGui::Separator();
        ImGui::Spacing();
        if (progress_mode) {
            ImGui::TextColored({1, .67f, .82f, 1},
                               dlc_progress_mode ? "OPTIONAL DLC / CUSA16429" :
                                                   "GAME UPDATE / CUSA16429");
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextUnformatted(dlc_progress_mode ? "Installing additional content" :
                                                       "Preparing version 1.01");
            ImGui::SetWindowFontScale(1);
            ImGui::Spacing();
            ImGui::TextWrapped("%s", progress.message.c_str());
            ImGui::Spacing();
            const float fraction = static_cast<float>(progress.completed) / progress.total;
            ImGui::ProgressBar(fraction, {-1, 26});
            ImGui::TextDisabled("%d of %d setup stages completed",
                                progress.completed, progress.total);
            if (progress.state == "working") {
                constexpr char spinner[] = "|/-\\";
                const int step = static_cast<int>(SDL_GetTicks() / 180) % 4;
                ImGui::Text("%c  Processing... Please keep this window open.",
                            spinner[step]);
            } else if (progress.state == "success") {
                ImGui::TextColored({.60f, .95f, .75f, 1},
                                   dlc_progress_mode ? "DLC setup complete. Restart to activate." :
                                                       "Version 1.01 is ready.");
            } else {
                ImGui::TextColored({1, .5f, .5f, 1},
                                   "Setup failed. See the launch terminal for details.");
            }
            ImGui::Spacing();
            ImGui::TextDisabled(dlc_progress_mode ?
                                "Three completed stages per DLC; not bytes or elapsed time." :
                                "The bar tracks completed stages, not bytes or elapsed time.");
        } else {
            ImGui::TextColored({1, .67f, .82f, 1}, "%s", copy.category);
            ImGui::SetWindowFontScale(1.35f);
            ImGui::TextUnformatted(copy.title);
            ImGui::SetWindowFontScale(1);
            ImGui::TextWrapped("%s", copy.description);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::BeginDisabled(picker.waiting);
            if (ImGui::Button(copy.multiple ? "Select PKGs..." : "Select PKG...",
                              {200, 42})) {
                picker.start_dir = recent_folder.string();
                picker.waiting = true;
                SDL_ShowOpenFileDialog(picker_callback, &picker, window,
                                       pkg_filters, 1, picker.start_dir.c_str(),
                                       copy.multiple);
            }
            ImGui::EndDisabled();
            if (picker.waiting) ImGui::TextDisabled("Desktop file chooser open...");
            if (!error.empty())
                ImGui::TextColored({1, .68f, .55f, 1}, "%s", error.c_str());

            // Only expose manual entry when requested; no duplicate file tree.
            if (ImGui::SmallButton(manual_entry ? "Hide manual path" : "Enter path manually"))
                manual_entry = !manual_entry;
            if (manual_entry) {
                ImGui::SetNextItemWidth(-135);
                ImGui::InputText("##pkgpath", manual_path, sizeof(manual_path));
                ImGui::SameLine();
                if (ImGui::Button("Add PKG")) {
                    const fs::path candidate(manual_path);
                    if (!regular_pkg(candidate)) {
                        error = "Enter a valid full path ending in .pkg.";
                    } else {
                        std::error_code ec;
                        const auto full = fs::absolute(candidate, ec);
                        if (ec) error = "Unable to resolve the PKG path.";
                        else {
                            if (!copy.multiple) chosen.clear();
                            chosen.insert(full);
                            recent_folder = full.parent_path();
                            error.clear();
                            manual_path[0] = '\0';
                        }
                    }
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            if (chosen.empty()) ImGui::TextDisabled("No PKG selected");
            else {
                ImGui::Text("%zu PKG%s selected", chosen.size(), chosen.size() == 1 ? "" : "s");
                ImGui::BeginChild("##chosen", {0, 77}, false);
                for (const auto& file : chosen) {
                    ImGui::BulletText("%s", file.filename().string().c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", file.string().c_str());
                }
                ImGui::EndChild();
                if (copy.multiple && ImGui::SmallButton("Clear selection")) chosen.clear();
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(chosen.empty() || picker.waiting);
            if (ImGui::Button(copy.action, {210, 42})) {
                accepted = true;
                done = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(picker.waiting);
            if (ImGui::Button(mode == "update" ? "Not Now" :
                              mode == "dlc" ? "Close and launch game" : "Cancel", {210, 42}))
                done = true;
            ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::Render();
        draw_setup_background(renderer, background_art);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    if (background_art) SDL_DestroyTexture(background_art);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!accepted) return 1;
    for (const auto& path : chosen)
        std::printf("%s\n", path.string().c_str());
    return 0;
}
