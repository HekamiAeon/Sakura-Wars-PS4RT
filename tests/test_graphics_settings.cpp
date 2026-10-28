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
    unlink(path);
    std::puts("PASS: native-scene settings persistence and native-port defaults");
}
