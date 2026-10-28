// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "ps4rt_menu_input.h"

int main() {
    using namespace Ps4RtMenuInput;
    Controllers pads;
    // Virtual-pad trigger and physical-pad stick must never form a shortcut.
    assert(!pads.Trigger(1, 32767));
    assert(!pads.RightStick(2, true));
    assert(pads.Trigger(2, 32767));
    assert(!pads.RightStick(2, true));
    assert(!pads.Trigger(2, 15000));
    assert(!pads.Trigger(2, 10000));
    assert(!pads.Trigger(2, 32767)); // trigger jitter cannot toggle twice
    assert(!pads.RightStick(2, false));
    assert(pads.RightStick(2, true)); // intentional second stick click works
    pads.Focus(2);
    assert(!pads.Accept(1) && pads.Accept(2));
    pads.Release();
    assert(!pads.Accept(1, false)); // neutral motion must not take ownership
    assert(pads.Accept(2) && !pads.Accept(1));
    pads.Remove(2);
    assert(pads.Accept(1)); // unplugging the owner permits controller handoff
    assert(!pads.Trigger(2, 32767)); // removed device has no stale stick state
    for (int value : {-9000, -2000, 0, 2000, 9000}) {
        assert(Axis(value, true) == 0 && Axis(value, false) == 0);
    }
    assert(Axis(-32768, false) == 1 && Axis(32767, true) == 1);
    assert(Axis(-32768, true) == 0 && Axis(32767, false) == 0);
    assert(Axis(16000, true) > 0 && Axis(16000, true) < 1);
    std::puts("Menu input: per-controller shortcut, repeat latch, ownership, stick dead zone PASS");
}
