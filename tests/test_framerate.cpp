// SPDX-License-Identifier: GPL-2.0-or-later
// Game frame rate hook (CUSA16429 settings object layout), without game files.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "ps4rt_framerate.h"
#include "ps4rt_graphics.h"

namespace {
using Ps4RtFramerate::Sakura::Apply;
using Ps4RtFramerate::Sakura::State;

struct Fields {
    std::uint8_t fixed;
    float fps;
    std::int32_t interval;
};
// Synthetic settings object: fixed-step flag +0x18, fps +0x1c, interval +0x20.
std::array<std::uint8_t, 0x40> Object(std::uint8_t fixed, float fps, std::int32_t interval) {
    std::array<std::uint8_t, 0x40> o{};
    o[0x18] = fixed;
    std::memcpy(o.data() + 0x1c, &fps, 4);
    std::memcpy(o.data() + 0x20, &interval, 4);
    return o;
}
Fields Read(const std::array<std::uint8_t, 0x40>& o) {
    Fields f{o[0x18], 0, 0};
    std::memcpy(&f.fps, o.data() + 0x1c, 4);
    std::memcpy(&f.interval, o.data() + 0x20, 4);
    return f;
}
} // namespace

int main() {
    // 30 selected from the start: the game's own values are never written.
    auto o = Object(1, 30.0f, 2);
    State state;
    assert(Apply(o.data(), 30, state) && state.applied == 30);
    auto f = Read(o);
    assert(f.fps == 30.0f && f.interval == 2);

    // 60: step 1/60 and one vblank per flip; rewritten after the game's scene setup.
    assert(Apply(o.data(), 60, state) && state.applied == 60);
    f = Read(o);
    assert(f.fixed == 1 && f.fps == 60.0f && f.interval == 1);
    o = Object(1, 30.0f, 2); // a scene setup writes the game's 30 back
    assert(Apply(o.data(), 60, state));
    f = Read(o);
    assert(f.fps == 60.0f && f.interval == 1);

    // Back to 30: the game's values restored once, then left to the game.
    assert(Apply(o.data(), 30, state) && state.applied == 30);
    f = Read(o);
    assert(f.fps == 30.0f && f.interval == 2);
    float fps = 60.0f; // whatever the game writes later is not touched
    std::memcpy(o.data() + 0x1c, &fps, 4);
    assert(Apply(o.data(), 30, state));
    assert(Read(o).fps == 60.0f);

    // Unexpected contents: nothing written.
    for (auto bad : {Object(0, 30.0f, 2), Object(1, 45.0f, 2), Object(1, 30.0f, 3)}) {
        State s;
        const auto before = bad;
        assert(!Apply(bad.data(), 60, s) && bad == before && s.applied == 30);
    }

    // The loop signature does not match arbitrary bytes or a short span.
    std::array<std::uint8_t, 0x2c> zeros{};
    assert(!Ps4RtFramerate::Sakura::CodeMatches(zeros));
    assert(!Ps4RtFramerate::Sakura::CodeMatches(std::span(zeros).first(4)));

    // Dash fix on a synthetic image (the clamp, lift, ground dash, jump, cut-in and event track
    // sites' original bytes, zero padding).
    {
        using namespace Ps4RtFramerate::Sakura;
        const std::size_t size = 0x1423000;
        auto* image = static_cast<std::uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        assert(image != MAP_FAILED);
        const std::uint8_t sites[4][7] = {{5, 0xc5, 0xea, 0x59, 0x58, 0x04},
                                          {5, 0xc5, 0xea, 0x59, 0x58, 0x08},
                                          {6, 0xc4, 0xc1, 0x6a, 0x59, 0x67, 0x04},
                                          {6, 0xc4, 0xc1, 0x6a, 0x59, 0x5f, 0x08}};
        const std::uintptr_t offsets[4] = {0x9d9978, 0x9d99a0, 0x9d9ae5, 0x9d9b09};
        std::memset(image + 0x9d9900, 0xcc, 0x300); // neighbouring code must stay unchanged
        for (int i = 0; i < 4; ++i) std::memcpy(image + offsets[i], sites[i] + 1, sites[i][0]);
        std::array<std::uint8_t, 0x300> original;
        std::memcpy(original.data(), image + 0x9d9900, original.size());
        // Lift: mov 0x2279236(%rip), %rax (the settings slot) amid other code.
        const std::uint8_t lift[7] = {0x48, 0x8b, 0x05, 0x36, 0x92, 0x27, 0x02};
        const std::uintptr_t lift_at = 0xa2ae93;
        std::memset(image + 0xa2ae80, 0xcc, 0x40);
        std::memcpy(image + lift_at, lift, 7);
        std::array<std::uint8_t, 0x40> lift_area;
        std::memcpy(lift_area.data(), image + 0xa2ae80, lift_area.size());
        // Jump thrust test: vucomiss (%rax), %xmm0; jbe 0x9d528b.
        const std::uint8_t jump[10] = {0xc5, 0xf8, 0x2e, 0x00, 0x0f, 0x86, 0x81, 0x01, 0x00, 0x00};
        const std::uintptr_t jump_at = 0x9d5100;
        std::memset(image + 0x9d50f0, 0xcc, 0x30);
        std::memcpy(image + jump_at, jump, 10);
        std::array<std::uint8_t, 0x30> jump_area;
        std::memcpy(jump_area.data(), image + 0x9d50f0, jump_area.size());
        // Cut-in track: mov 0x2a7c795(%rip), %rax (the settings slot) amid other code.
        const std::uint8_t cutin[7] = {0x48, 0x8b, 0x05, 0x95, 0xc7, 0xa7, 0x02};
        const std::uintptr_t cutin_at = 0x227934;
        std::memset(image + 0x227920, 0xcc, 0x30);
        std::memcpy(image + cutin_at, cutin, 7);
        std::array<std::uint8_t, 0x30> cutin_area;
        std::memcpy(cutin_area.data(), image + 0x227920, cutin_area.size());
        // Ground dash start: mov 0x22d2d66(%rip), %rax (the settings slot) amid other code.
        const std::uint8_t ground[7] = {0x48, 0x8b, 0x05, 0x66, 0x2d, 0x2d, 0x02};
        const std::uintptr_t ground_at = 0x9d1363;
        std::memset(image + 0x9d1350, 0xcc, 0x30);
        std::memcpy(image + ground_at, ground, 7);
        std::array<std::uint8_t, 0x30> ground_area;
        std::memcpy(ground_area.data(), image + 0x9d1350, ground_area.size());
        // Ground dash cap: vmovaps -0xb0(%rbp), %xmm1 amid other code.
        const std::uint8_t cap[8] = {0xc5, 0xf8, 0x28, 0x8d, 0x50, 0xff, 0xff, 0xff};
        const std::uintptr_t cap_at = 0x9d2701;
        std::memset(image + 0x9d26f0, 0xcc, 0x30);
        std::memcpy(image + cap_at, cap, 8);
        std::array<std::uint8_t, 0x30> cap_area;
        std::memcpy(cap_area.data(), image + 0x9d26f0, cap_area.size());
        // Event segment time: vmovd 0x1378(%r13), %xmm0 amid other code.
        const std::uint8_t seg[9] = {0xc4, 0xc1, 0x79, 0x6e, 0x85, 0x78, 0x13, 0x00, 0x00};
        const std::uintptr_t seg_at = 0x1d9824;
        std::memset(image + 0x1d9810, 0xcc, 0x30);
        std::memcpy(image + seg_at, seg, 9);
        std::array<std::uint8_t, 0x30> seg_area;
        std::memcpy(seg_area.data(), image + 0x1d9810, seg_area.size());
        // Air attack falling boost: vinsertps $0x1d, 0x50(%r12), %xmm0, %xmm0 amid other code.
        const std::uint8_t boost[8] = {0xc4, 0xc3, 0x79, 0x21, 0x44, 0x24, 0x50, 0x1d};
        const std::uintptr_t boost_at = 0x9dc827;
        std::memset(image + 0x9dc810, 0xcc, 0x30);
        std::memcpy(image + boost_at, boost, 8);
        std::array<std::uint8_t, 0x30> boost_area;
        std::memcpy(boost_area.data(), image + 0x9dc810, boost_area.size());
        // Side-step cap: vmulss 0x18(%rdx), %xmm3, %xmm2 amid other code.
        const std::uint8_t step[5] = {0xc5, 0xe2, 0x59, 0x52, 0x18};
        const std::uintptr_t step_at = 0x9d7988;
        std::memset(image + 0x9d7970, 0xcc, 0x30);
        std::memcpy(image + step_at, step, 5);
        std::array<std::uint8_t, 0x30> step_area;
        std::memcpy(step_area.data(), image + 0x9d7970, step_area.size());
        // Fall speed caps: vxorps (sign mask)(%rip), %xmm0, %xmm0 in both postures.
        const std::uint8_t fall_jump[8] = {0xc5, 0xf8, 0x57, 0x05, 0x82, 0x7a, 0x78, 0x00};
        const std::uint8_t fall_attack[8] = {0xc5, 0xf8, 0x57, 0x05, 0x95, 0x08, 0x78, 0x00};
        const std::uintptr_t fall_at[2] = {0x9d5216, 0x9dc7a3};
        std::memset(image + 0x9d5200, 0xcc, 0x30);
        std::memcpy(image + fall_at[0], fall_jump, 8);
        std::memcpy(image + fall_at[1], fall_attack, 8);
        std::array<std::uint8_t, 0x30> fall_area;
        std::memcpy(fall_area.data(), image + 0x9d5200, fall_area.size());
        // Motion key start: call *0x228(%rax) amid other code.
        const std::uint8_t motion[6] = {0xff, 0x90, 0x28, 0x02, 0x00, 0x00};
        const std::uintptr_t motion_at = 0x1dbc6a;
        std::memset(image + 0x1dbc50, 0xcc, 0x30);
        std::memcpy(image + motion_at, motion, 6);
        std::array<std::uint8_t, 0x30> motion_area;
        std::memcpy(motion_area.data(), image + 0x1dbc50, motion_area.size());
        // Motion node clip time: xor %esi, %esi; mov (%rdi), %rax amid other code.
        const std::uint8_t node[5] = {0x31, 0xf6, 0x48, 0x8b, 0x07};
        const std::uintptr_t node_at = 0x1dbf31;
        std::memset(image + 0x1dbf20, 0xcc, 0x30);
        std::memcpy(image + node_at, node, 5);
        std::array<std::uint8_t, 0x30> node_area;
        std::memcpy(node_area.data(), image + 0x1dbf20, node_area.size());
        // Upper-body layer: the apply's and the offset's settings loads (mov rip, %rax / %rcx).
        const std::uint8_t layer[2][7] = {{0x48, 0x8b, 0x05, 0x51, 0xec, 0xab, 0x02},
                                          {0x48, 0x8b, 0x0d, 0x70, 0xf1, 0xab, 0x02}};
        const std::uintptr_t layer_at[2] = {0x1e5478, 0x1e4f59};
        std::memset(image + 0x1e4f40, 0xcc, 0x30);
        std::memset(image + 0x1e5460, 0xcc, 0x30);
        std::memcpy(image + layer_at[0], layer[0], 7);
        std::memcpy(image + layer_at[1], layer[1], 7);
        std::array<std::uint8_t, 0x30> layer_area[2];
        std::memcpy(layer_area[0].data(), image + 0x1e5460, 0x30);
        std::memcpy(layer_area[1].data(), image + 0x1e4f40, 0x30);
        // Event motion track: three settings loads (mov rip, %rax) and one fps read.
        const std::uint8_t motrack[3][7] = {{0x48, 0x8b, 0x05, 0xa2, 0x23, 0xa8, 0x02},
                                            {0x48, 0x8b, 0x05, 0x91, 0xf4, 0xa7, 0x02},
                                            {0x48, 0x8b, 0x05, 0x32, 0xf4, 0xa7, 0x02}};
        const std::uintptr_t motrack_at[3] = {0x221d27, 0x224c38, 0x224c97};
        const std::uint8_t fps_read[5] = {0xc5, 0xfa, 0x10, 0x40, 0x1c};
        const std::uintptr_t fps_read_at = 0x2252d6;
        std::memset(image + 0x221d10, 0xcc, 0x30);
        std::memset(image + 0x224c30, 0xcc, 0x80);
        std::memset(image + 0x2252c0, 0xcc, 0x30);
        for (int i = 0; i < 3; ++i) std::memcpy(image + motrack_at[i], motrack[i], 7);
        std::memcpy(image + fps_read_at, fps_read, 5);
        std::array<std::uint8_t, 0x30> motrack_area[3];
        std::array<std::uint8_t, 0x80> motrack_mid;
        std::memcpy(motrack_area[0].data(), image + 0x221d10, 0x30);
        std::memcpy(motrack_mid.data(), image + 0x224c30, 0x80);
        std::memcpy(motrack_area[2].data(), image + 0x2252c0, 0x30);
        assert(DashFixState(image) == DashFix::Original);

        assert(SetDashFix(image, true) && DashFixState(image) == DashFix::Fixed);
        // Each site calls its routine entry (DashCave + 0x10 * (i + 1)), padded with a nop.
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t* p = image + offsets[i];
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && offsets[i] + 5 + rel == DashCave + 0x10 * (i + 1));
            assert(sites[i][0] == 5 || p[5] == 0x90);
        }
        // The lift's load becomes lea (DashCave - 0x34)(%rip), %rax: 0x34(%rax) reads 1/30.
        {
            const std::uint8_t* p = image + lift_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 3, 4);
            assert(p[0] == 0x48 && p[1] == 0x8d && p[2] == 0x05);
            assert(lift_at + 7 + rel + 0x34 == DashCave);
        }
        // The upper-body layer's loads become lea (DashCave + 8 - 0x1c)(%rip) into the same
        // register: 0x1c(reg) reads 30.0.
        for (int i = 0; i < 2; ++i) {
            const std::uint8_t* p = image + layer_at[i];
            std::int32_t rel;
            std::memcpy(&rel, p + 3, 4);
            assert(p[0] == 0x48 && p[1] == 0x8d && p[2] == layer[i][2]);
            assert(layer_at[i] + 7 + rel + 0x1c == DashCave + 8);
        }
        // The event motion track's loads likewise; its last read calls the routine's 30.0 load.
        for (int i = 0; i < 3; ++i) {
            const std::uint8_t* p = image + motrack_at[i];
            std::int32_t rel;
            std::memcpy(&rel, p + 3, 4);
            assert(p[0] == 0x48 && p[1] == 0x8d && p[2] == 0x05);
            assert(motrack_at[i] + 7 + rel + 0x1c == DashCave + 8);
        }
        {
            const std::uint8_t* p = image + fps_read_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && fps_read_at + 5 + rel == DashCave + 0xc6);
            const std::uint8_t* r = image + DashCave + 0xc6;
            std::memcpy(&rel, r + 4, 4);
            assert(r[0] == 0xc5 && r[1] == 0xfa && r[2] == 0x10 && r[3] == 0x05 && r[8] == 0xc3);
            assert(DashCave + 0xc6 + 8 + rel == DashCave + 8);
        }
        // The cut-in track's load likewise.
        {
            const std::uint8_t* p = image + cutin_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 3, 4);
            assert(p[0] == 0x48 && p[1] == 0x8d && p[2] == 0x05);
            assert(cutin_at + 7 + rel + 0x34 == DashCave);
        }
        // The ground dash's load likewise.
        {
            const std::uint8_t* p = image + ground_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 3, 4);
            assert(p[0] == 0x48 && p[1] == 0x8d && p[2] == 0x05);
            assert(ground_at + 7 + rel + 0x34 == DashCave);
        }
        // The cap loads the 1/30: vmovss DashCave(%rip), %xmm1.
        {
            const std::uint8_t* p = image + cap_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 4, 4);
            assert(p[0] == 0xc5 && p[1] == 0xfa && p[2] == 0x10 && p[3] == 0x0d);
            assert(cap_at + 8 + rel == DashCave);
        }
        // The thrust test jumps to the routine at 0x50, which branches back to the original
        // targets (jbe 0x9d528b, jmp 0x9d510a) after comparing against limit x 0.5.
        {
            const std::uint8_t* p = image + jump_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe9 && jump_at + 5 + rel == DashCave + 0x50);
            for (int i = 5; i < 10; ++i) assert(p[i] == 0x90);
            const std::uint8_t* r = image + DashCave + 0x50;
            std::int32_t jbe, jmp;
            std::memcpy(&jbe, r + 0x12, 4);
            std::memcpy(&jmp, r + 0x17, 4);
            assert(r[0x10] == 0x0f && r[0x11] == 0x86 && DashCave + 0x66 + jbe == 0x9d528b);
            assert(r[0x16] == 0xe9 && DashCave + 0x6b + jmp == 0x9d510a);
        }
        // The segment time calls the routine at 0x70: the original load, a scan of the segments
        // (r8 -> list, ecx count), and in a gap vroundss $9 (round down) stored to the time and
        // the previous time; ret.
        {
            const std::uint8_t* p = image + seg_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && seg_at + 5 + rel == DashCave + 0x70);
            for (int i = 5; i < 9; ++i) assert(p[i] == 0x90);
            const std::uint8_t* r = image + DashCave + 0x70;
            assert(std::memcmp(r, seg, 9) == 0);
            const std::uint8_t scan[5] = {0x49, 0x8b, 0x30, 0x89, 0xc8};
            assert(std::memcmp(r + 9, scan, 5) == 0);
            const std::uint8_t round[6] = {0xc4, 0xe3, 0x79, 0x0a, 0xc0, 0x09};
            assert(std::memcmp(r + 0x27, round, 6) == 0);
            const std::uint8_t store[9] = {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x78, 0x13, 0x00, 0x00};
            const std::uint8_t store_prev[9] = {0xc4, 0xc1, 0x7a, 0x11, 0x85,
                                                0x7c, 0x13, 0x00, 0x00};
            assert(std::memcmp(r + 0x2d, store, 9) == 0);
            assert(std::memcmp(r + 0x36, store_prev, 9) == 0 && r[0x3f] == 0xc3);
        }
        // The boost calls the routine at 0xb0: vmovss 0x50(%r12), %xmm0; vmulss of the 0.5 at
        // DashCave + 4; vinsertps $0x1d, %xmm0, %xmm0, %xmm0; ret.
        {
            const std::uint8_t* p = image + boost_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && boost_at + 5 + rel == DashCave + 0xb0);
            for (int i = 5; i < 8; ++i) assert(p[i] == 0x90);
            const std::uint8_t* r = image + DashCave + 0xb0;
            const std::uint8_t load[7] = {0xc4, 0xc1, 0x7a, 0x10, 0x44, 0x24, 0x50};
            assert(std::memcmp(r, load, 7) == 0);
            std::int32_t k_rel;
            std::memcpy(&k_rel, r + 0xb, 4);
            assert(r[7] == 0xc5 && r[8] == 0xfa && r[9] == 0x59 && r[10] == 0x05);
            assert(DashCave + 0xbf + k_rel == DashCave + 4);
            const std::uint8_t insert[7] = {0xc4, 0xe3, 0x79, 0x21, 0xc0, 0x1d, 0xc3};
            assert(std::memcmp(r + 0xf, insert, 7) == 0);
        }
        // The side-step cap calls the routine at 0xd0: vmovss DashCave(%rip), %xmm2 (1/30);
        // vmulss 0x18(%rdx), %xmm2, %xmm2; ret.
        {
            const std::uint8_t* p = image + step_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && step_at + 5 + rel == DashCave + 0xd0);
            const std::uint8_t* r = image + DashCave + 0xd0;
            std::int32_t k_rel;
            std::memcpy(&k_rel, r + 4, 4);
            assert(r[0] == 0xc5 && r[1] == 0xfa && r[2] == 0x10 && r[3] == 0x15);
            assert(DashCave + 0xd8 + k_rel == DashCave);
            const std::uint8_t mul[6] = {0xc5, 0xea, 0x59, 0x52, 0x18, 0xc3};
            assert(std::memcmp(r + 8, mul, 6) == 0);
        }
        // Both fall caps call the routine at 0xe0: the sign flip (mask at 0x115cca0), a test of
        // the step against zero, then x 1/30 and / step; ret.
        for (const auto at : fall_at) {
            const std::uint8_t* p = image + at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && at + 5 + rel == DashCave + 0xe0);
            for (int i = 5; i < 8; ++i) assert(p[i] == 0x90);
        }
        {
            const std::uint8_t* r = image + DashCave + 0xe0;
            std::int32_t mask_rel, k_rel;
            std::memcpy(&mask_rel, r + 4, 4);
            std::memcpy(&k_rel, r + 0x16, 4);
            assert(r[0] == 0xc5 && r[1] == 0xf8 && r[2] == 0x57 && r[3] == 0x05);
            assert(DashCave + 0xe8 + mask_rel == 0x115cca0);
            assert(r[0x10] == 0x76 && r[0x11] == 0x0c);
            assert(DashCave + 0xfa + k_rel == DashCave);
            const std::uint8_t div[5] = {0xc5, 0xfa, 0x5e, 0xc1, 0xc3};
            assert(std::memcmp(r + 0x1a, div, 5) == 0);
        }
        // The motion key start calls MotionCave, padded with a nop; the routine skips the call
        // at times with a fraction and otherwise jumps to the original target.
        {
            const std::uint8_t* p = image + motion_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && motion_at + 5 + rel == MotionCave && p[5] == 0x90);
            const std::uint8_t* r = image + MotionCave;
            const std::uint8_t load[5] = {0xc5, 0xfa, 0x10, 0x45, 0xcc}; // -0x34(%rbp)
            const std::uint8_t trunc[6] = {0xc4, 0xe3, 0x79, 0x0a, 0xc8, 0x0b};
            const std::uint8_t call[6] = {0xff, 0xa0, 0x28, 0x02, 0x00, 0x00};
            assert(std::memcmp(r, load, 5) == 0 && std::memcmp(r + 5, trunc, 6) == 0);
            assert(r[0xf] == 0x75 && 0x11 + r[0x10] == 0x17 && r[0x17] == 0xc3);
            assert(std::memcmp(r + 0x11, call, 6) == 0);
        }
        // The clip time call calls NodeCave in place of its two instructions (no padding); the
        // routine guards the length call, does the two instructions and returns.
        {
            const std::uint8_t* p = image + node_at;
            std::int32_t rel;
            std::memcpy(&rel, p + 1, 4);
            assert(p[0] == 0xe8 && node_at + 5 + rel == NodeCave && p[5] == 0xcc);
            const std::uint8_t* r = image + NodeCave;
            const std::uint8_t guard[7] = {0x83, 0xb8, 0x20, 0x16, 0x00, 0x00, 0x04};
            const std::uint8_t len[6] = {0xff, 0x90, 0xd0, 0x02, 0x00, 0x00};
            const std::uint8_t tail[7] = {0x5f, 0x31, 0xf6, 0x48, 0x8b, 0x07, 0xc3};
            assert(r[0] == 0x57 && std::memcmp(r + 0x19, guard, 7) == 0);
            assert(std::memcmp(r + 0x25, len, 6) == 0 && std::memcmp(r + 0x62, tail, 7) == 0);
            std::int32_t lim_rel;
            std::memcpy(&lim_rel, r + 0x48, 4);
            float lim;
            std::memcpy(&lim, r + 0x4c + lim_rel, 4);
            assert(0x4c + lim_rel == 0x6c && lim > 1.0f / 30.0f - 0.0011f && lim < 1.0f / 30.0f);
        }
        float k, half;
        std::memcpy(&k, image + DashCave, 4);
        std::memcpy(&half, image + DashCave + 4, 4);
        float fps;
        std::memcpy(&fps, image + DashCave + 8, 4);
        assert(k == 1.0f / 30.0f && half == 0.5f && fps == 30.0f);
        assert(SetDashFix(image, true)); // already fixed: nothing to do

        // Off: original bytes back, routine left in the padding.
        assert(SetDashFix(image, false) && DashFixState(image) == DashFix::Original);
        assert(std::memcmp(original.data(), image + 0x9d9900, original.size()) == 0);
        assert(std::memcmp(lift_area.data(), image + 0xa2ae80, lift_area.size()) == 0);
        assert(std::memcmp(jump_area.data(), image + 0x9d50f0, jump_area.size()) == 0);
        assert(std::memcmp(cutin_area.data(), image + 0x227920, cutin_area.size()) == 0);
        assert(std::memcmp(ground_area.data(), image + 0x9d1350, ground_area.size()) == 0);
        assert(std::memcmp(cap_area.data(), image + 0x9d26f0, cap_area.size()) == 0);
        assert(std::memcmp(seg_area.data(), image + 0x1d9810, seg_area.size()) == 0);
        assert(std::memcmp(boost_area.data(), image + 0x9dc810, boost_area.size()) == 0);
        assert(std::memcmp(step_area.data(), image + 0x9d7970, step_area.size()) == 0);
        assert(std::memcmp(fall_area.data(), image + 0x9d5200, fall_area.size()) == 0);
        assert(std::memcmp(image + fall_at[1], fall_attack, 8) == 0);
        assert(std::memcmp(motion_area.data(), image + 0x1dbc50, motion_area.size()) == 0);
        assert(std::memcmp(node_area.data(), image + 0x1dbf20, node_area.size()) == 0);
        assert(std::memcmp(layer_area[0].data(), image + 0x1e5460, 0x30) == 0);
        assert(std::memcmp(layer_area[1].data(), image + 0x1e4f40, 0x30) == 0);
        assert(std::memcmp(motrack_area[0].data(), image + 0x221d10, 0x30) == 0);
        assert(std::memcmp(motrack_mid.data(), image + 0x224c30, 0x80) == 0);
        assert(std::memcmp(motrack_area[2].data(), image + 0x2252c0, 0x30) == 0);
        assert(SetDashFix(image, true) && SetDashFix(image, false));

        // Unexpected bytes at a site or in the padding: nothing written.
        mprotect(image, size, PROT_READ | PROT_WRITE);
        image[offsets[2] + 3] = 0x58;
        std::memcpy(original.data(), image + 0x9d9900, original.size());
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        assert(std::memcmp(original.data(), image + 0x9d9900, original.size()) == 0);
        image[offsets[2] + 3] = 0x59;
        image[lift_at + 3] = 0x37; // the lift site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[lift_at + 3] = 0x36;
        image[jump_at + 6] = 0x80; // the jump site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[jump_at + 6] = 0x81;
        image[cutin_at + 3] = 0x96; // the cut-in site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[cutin_at + 3] = 0x95;
        image[ground_at + 3] = 0x67; // the ground dash site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[ground_at + 3] = 0x66;
        image[cap_at + 4] = 0x58; // the cap site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[cap_at + 4] = 0x50;
        image[seg_at + 5] = 0x79; // the event segment site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[seg_at + 5] = 0x78;
        image[boost_at + 6] = 0x51; // the air attack site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[boost_at + 6] = 0x50;
        image[step_at + 4] = 0x19; // the side-step site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[step_at + 4] = 0x18;
        image[fall_at[1] + 4] = 0x96; // the attack fall cap site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[fall_at[1] + 4] = 0x95;
        image[motion_at + 2] = 0x30; // the motion key site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[motion_at + 2] = 0x28;
        image[node_at + 1] = 0xf7; // the clip time site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[node_at + 1] = 0xf6;
        image[layer_at[1] + 2] = 0x15; // the upper-body layer's offset site alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[layer_at[1] + 2] = 0x0d;
        image[fps_read_at + 4] = 0x20; // the event motion track's fps read alone differs
        assert(DashFixState(image) == DashFix::Unknown && !SetDashFix(image, true));
        image[fps_read_at + 4] = 0x1c;
        std::memset(image + MotionCave, 0, MotionCaveSize);
        std::memset(image + NodeCave, 0, NodeCaveSize);
        std::memset(image + DashCave, 0, DashCaveSize);
        image[MotionCave + 0x1f] = 1; // the motion routine's padding is not free
        assert(!SetDashFix(image, true) && DashFixState(image) == DashFix::Original);
        image[MotionCave + 0x1f] = 0;
        image[NodeCave + 0x40] = 1; // the clip end routine's padding is not free
        assert(!SetDashFix(image, true) && DashFixState(image) == DashFix::Original);
        image[NodeCave + 0x40] = 0;
        image[DashCave + 0x20] = 1;
        assert(!SetDashFix(image, true) && DashFixState(image) == DashFix::Original);
        munmap(image, size);
    }

    // Settings values: listed rates, anything else is 30.
    assert(Ps4RtGraphics::ParseFramerate(60) == 60 && Ps4RtGraphics::ParseFramerate(120) == 120);
    assert(Ps4RtGraphics::ParseFramerate(45) == 30 && Ps4RtGraphics::ParseFramerate(0) == 30);
    std::puts("PASS: game frame rate hook");
}
