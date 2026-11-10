// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_screenshot.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace Ps4RtScreenshot {

namespace {
std::atomic<unsigned> requests{0};
}

void Request() {
    requests.fetch_add(1, std::memory_order_relaxed);
}

unsigned Consume() {
    static const char* const trigger = [] {
        const char* path = std::getenv("PS4RT_SCREENSHOT_TRIGGER");
        return path && *path ? path : nullptr;
    }();
    if (trigger && std::remove(trigger) == 0) {
        Request();
    }
    return requests.exchange(0, std::memory_order_acq_rel);
}

} // namespace Ps4RtScreenshot
