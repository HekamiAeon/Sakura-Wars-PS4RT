// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace Ps4RtMenuInput {
// SDL may expose physical and virtual pads together. A shortcut must belong to
// one device; the controller opening the panel owns navigation until it closes.
class Controllers {
    struct Chord { bool r3 = false, l2 = false, armed = true; };
    std::unordered_map<uint32_t, Chord> chords;
    uint32_t owner = 0;
    static bool Toggle(Chord& c) {
        if (!c.r3) c.armed = true;
        if (!c.r3 || !c.l2 || !c.armed) return false;
        c.armed = false;
        return true;
    }
public:
    bool RightStick(uint32_t id, bool down) {
        auto& c = chords[id]; c.r3 = down; return Toggle(c);
    }
    bool Trigger(uint32_t id, int value) {
        auto& c = chords[id];
        c.l2 = value > (c.l2 ? 12000 : 16000); // hysteresis at the shortcut threshold
        return Toggle(c);
    }
    void Focus(uint32_t id) { owner = id; }
    void Release() { owner = 0; }
    bool Accept(uint32_t id, bool active = true) {
        if (!owner && active) owner = id;
        return owner == id;
    }
    void Remove(uint32_t id) { chords.erase(id); if (owner == id) owner = 0; }
};

inline float Axis(int value, bool positive) {
    const int magnitude = positive ? value : -value;
    return std::clamp(float(magnitude - 9000) / (32767.0f - 9000.0f), 0.0f, 1.0f);
}
} // namespace Ps4RtMenuInput
