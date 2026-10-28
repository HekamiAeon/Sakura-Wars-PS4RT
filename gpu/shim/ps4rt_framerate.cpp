// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_framerate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "ps4rt_settings.h"

namespace Ps4RtFramerate {
namespace Sakura {
namespace {
// mov rbx, [rip -> 0x2ca40d0]; vmovss xmm2, [60.0]; mov r15b, [rbp - 0x444]; vxorps xmm1;
// vmovss xmm0, [rbx + 0x1c]; vcmpltss; vandps; vandnps
constexpr std::array<std::uint8_t, 0x2c> Loop = {
    0x48, 0x8b, 0x1d, 0x79, 0x18, 0xea, 0x01, 0xc5, 0xfa, 0x10, 0x15, 0xb1, 0x7e, 0x33, 0x00,
    0x44, 0x8a, 0xbd, 0xbc, 0xfb, 0xff, 0xff, 0xc5, 0xf0, 0x57, 0xc9, 0xc5, 0xfa, 0x10, 0x43,
    0x1c, 0xc5, 0xf2, 0xc2, 0xc8, 0x01, 0xc5, 0xf0, 0x54, 0xc0, 0xc5, 0xf0, 0x55, 0xca};
constexpr std::uintptr_t FixedStep = 0x18, Fps = 0x1c, Interval = 0x20;

float ReadFloat(const std::uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
std::int32_t ReadInt(const std::uint8_t* p) {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void Write(std::uint8_t* settings, float fps, std::int32_t interval) {
    if (ReadFloat(settings + Fps) != fps) std::memcpy(settings + Fps, &fps, 4);
    if (ReadInt(settings + Interval) != interval) std::memcpy(settings + Interval, &interval, 4);
}
} // namespace

bool CodeMatches(std::span<const std::uint8_t> code) {
    return code.size() >= Loop.size() && std::equal(Loop.begin(), Loop.end(), code.begin());
}

bool Apply(std::uint8_t* settings, int fps, State& state) {
    const float current = ReadFloat(settings + Fps);
    const std::int32_t interval = ReadInt(settings + Interval);
    if (settings[FixedStep] != 1 || (current != 30.0f && current != 60.0f) ||
        (interval != 1 && interval != 2))
        return false;
    if (fps == 60) {
        Write(settings, 60.0f, 1);
        state.applied = 60;
    } else if (state.applied != 30) {
        Write(settings, 30.0f, 2);
        state.applied = 30;
    }
    return true;
}

namespace {
// The routine at DashCave: 1/30 (0x3d088889, the game's own 30 fps step), 0.5 (30 / 60) and
// 30.0 (the game's own rate, read as "fps" by the Fps sites), then one entry per clamp site:
// vmovss k(%rip), xmmD; vmulss off(base), xmmD, xmmD; ret; the jump thrust test at 0x50, the
// event segment time at 0x70, the air attack's falling boost at 0xb0, vmovss 30.0(%rip), %xmm0
// for the event motion track at 0xc6, the side-step cap at 0xd0 and the fall speed caps at 0xe0
// (see Sites).
constexpr std::array<std::uint8_t, DashCaveSize> CaveCode = {
    0x89, 0x88, 0x08, 0x3d, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0xf0, 0x41, 0x00, 0x00,
    0x00, 0x00, 0xc5, 0xfa, 0x10, 0x1d, 0xe8, 0xff, 0xff, 0xff, 0xc5, 0xe2, 0x59, 0x58,
    0x04, 0xc3, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x1d, 0xd8, 0xff, 0xff, 0xff, 0xc5, 0xe2,
    0x59, 0x58, 0x08, 0xc3, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x25, 0xc8, 0xff, 0xff, 0xff,
    0xc4, 0xc1, 0x5a, 0x59, 0x67, 0x04, 0xc3, 0x00, 0xc5, 0xfa, 0x10, 0x1d, 0xb8, 0xff,
    0xff, 0xff, 0xc4, 0xc1, 0x62, 0x59, 0x5f, 0x08, 0xc3, 0x00, 0xc5, 0xfa, 0x10, 0x08,
    0xc5, 0xf2, 0x59, 0x0d, 0xa8, 0xff, 0xff, 0xff, 0xc5, 0xf8, 0x2e, 0xc1, 0x0f, 0x86,
    0x25, 0x23, 0x5b, 0xff, 0xe9, 0x9f, 0x21, 0x5b, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc4, 0xc1, 0x79, 0x6e, 0x85, 0x78, 0x13, 0x00, 0x00, 0x49, 0x8b, 0x30, 0x89, 0xc8,
    0xc5, 0xf8, 0x2e, 0x06, 0x72, 0x0b, 0xc5, 0xfa, 0x10, 0x4e, 0x04, 0xc5, 0xf8, 0x2e,
    0xc8, 0x73, 0x20, 0x48, 0x83, 0xc6, 0x28, 0xff, 0xc8, 0x75, 0xe7, 0xc4, 0xe3, 0x79,
    0x0a, 0xc0, 0x09, 0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x78, 0x13, 0x00, 0x00, 0xc4, 0xc1,
    0x7a, 0x11, 0x85, 0x7c, 0x13, 0x00, 0x00, 0xc3, 0xc4, 0xc1, 0x7a, 0x10, 0x44, 0x24,
    0x50, 0xc5, 0xfa, 0x59, 0x05, 0x45, 0xff, 0xff, 0xff, 0xc4, 0xe3, 0x79, 0x21, 0xc0,
    0x1d, 0xc3, 0xc5, 0xfa, 0x10, 0x05, 0x3a, 0xff, 0xff, 0xff, 0xc3, 0x00, 0xc5, 0xfa,
    0x10, 0x15, 0x28, 0xff, 0xff, 0xff, 0xc5, 0xea, 0x59, 0x52, 0x18, 0xc3, 0x00, 0x00,
    0xc5, 0xf8, 0x57, 0x05, 0xb8, 0x9c, 0xd3, 0xff, 0xc5, 0xd0, 0x57, 0xed, 0xc5, 0xf8,
    0x2e, 0xcd, 0x76, 0x0c, 0xc5, 0xfa, 0x59, 0x05, 0x06, 0xff, 0xff, 0xff, 0xc5, 0xfa,
    0x5e, 0xc1, 0xc3};
// The routine at MotionCave, called in place of the motion node's start-motion call (see
// Sites): vmovss -0x34(%rbp), %xmm0 (the node's event time); vroundss $0xb (truncate), %xmm0,
// %xmm0, %xmm1; vucomiss %xmm1, %xmm0; jne ret; jmp *0x228(%rax) (the original call, which
// returns to the site); ret.
constexpr std::array<std::uint8_t, 0x18> MotionCode = {
    0xc5, 0xfa, 0x10, 0x45, 0xcc, 0xc4, 0xe3, 0x79, 0x0a, 0xc8, 0x0b, 0xc5,
    0xf8, 0x2e, 0xc1, 0x75, 0x06, 0xff, 0xa0, 0x28, 0x02, 0x00, 0x00, 0xc3};
// The routine at NodeCave, called in place of the clip time call's "xor %esi, %esi; mov (%rdi),
// %rax" (see Sites) with xmm0 = the node's clip time, xmm1 = the blend source's, rdi = the actor:
// push %rdi; saves xmm0 and xmm1; if actor[1] && actor[1]->0x1620 != 4 (the game's own guard for
// the length call after it), call *0x2d0(%rax) (the clip length); if length > 0 and
// 0 < time - length < 1/30 - 0.001 (the float at 0x6c), the saved time becomes the length;
// restores xmm0 and xmm1, pop %rdi; xor %esi, %esi; mov (%rdi), %rax; ret.
constexpr std::array<std::uint8_t, NodeCaveSize> NodeCode = {
    0x57, 0x48, 0x83, 0xec, 0x20, 0xc5, 0xfa, 0x11, 0x04, 0x24, 0xc5, 0xfa, 0x11, 0x4c,
    0x24, 0x04, 0x48, 0x8b, 0x47, 0x08, 0x48, 0x85, 0xc0, 0x74, 0x3a, 0x83, 0xb8, 0x20,
    0x16, 0x00, 0x00, 0x04, 0x74, 0x31, 0x48, 0x8b, 0x07, 0xff, 0x90, 0xd0, 0x02, 0x00,
    0x00, 0xc5, 0xe0, 0x57, 0xdb, 0xc5, 0xf8, 0x2e, 0xc3, 0x76, 0x1e, 0xc5, 0xfa, 0x10,
    0x0c, 0x24, 0xc5, 0xf2, 0x5c, 0xd0, 0xc5, 0xf8, 0x2e, 0xd3, 0x76, 0x0f, 0xc5, 0xf8,
    0x2e, 0x15, 0x20, 0x00, 0x00, 0x00, 0x73, 0x05, 0xc5, 0xfa, 0x11, 0x04, 0x24, 0xc5,
    0xfa, 0x10, 0x04, 0x24, 0xc5, 0xfa, 0x10, 0x4c, 0x24, 0x04, 0x48, 0x83, 0xc4, 0x20,
    0x5f, 0x31, 0xf6, 0x48, 0x8b, 0x07, 0xc3, 0x0f, 0x1f, 0x00, 0xec, 0x6f, 0x04, 0x3d};
enum class Kind { Call, Lea, Load, Jump, Fps };
struct Site {
    std::uintptr_t offset;
    std::size_t length;
    std::array<std::uint8_t, 10> original;
    Kind kind;
    std::uintptr_t entry; // in the routine (Call, Jump)
    std::uintptr_t routine = DashCave;
};
constexpr std::array<Site, 22> Sites = {{
    {0x9d9978, 5, {0xc5, 0xea, 0x59, 0x58, 0x04}, Kind::Call, 0x10}, // vmulss 4(%rax), %xmm2, %xmm3
    {0x9d99a0, 5, {0xc5, 0xea, 0x59, 0x58, 0x08}, Kind::Call, 0x20}, // vmulss 8(%rax), %xmm2, %xmm3
    {0x9d9ae5, 6, {0xc4, 0xc1, 0x6a, 0x59, 0x67, 0x04}, Kind::Call, 0x30}, // vmulss 4(%r15), %xmm2, %xmm4
    {0x9d9b09, 6, {0xc4, 0xc1, 0x6a, 0x59, 0x5f, 0x08}, Kind::Call, 0x40}, // vmulss 8(%r15), %xmm2, %xmm3
    // Dash start lift (0xa2ae8e..0xa2aecd): y = P+0x14 (650) x [settings + 0x34] (the step).
    // mov [rip -> settings slot], %rax becomes lea (DashCave - 0x34)(%rip), %rax, so the
    // following vmulss 0x34(%rax) reads 1/30; rax is dead after it on both paths.
    {0xa2ae93, 7, {0x48, 0x8b, 0x05, 0x36, 0x92, 0x27, 0x02}, Kind::Lea, 0},
    // Cut-in 2D tracks (0x227840): a SurfRide layer's frame = (event time - track start) x
    // [settings + 0x34] x 60, the event time counted in 1/30 s; at 60 the lettering plays at
    // half speed. Same load swap as the lift; rax is reloaded at 0x227944.
    {0x227934, 7, {0x48, 0x8b, 0x05, 0x95, 0xc7, 0xa7, 0x02}, Kind::Lea, 0},
    // Ground dash start (0x9d10c0): velocity = direction x speed parameter x [settings + 0x34],
    // set once and carried by the motion update; at 60 the dash runs at half speed. Same load
    // swap; rax is reloaded at 0x9d138a.
    {0x9d1363, 7, {0x48, 0x8b, 0x05, 0x66, 0x2d, 0x2d, 0x02}, Kind::Lea, 0},
    // Its per-frame update (0x9d16f0, step in xmm0, kept at -0xb0(%rbp)) caps the speed at the
    // same parameter x step. vmovaps -0xb0(%rbp), %xmm1 becomes vmovss DashCave(%rip), %xmm1:
    // only the low lane is used (vmulss, then broadcast).
    {0x9d2701, 8, {0xc5, 0xf8, 0x28, 0x8d, 0x50, 0xff, 0xff, 0xff}, Kind::Load, 0},
    // Jump thrust (0x9d50fa): applied while the jump timer (counted up by the step) exceeds
    // 0.03, just under one 30 fps update, so at 60 the first update of each timer run is
    // skipped. vucomiss (%rax), %xmm0; jbe 0x9d528b becomes a jump to the routine, which
    // compares against the limit x 0.5 and continues at 0x9d528b or 0x9d510a. xmm1 is dead
    // on both paths (reloaded at 0x9d5115 and 0x9d5294). The rates above 30 are 60 only.
    {0x9d5100, 10, {0xc5, 0xf8, 0x2e, 0x00, 0x0f, 0x86, 0x81, 0x01, 0x00, 0x00}, Kind::Jump, 0x50},
    // Event track tree update (0x1d9620): it runs only the nodes of the segment holding the
    // event time (start <= t <= end, both passes). Segments are cut at whole 1/30 s frames, one
    // per shot ([520, 618], [619, 661]), so at 60 the half frame before each shot cut (618.5)
    // falls in none and no track updates: a fade spanning the cut drops out for a frame. Running
    // the old shot's tracks at 618.5 instead puts its camera past its last frame (a one-frame
    // flash of another angle), so that frame holds the old shot as 30 fps does. vmovd
    // 0x1378(%r13), %xmm0 (the tree's time, before the segment loop; r8 -> segment list, ecx
    // count > 0) becomes a call to the routine at 0x70: when no segment holds the time, it
    // rounds the time down and stores it back to 0x1378 and the previous time 0x137c (equal to
    // it here), which the nodes are updated with: a repeat of the old shot's last frame. With
    // the previous time left at 618.5 the subtitles saw time run backwards and flickered.
    // rax, rsi, xmm1 and the flags are reloaded by the loop; xmm0 is dead after it.
    {0x1d9824, 9, {0xc4, 0xc1, 0x79, 0x6e, 0x85, 0x78, 0x13, 0x00, 0x00}, Kind::Call, 0x70},
    // Attack posture update (0x9dbb40, air attacks): after gravity (step x g, correct) a falling
    // velocity gets a fixed boost, vy += [state + 0x50] (4.25), once per update with no step.
    // At 30 the fall is -6.67 + 4.25 per update; at 60 it is -3.33 + 4.25, so the character
    // rises and the spin, which lasts until landing, keeps going. vinsertps $0x1d, 0x50(%r12),
    // %xmm0, %xmm0 (lane 1 = the boost, other lanes zero; the vaddps after it adds it) becomes
    // a call to the routine at 0xb0, which builds the same vector with the boost x 0.5.
    {0x9dc827, 8, {0xc4, 0xc3, 0x79, 0x21, 0x44, 0x24, 0x50, 0x1d}, Kind::Call, 0xb0},
    // Side-step (0x9d7630, step kept in xmm3): speed += step x accel (correct), then the speed
    // is capped at step x Step+0x18, the ground dash cap's defect: at 60 the dodge's top speed
    // and distance are half. vmulss 0x18(%rdx), %xmm3, %xmm2 calls the routine at 0xd0, which
    // multiplies by 1/30 into xmm2; xmm3 (the step) is still added to the timer after it.
    {0x9d7988, 5, {0xc5, 0xe2, 0x59, 0x52, 0x18}, Kind::Call, 0xd0},
    // Fall speed caps of the jump posture (0x9d5216) and the attack posture (0x9dc7a3): after
    // gravity the downward speed is capped at step x P, with xmm1 = the step and xmm0 = -P
    // (vxorps of a sign mask), used as step x xmm0 for the cap and the clamped vector; at 60
    // jumps fall at half speed (measured: top fall speed 16.7 vs 33.3, fall 0.70 s vs 0.43 s).
    // The vxorps calls the routine at 0xe0: the same negation (both masks are 0x80000000 x 4;
    // it uses 0x115cca0), then xmm0 x (1/30) / xmm1, so both products become P / 30. xmm1 = 0
    // (a clamped first step) keeps the original. xmm5 is overwritten after it on both paths.
    {0x9d5216, 8, {0xc5, 0xf8, 0x57, 0x05, 0x82, 0x7a, 0x78, 0x00}, Kind::Call, 0xe0},
    {0x9dc7a3, 8, {0xc5, 0xf8, 0x57, 0x05, 0x95, 0x08, 0x78, 0x00}, Kind::Call, 0xe0},
    // Motion track node (0x1dbad0, event time at -0x34(%rbp)): it looks up the motion key at
    // (int)time and starts the key's motion when it finds one, with no check that the frame was
    // already handled. At 60 the times N and N + 0.5 both truncate to N, so every motion key
    // starts twice and the second start resets the character physics pose (GOCCharacterPhysics
    // +0xbb0): a frozen physics frame and a hair and skirt jump at every shot cut (measured:
    // two starts per character per cut at 60, one at 30). call *0x228(%rax) (the start) calls
    // the routine at MotionCave, which makes the call only at whole 1/30 s times; its result is
    // unused (jmp 0x1dbcaf, which reloads rdi and rax).
    {0x1dbc6a, 6, {0xff, 0x90, 0x28, 0x02, 0x00, 0x00}, Kind::Call, 0, MotionCave},
    // Motion node clip time (0x1dbd50, node in r12): it sends +0x178 to the actor's body clip
    // (call *0x38 at 0x1dbf3a, which sets the clip's time) and then adds the step to it; a clip
    // that has ended is replaced only on a whole 1/30 s frame. At 60 the half step after the end
    // sends end + 1/60, and the looping clip wraps to its first frame for that update (measured:
    // Sakura's 2 s clip at event 578.5, a 107 degree arm pop and the hair flick after it; one
    // wrong pose, correct at 578 and 579). "xor %esi, %esi; mov (%rdi), %rax" before the call
    // calls the routine at NodeCave, which sends the clip's end instead while the time is less
    // than a 1/30 s frame past it, then does the two instructions.
    {0x1dbf31, 5, {0x31, 0xf6, 0x48, 0x8b, 0x07}, Kind::Call, 0, NodeCave},
    // Additive upper-body event tracks (vtable 0x1618a48): the apply (0x1e5430) samples a second
    // clip at (event time - track start) / [settings + 0x1c] + offset and multiplies it onto the
    // bones Spine..Head_root; the event time counts 1/30 s frames, so at 60 the layer plays at
    // half speed. The upper body drifts from the 30 fps pose (measured: 3-19 degrees at the
    // head), and each shot cut's baked physics pose (the 30 fps one) snaps it back for one update
    // (the hair whips in group shots). The offset (0x1e4cc0, +0x3c / fps) likewise. mov [rip ->
    // settings slot], %rax / %rcx becomes lea (DashCave + 8 - 0x1c)(%rip), so the following
    // vmovss 0x1c reads 30.0; the register is dead after it.
    {0x1e5478, 7, {0x48, 0x8b, 0x05, 0x51, 0xec, 0xab, 0x02}, Kind::Fps, 0},
    {0x1e4f59, 7, {0x48, 0x8b, 0x0d, 0x70, 0xf1, 0xab, 0x02}, Kind::Fps, 0},
    // Event motion tracks (vtable 0x161cb80): the start (0x221c10) and its two helpers (0x224c00,
    // 0x224db0) start the actor's motion at (event time - track start) / [settings + 0x1c] into
    // the clip, the event time counted in 1/30 s; at 60 the start offset is halved. Same load
    // swap as the upper-body layer; rax is dead after the read on every path. The last load
    // (0x2252bf) crosses a 16-byte block, so its read (vmovss 0x1c(%rax), %xmm0) calls the
    // routine's 30.0 load instead.
    {0x221d27, 7, {0x48, 0x8b, 0x05, 0xa2, 0x23, 0xa8, 0x02}, Kind::Fps, 0},
    {0x224c38, 7, {0x48, 0x8b, 0x05, 0x91, 0xf4, 0xa7, 0x02}, Kind::Fps, 0},
    {0x224c97, 7, {0x48, 0x8b, 0x05, 0x32, 0xf4, 0xa7, 0x02}, Kind::Fps, 0},
    {0x2252d6, 5, {0xc5, 0xfa, 0x10, 0x40, 0x1c}, Kind::Call, 0xc6},
}};
constexpr std::uintptr_t PageSize = 0x1000;

std::int32_t Rel(std::uintptr_t target, std::uintptr_t next) {
    return static_cast<std::int32_t>(static_cast<std::int64_t>(target) -
                                     static_cast<std::int64_t>(next));
}

// Call: call routine + entry, nop-padded to the instruction's length. Lea: lea
// (DashCave - 0x34)(%rip), %rax. Load: vmovss DashCave(%rip), %xmm1. Jump: jmp DashCave +
// entry, nop-padded. Fps: lea (DashCave + 8 - 0x1c)(%rip) into the original mov's register.
std::array<std::uint8_t, 10> Fixed(const Site& site) {
    std::array<std::uint8_t, 10> code;
    code.fill(0x90);
    std::int32_t rel;
    if (site.kind == Kind::Lea) {
        code[0] = 0x48, code[1] = 0x8d, code[2] = 0x05;
        rel = Rel(DashCave - 0x34, site.offset + 7);
        std::memcpy(code.data() + 3, &rel, 4);
    } else if (site.kind == Kind::Fps) {
        code[0] = 0x48, code[1] = 0x8d, code[2] = site.original[2];
        rel = Rel(DashCave + 8 - 0x1c, site.offset + 7);
        std::memcpy(code.data() + 3, &rel, 4);
    } else if (site.kind == Kind::Load) {
        code[0] = 0xc5, code[1] = 0xfa, code[2] = 0x10, code[3] = 0x0d;
        rel = Rel(DashCave, site.offset + 8);
        std::memcpy(code.data() + 4, &rel, 4);
    } else {
        code[0] = site.kind == Kind::Call ? 0xe8 : 0xe9;
        rel = Rel(site.routine + site.entry, site.offset + 5);
        std::memcpy(code.data() + 1, &rel, 4);
    }
    return code;
}

bool Holds(const std::uint8_t* image, const Site& site, const std::array<std::uint8_t, 10>& code) {
    return std::memcmp(image + site.offset, code.data(), site.length) == 0;
}

bool Protect(std::uint8_t* page, std::size_t size, bool writable) {
#ifdef _WIN32
    (void)page, (void)size, (void)writable;
    return false;
#else
    return mprotect(page, size, PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0)) == 0;
#endif
}

// Replaces the site's bytes inside its aligned 16-byte block in one locked exchange.
bool Exchange(std::uint8_t* image, const Site& site, const std::array<std::uint8_t, 10>& from,
              const std::array<std::uint8_t, 10>& to) {
    auto* block = image + (site.offset & ~std::uintptr_t{15});
    const std::size_t at = site.offset & 15;
    if (at + site.length > 16) return false;
    std::array<std::uint8_t, 16> expected, desired;
    std::memcpy(expected.data(), block, 16);
    std::memcpy(expected.data() + at, from.data(), site.length);
    desired = expected;
    std::memcpy(desired.data() + at, to.data(), site.length);
    std::uint64_t lo, hi, new_lo, new_hi;
    std::memcpy(&lo, expected.data(), 8);
    std::memcpy(&hi, expected.data() + 8, 8);
    std::memcpy(&new_lo, desired.data(), 8);
    std::memcpy(&new_hi, desired.data() + 8, 8);
    bool exchanged;
    asm volatile("lock cmpxchg16b %1"
                 : "=@ccz"(exchanged), "+m"(*reinterpret_cast<unsigned __int128*>(block)),
                   "+a"(lo), "+d"(hi)
                 : "b"(new_lo), "c"(new_hi)
                 : "memory");
    return exchanged;
}
} // namespace

DashFix DashFixState(const std::uint8_t* image) {
    bool original = true, fixed = true;
    for (const auto& site : Sites) {
        original = original && Holds(image, site, site.original);
        fixed = fixed && Holds(image, site, Fixed(site));
    }
    const bool routine =
        std::memcmp(image + DashCave, CaveCode.data(), CaveCode.size()) == 0 &&
        std::memcmp(image + MotionCave, MotionCode.data(), MotionCode.size()) == 0 &&
        std::memcmp(image + NodeCave, NodeCode.data(), NodeCode.size()) == 0;
    if (fixed && routine) return DashFix::Fixed;
    if (original) return DashFix::Original;
    return DashFix::Unknown;
}

bool SetDashFix(std::uint8_t* image, bool on) {
    const DashFix state = DashFixState(image);
    if (state == DashFix::Unknown) return false;
    if (state == (on ? DashFix::Fixed : DashFix::Original)) return true;
    // The routines go into zero padding only (the segment ends before them); written once.
    const auto zero = [&](std::uintptr_t at, std::size_t n) {
        return std::all_of(image + at, image + at + n, [](std::uint8_t b) { return b == 0; });
    };
    const bool routine =
        std::memcmp(image + DashCave, CaveCode.data(), CaveCode.size()) == 0;
    const bool motion =
        std::memcmp(image + MotionCave, MotionCode.data(), MotionCode.size()) == 0;
    const bool node = std::memcmp(image + NodeCave, NodeCode.data(), NodeCode.size()) == 0;
    if (on && ((!routine && !zero(DashCave, DashCaveSize)) ||
               (!motion && !zero(MotionCave, MotionCaveSize)) ||
               (!node && !zero(NodeCave, NodeCaveSize))))
        return false;
    // The routines share the code segment's last page.
    static_assert((DashCave & ~(PageSize - 1)) == (MotionCave & ~(PageSize - 1)));
    static_assert((DashCave & ~(PageSize - 1)) == (NodeCave & ~(PageSize - 1)));
    static_assert(MotionCave + MotionCaveSize <= NodeCave && NodeCave + NodeCaveSize <= DashCave);
    auto* cave_page = image + (DashCave & ~(PageSize - 1));
    if (on && (!routine || !motion || !node)) {
        if (!Protect(cave_page, PageSize, true)) return false;
        if (!routine) std::memcpy(image + DashCave, CaveCode.data(), CaveCode.size());
        if (!motion) std::memcpy(image + MotionCave, MotionCode.data(), MotionCode.size());
        if (!node) std::memcpy(image + NodeCave, NodeCode.data(), NodeCode.size());
        Protect(cave_page, PageSize, false);
    }
    bool ok = true;
    for (const auto& site : Sites) {
        auto* page = image + (site.offset & ~(PageSize - 1));
        if (!ok || !Protect(page, PageSize, true)) return false;
        const auto fixed = Fixed(site);
        ok = Exchange(image, site, on ? site.original : fixed, on ? fixed : site.original);
        Protect(page, PageSize, false);
    }
    return ok;
}
} // namespace Sakura

void OnFlip(std::uintptr_t image_base, std::uintptr_t image_size) {
    auto& s = Ps4RtSettings::Get();
    static const bool sakura = [&] {
        if (!image_base || image_size < Sakura::SettingsSlot + 8 ||
            !Sakura::CodeMatches({reinterpret_cast<const std::uint8_t*>(image_base + Sakura::LoopCode),
                                  0x2c}))
            return false;
        s.framerate_max = 60;
        std::printf("Framerate: game frame rate hook available (30, 60)\n");
        return true;
    }();
    if (!sakura) return;
    std::uintptr_t object;
    std::memcpy(&object, reinterpret_cast<const void*>(image_base + Sakura::SettingsSlot), 8);
    if (object < 0x100000 || object % 8) return; // not set up yet
    static Sakura::State state;
    const int fps = std::min(s.framerate.load(), s.framerate_max.load());
    static bool reported = false;
    const bool applied = Sakura::Apply(reinterpret_cast<std::uint8_t*>(object), fps, state);
    if (!applied && !reported) {
        reported = true;
        std::printf("Framerate: game settings hold unexpected values; frame rate left to the game\n");
    }
    // Dash, jump, fall, side-step, cut-in, event track and air attack fix only while the hook has
    // the game above 30; changed only when that does.
    static int dash = 30;
    const int want = state.applied;
    if (want != dash) {
        dash = want;
        auto* image = reinterpret_cast<std::uint8_t*>(image_base);
        const bool ok = image_size > Sakura::DashCave + Sakura::DashCaveSize &&
                        Sakura::SetDashFix(image, want > 30);
        std::printf("Framerate: dash, jump, fall, side-step, cut-in, event track, motion key, clip end, upper-body layer, event motion and air attack fix %s\n",
                    !ok ? "not applied (unexpected code bytes)" : want > 30 ? "on" : "off");
    }
}

} // namespace Ps4RtFramerate
