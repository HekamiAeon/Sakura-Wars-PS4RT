// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_screenshot.h"

#include <atomic>

namespace Ps4RtScreenshot {

namespace {
std::atomic<unsigned> requests{0};
}

void Request() {
    requests.fetch_add(1, std::memory_order_relaxed);
}

unsigned Consume() {
    return requests.exchange(0, std::memory_order_acq_rel);
}

} // namespace Ps4RtScreenshot
