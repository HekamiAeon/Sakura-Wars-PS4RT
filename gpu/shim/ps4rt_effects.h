// SPDX-License-Identifier: GPL-2.0-or-later
// ps4rt: graphics presets (menu "Graphics preset": Max, High, Medium, Lowest) that turn the
// game's costlier post effects down or off, a per-game hook for slow GPUs such as the Steam
// Deck. Games without a hook keep their effects; the menu hides the option.
#pragma once

#include <cstdint>

namespace Ps4RtEffects {

/// Ps4RtSettings::Values::graphics_preset.
enum Preset { Max = 0, High = 1, Medium = 2, Lowest = 3 };

/// CUSA16429: the scene's NeedleFxParameter (blended from the area presets every frame) is
/// applied to the renderer by 0xdad540, called through a vtable slot (0x183a868, read-only
/// data). The hook points that slot at a routine in the code segment's unused tail (Cave + 0x10)
/// that saves six switches in the parameter, writes the preset's values, calls 0xdad540 and
/// restores them, so the game's own parameter never changes and presets apply both ways on the
/// next frame. The switches (first byte of each block): bloom +0x2b0, occlusionCapsule +0x670
/// (contact shadows), antialiasing +0x8b0 (AATYPE_NONE 0, TAA 1, FXAA 2, SMAA 3), edge +0x900
/// (outlines pass; the menu's Outlines option only zeroes its constants), dof +0x3d0 and its
/// rtScale +0x400 (RTSIZE_FULL 0, HALF 1, QUARTER 2; battles use half). The preset's bytes
/// are at Cave, 0xff keeping the game's value.
/// Measured on a Radeon 610M in free roam (GPU bound, native 15.8 fps): contact shadows off
/// +0.8, bloom off +0.2, FXAA +1.1, AA off +1.6, outlines off +1.8; Medium 17.9, Lowest 21.4.
/// Battle (about 12 fps with the free roam set off): depth of field off +1.5 to +3, at quarter
/// resolution +0.4; shadow cascades, shadow map size, heat haze, light scattering, chromatic
/// aberration and post ambient no measurable gain.
/// The light field (+0x610) is left alone: off turns foliage black for no gain.
namespace Sakura {
inline constexpr std::uintptr_t Slot = 0x183a868;
inline constexpr std::uintptr_t Apply = 0xdad540;
/// Before the frame rate fixes' routines (ps4rt_framerate.h), in the same page (zero padding
/// from 0x142272c).
inline constexpr std::uintptr_t Cave = 0x1422c00;
inline constexpr std::uintptr_t CaveSize = 0x200; ///< up to MotionCave
/// The override bytes of a preset, as stored at Cave.
std::uint64_t PresetBytes(int preset);
enum class Hook { Original, Patched, Unknown };
Hook HookState(const std::uint8_t* image);
/// Applies `preset`: Max restores the slot; the others write the routine (once) and the preset
/// bytes, then point the slot at the routine. False (nothing written) when the bytes are not the
/// expected ones.
bool SetPreset(std::uint8_t* image, int preset);
} // namespace Sakura

/// Per flip, on the game's thread: detects the hook once (enables the menu option) and applies a
/// changed setting.
void OnFlip(std::uintptr_t image_base, std::uintptr_t image_size);

} // namespace Ps4RtEffects
