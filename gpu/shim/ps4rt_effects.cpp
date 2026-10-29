// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_effects.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "ps4rt_settings.h"

namespace Ps4RtEffects {
namespace Sakura {
namespace {
constexpr std::uintptr_t Entry = 0x10;
// The start of 0xdad540: push %rbp; mov %rsp, %rbp; push %r15; push %r14; push %r13; push %r12.
constexpr std::array<std::uint8_t, 12> ApplyCode = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                    0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
// At Cave + Entry (assembled at that address; see ps4rt_effects.h): push %rbx; push %r12;
// push %r13; rbx = rsi; r12d = the bytes at +0x2b0, +0x670, +0x8b0, +0x900, r13d = those at
// +0x3d0, +0x400, +0xd20; for each, the byte at Cave + i unless it is 0xff; call 0xdad540; the
// bytes back from r12d and r13d; pop %r13; pop %r12; pop %rbx; ret (rax, 0xdad540's result,
// untouched).
constexpr std::array<std::uint8_t, 0x119> Routine = {
    0x53, 0x41, 0x54, 0x41, 0x55, 0x48, 0x89, 0xf3, 0x44, 0x0f, 0xb6, 0xa3, 0xb0, 0x02, 0x00,
    0x00, 0x0f, 0xb6, 0x83, 0x70, 0x06, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x41, 0x09, 0xc4, 0x0f,
    0xb6, 0x83, 0xb0, 0x08, 0x00, 0x00, 0xc1, 0xe0, 0x10, 0x41, 0x09, 0xc4, 0x0f, 0xb6, 0x83,
    0x00, 0x09, 0x00, 0x00, 0xc1, 0xe0, 0x18, 0x41, 0x09, 0xc4, 0x44, 0x0f, 0xb6, 0xab, 0xd0,
    0x03, 0x00, 0x00, 0x0f, 0xb6, 0x83, 0x00, 0x04, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x41, 0x09,
    0xc5, 0x0f, 0xb6, 0x83, 0x20, 0x0d, 0x00, 0x00, 0xc1, 0xe0, 0x10, 0x41, 0x09, 0xc5, 0x8a,
    0x05, 0x91, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06, 0x88, 0x83, 0xb0, 0x02, 0x00, 0x00,
    0x8a, 0x05, 0x82, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06, 0x88, 0x83, 0x70, 0x06, 0x00,
    0x00, 0x8a, 0x05, 0x73, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06, 0x88, 0x83, 0xb0, 0x08,
    0x00, 0x00, 0x8a, 0x05, 0x64, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06, 0x88, 0x83, 0x00,
    0x09, 0x00, 0x00, 0x8a, 0x05, 0x55, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06, 0x88, 0x83,
    0xd0, 0x03, 0x00, 0x00, 0x8a, 0x05, 0x46, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06, 0x88,
    0x83, 0x00, 0x04, 0x00, 0x00, 0x8a, 0x05, 0x37, 0xff, 0xff, 0xff, 0x3c, 0xff, 0x74, 0x06,
    0x88, 0x83, 0x20, 0x0d, 0x00, 0x00, 0xe8, 0x62, 0xa8, 0x98, 0xff, 0x44, 0x88, 0xa3, 0xb0,
    0x02, 0x00, 0x00, 0x41, 0xc1, 0xec, 0x08, 0x44, 0x88, 0xa3, 0x70, 0x06, 0x00, 0x00, 0x41,
    0xc1, 0xec, 0x08, 0x44, 0x88, 0xa3, 0xb0, 0x08, 0x00, 0x00, 0x41, 0xc1, 0xec, 0x08, 0x44,
    0x88, 0xa3, 0x00, 0x09, 0x00, 0x00, 0x44, 0x88, 0xab, 0xd0, 0x03, 0x00, 0x00, 0x41, 0xc1,
    0xed, 0x08, 0x44, 0x88, 0xab, 0x00, 0x04, 0x00, 0x00, 0x41, 0xc1, 0xed, 0x08, 0x44, 0x88,
    0xab, 0x20, 0x0d, 0x00, 0x00, 0x41, 0x5d, 0x41, 0x5c, 0x5b, 0xc3};
static_assert(Entry + Routine.size() <= CaveSize);
constexpr std::uintptr_t PageSize = 0x1000;

bool Protect(std::uint8_t* page, int prot) {
#ifdef _WIN32
    (void)page, (void)prot;
    return false;
#else
    return mprotect(page, PageSize, prot) == 0;
#endif
}
std::uint64_t ReadSlot(const std::uint8_t* image) {
    std::uint64_t v;
    std::memcpy(&v, image + Slot, 8);
    return v;
}
// The slot is 8-byte aligned: one store swaps it for every reader.
bool WriteSlot(std::uint8_t* image, std::uint64_t value) {
    auto* page = image + (Slot & ~(PageSize - 1));
    if (!Protect(page, PROT_READ | PROT_WRITE)) return false;
    __atomic_store_n(reinterpret_cast<std::uint64_t*>(image + Slot), value, __ATOMIC_SEQ_CST);
    Protect(page, PROT_READ);
    return true;
}
} // namespace

std::uint64_t PresetBytes(int preset) {
    // bloom, occlusionCapsule, antialiasing type, edge, dof, dof rtScale, character screen-space
    // shadow, unused (low byte first); 0xff keeps the game's.
    switch (preset) {
    case High: return 0xffffffffffff0000;   // contact shadows and bloom off
    case Medium: return 0xff0002ffff020000; // and FXAA, depth of field at quarter resolution,
                                            // no character screen-space shadow
    case Lowest: return 0xff00ff0000000000; // and no anti-aliasing, outlines and depth of field
    default: return ~std::uint64_t{0};
    }
}

Hook HookState(const std::uint8_t* image) {
    const auto base = reinterpret_cast<std::uint64_t>(image);
    if (std::memcmp(image + Apply, ApplyCode.data(), ApplyCode.size()) != 0) return Hook::Unknown;
    const bool routine = std::memcmp(image + Cave + Entry, Routine.data(), Routine.size()) == 0;
    if (ReadSlot(image) == base + Apply &&
        (routine || std::all_of(image + Cave, image + Cave + CaveSize,
                                [](std::uint8_t b) { return b == 0; })))
        return Hook::Original;
    if (ReadSlot(image) == base + Cave + Entry && routine) return Hook::Patched;
    return Hook::Unknown;
}

bool SetPreset(std::uint8_t* image, int preset) {
    const Hook state = HookState(image);
    if (state == Hook::Unknown || preset < Max || preset > Lowest) return false;
    const auto base = reinterpret_cast<std::uint64_t>(image);
    if (preset == Max) return state == Hook::Original || WriteSlot(image, base + Apply);
    static_assert((Cave & ~(PageSize - 1)) == ((Cave + CaveSize - 1) & ~(PageSize - 1)));
    auto* cave_page = image + (Cave & ~(PageSize - 1));
    if (!Protect(cave_page, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    if (std::memcmp(image + Cave + Entry, Routine.data(), Routine.size()) != 0)
        std::memcpy(image + Cave + Entry, Routine.data(), Routine.size());
    __atomic_store_n(reinterpret_cast<std::uint64_t*>(image + Cave), PresetBytes(preset),
                     __ATOMIC_SEQ_CST);
    Protect(cave_page, PROT_READ | PROT_EXEC);
    return state == Hook::Patched || WriteSlot(image, base + Cave + Entry);
}
} // namespace Sakura

void OnFlip(std::uintptr_t image_base, std::uintptr_t image_size) {
    auto& s = Ps4RtSettings::Get();
    static const bool sakura = [&] {
        if (!image_base || image_size < Sakura::Slot + 8 ||
            Sakura::HookState(reinterpret_cast<const std::uint8_t*>(image_base)) ==
                Sakura::Hook::Unknown)
            return false;
        s.graphics_preset_available = true;
        std::printf("Effects: graphics preset hook available\n");
        return true;
    }();
    if (!sakura) return;
    static int applied = Max;
    const int want = std::clamp(s.graphics_preset.load(), int(Max), int(Lowest));
    if (want == applied) return;
    applied = want;
    static const char* const names[] = {"Max", "High", "Medium", "Lowest"};
    if (Sakura::SetPreset(reinterpret_cast<std::uint8_t*>(image_base), want))
        std::printf("Effects: graphics preset %s\n", names[want]);
    else
        std::printf("Effects: graphics preset not applied (unexpected code bytes)\n");
}

} // namespace Ps4RtEffects
