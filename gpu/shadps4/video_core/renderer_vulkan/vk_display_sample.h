// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Scheduler;
// GPU-only native-size resolve for guest reads of a redirected display. Both
// images leave General; source enters General, destination is new or General.
void CopyDisplayForSampling(Scheduler& scheduler, vk::Image source, vk::Extent2D source_size,
                            vk::Image destination, vk::Extent2D native_size, bool initialized);
// Restore the native attachment before handing a redirected display back to the
// game. Its tracked layout is restored; subsequent loads/blends see the last
// actual display contents. A following game clear still replaces those contents.
void CopyDisplayForRendering(Scheduler& scheduler, vk::Image source, vk::Extent2D source_size,
                             vk::Image destination, vk::Extent2D native_size,
                             vk::ImageLayout destination_layout);
} // namespace Vulkan
