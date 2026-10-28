// SPDX-License-Identifier: GPL-2.0-or-later
// ps4rt: the game's own frame rate (menu "Framerate"), set through a per-game hook that changes
// the game's simulation step together with its flip interval, so a higher rate does not speed
// the game up. Games without a hook keep their rate; the menu hides the option.
#pragma once

#include <cstdint>
#include <span>

namespace Ps4RtFramerate {

/// CUSA16429: the run loop (0xe00960) steps the game by 1/fps when its settings object
/// ([image + 0x2ca40d0]) is in fixed-step mode (+0x18), fps at +0x1c (float), and the frame
/// interval at +0x20 (vblanks per flip) drives sceVideoOutSetFlipRate. The game writes 30 fps
/// and interval 2 on every scene setup.
namespace Sakura {
inline constexpr std::uintptr_t SettingsSlot = 0x2ca40d0;
inline constexpr std::uintptr_t LoopCode = 0xe02850;
/// The loop's bytes at LoopCode: the settings load and its fps read (this eboot only).
bool CodeMatches(std::span<const std::uint8_t> code);
/// Last rate written into the settings object; 30 is the game's own (nothing written).
struct State {
    int applied = 30;
};
/// Applies `fps` (30 or 60) to the settings object. 60 is written every call (the game's
/// scene setups write 30 back); 30 restores the game's values once and then leaves them to the
/// game. False when the object does not hold the expected values.
bool Apply(std::uint8_t* settings, int fps, State& state);

/// The mech dash's speed clamp (dash velocity function, 0x9d9978..0x9d9b0e) computes its
/// bounds as step x 1000 and step x 900, values meant per 1/30 s: at 60 fps the dash runs at
/// half speed. The fix calls a small routine in the code segment's unused tail (DashCave) that
/// multiplies the same parameter by 1/30 instead of the live step; the slowdown and lift decay
/// next to it already scale correctly and are left alone. The dash start's lift (0xa2ae9e,
/// 650 x step) has the same defect; there the settings load feeding the multiply is pointed at
/// the routine's 1/30 instead. The jump's thrust test (0x9d5100) gets a 30/fps-scaled limit.
/// Cut-in 2D tracks (the special move's lettering, 0x227934) turn the event time, counted in
/// 1/30 s, into SurfRide frames with the live step and play at half speed at 60; their settings
/// load is pointed at the same 1/30. The ground dash's start velocity (0x9d1363, speed
/// parameter x step) likewise; its per-frame speed cap (0x9d2701) loads the 1/30 directly.
/// The event track tree (0x1d9824): at the half frame before a shot cut the time is in no shot
/// segment, so no track updates; there it is rounded down to the old shot's last 1/30 s frame.
/// The air attack posture (0x9dc827) adds a per-update boost to a falling velocity; it gets
/// the boost x 30/fps. The side-step cap (0x9d7988, step x Step+0x18) gets the 1/30 likewise,
/// and so do the jump and attack postures' fall speed caps (0x9d5216, 0x9dc7a3).
/// Additive upper-body event tracks (0x1e5430) turn the event time, counted in 1/30 s, into clip
/// time by dividing by the live rate and play at half speed at 60; their two settings loads
/// (0x1e5478, 0x1e4f59) are pointed at the routine's 30.0, and so are the event motion tracks'
/// start offsets (0x221c10, 0x224c00, 0x224db0), which have the same division.
inline constexpr std::uintptr_t DashCave = 0x1422f00;
inline constexpr std::uintptr_t DashCaveSize = 0xff;
/// Motion keys (0x1dbc6a): the motion track node starts the key at (int)event time, so at 60
/// each key starts on two updates (N and N + 0.5), and the second start resets the characters'
/// physics pose (hair and skirt jump at every shot cut). The start goes through a routine here
/// that skips it at half-frame times.
inline constexpr std::uintptr_t MotionCave = 0x1422e00;
inline constexpr std::uintptr_t MotionCaveSize = 0x20;
/// Clip ends (0x1dbf31): the cutscene motion node sends its clip time to the body clip and then
/// adds the step; the next clip only switches in on a whole 1/30 s frame. At 60 the half step
/// after a clip's end sends a time just past it, and the looping clip wraps to its first frame
/// for one update (a one-frame pose pop that whips hair). The routine here sends the clip's end
/// instead while the time is less than a 1/30 s frame past it.
inline constexpr std::uintptr_t NodeCave = 0x1422e20;
inline constexpr std::uintptr_t NodeCaveSize = 0x70;
enum class DashFix { Original, Fixed, Unknown };
/// Current state of the clamp, lift, ground dash, jump, fall, side-step, cut-in, event track, motion key, clip end, upper-body layer, event motion and air attack sites and the routines in `image`.
DashFix DashFixState(const std::uint8_t* image);
/// Fixes (`on`) or restores the sites; false (nothing written) when the bytes are not the
/// expected original or fixed ones. Writes the routine once, then each site with one atomic
/// 16-byte exchange, so a thread running the function sees either the old or the new code.
bool SetDashFix(std::uint8_t* image, bool on);
} // namespace Sakura

/// Per flip, on the game's thread: detects the game's hook once (sets the menu's maximum)
/// and applies the selected rate, with the dash, fall, side-step, cut-in, event track and air attack fix above 30.
void OnFlip(std::uintptr_t image_base, std::uintptr_t image_size);

} // namespace Ps4RtFramerate
