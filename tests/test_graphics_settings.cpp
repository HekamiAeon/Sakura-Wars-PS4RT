// SPDX-License-Identifier: GPL-2.0-or-later
// Settings persistence and safe provider transitions, without game files or Vulkan.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#include "ps4rt_settings.h"

int main() {
    using namespace Ps4RtSettings;
    char path[] = "/tmp/ps4rt-scene-settings-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("PS4RT_CONFIG", path, 1);
    for (const char* key : {"PS4RT_UPSCALER", "PS4RT_OUTPUT_RES", "PS4RT_RCAS",
                            "PS4RT_OBJECT_MOTION", "PS4RT_JITTER"}) unsetenv(key);
    auto& s = Get();
    Load();
    assert(s.native_scene && s.CompactGraphicsMenu());
    assert(s.upscaler == UpscalerOff && s.output_res == OutputDefault);
    assert(!s.object_motion && !s.jitter && !s.rcas && s.temporal_problem);
    assert(!s.fsr_jitter); // Camera jitter defaults off (CUSA16429 fine-shading flicker)
    for (int provider : {UpscalerOff, UpscalerFsr3, UpscalerFsr4}) {
        for (int output : {1, 2, 3}) {
            const bool fsr_jitter = output == 2; // both saved states
            s.fsr_jitter = fsr_jitter;
            s.sharpen = true;
            s.sharpness = 0.42f;
            s.anisotropy = 16;
            s.framerate = 60;
            s.output_res = output;
            SetSceneUpscaler(provider, false);
            Save();
            Load();
            assert(s.upscaler == provider && s.startup_upscaler == provider);
            assert(s.output_res == output && s.startup_output_res == output);
            assert(s.sharpness > 0.419f && s.sharpness < 0.421f);
            assert(s.anisotropy == 16 && s.startup_anisotropy == 16);
            assert(s.framerate == 60);
            assert(!s.object_motion && !s.fsr1 && !s.spatial_aa);
            assert(s.fsr_jitter == fsr_jitter);
            assert(s.jitter == (provider != UpscalerOff && fsr_jitter));
            assert(s.rcas == (provider == UpscalerOff));
        }
    }
    setenv("PS4RT_UPSCALER", "off", 1);
    setenv("PS4RT_OUTPUT_RES", "1920x1080", 1);
    Load();
    assert(s.upscaler == UpscalerOff && s.output_res == 1 && !s.jitter);
    unsetenv("PS4RT_UPSCALER");
    unsetenv("PS4RT_OUTPUT_RES");
    Load();
    assert(!s.object_motion && s.temporal_problem);

    // Game language (ps4rt_launch.py protocol): the env carries the active console language;
    // ps4rt.ini only ever carries a pending change, never read back by Load().
    const auto ini_language = [&] {
        FILE* file = std::fopen(path, "r");
        assert(file);
        char line[256];
        int value = -99;
        while (std::fgets(line, sizeof(line), file)) std::sscanf(line, "language=%d", &value);
        std::fclose(file);
        return value;
    };
    unsetenv("PS4RT_LANGUAGE_CHOICES");
    setenv("PS4RT_LANGUAGE", "3", 1);
    Load();
    assert(s.language == -1 && s.startup_language == -1); // no choices offered: no row
    Save();
    assert(ini_language() == -1);
    setenv("PS4RT_LANGUAGE_CHOICES", "1=English|3=Spanish|0=Japanese", 1);
    Load();
    assert(s.language == 3 && s.startup_language == 3);
    Save();
    assert(ini_language() == -1); // unchanged: no pending choice
    s.language = 0;               // the menu picks Japanese
    Save();
    assert(ini_language() == 0);
    Load(); // a stale pending value never becomes the current language
    assert(s.language == 3 && s.startup_language == 3);
    s.language = 1;
    s.language = 3; // picked, then picked back: nothing pending
    Save();
    assert(ini_language() == -1);
    for (const char* bad : {"", "x", "3x", "-1", "31"}) {
        setenv("PS4RT_LANGUAGE", bad, 1);
        Load();
        assert(s.language == -1 && s.startup_language == -1);
    }
    unsetenv("PS4RT_LANGUAGE");
    unsetenv("PS4RT_LANGUAGE_CHOICES");
    unlink(path);
    std::puts("PASS: native-scene settings persistence, native-port defaults and the pending game-language protocol");
}
