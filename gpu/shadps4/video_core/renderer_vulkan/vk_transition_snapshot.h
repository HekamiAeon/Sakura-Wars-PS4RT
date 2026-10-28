// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {
class Instance;
class Scheduler;

// Independently owned color; guest alpha remains live. Used only by the explicit
// transition policy inherited from the template runtime, never registered in the texture
// cache.
class TransitionSnapshot {
public:
    TransitionSnapshot(const Instance& instance, Scheduler& scheduler,
                       const VideoCore::ImageInfo& info, vk::Image source);
    ~TransitionSnapshot();
    void UpdateAlpha(vk::DescriptorBufferInfo linear_guest, u32 pitch);
    vk::ImageView View(const VideoCore::ImageViewInfo& view);
    vk::Image Image() const { return image.image; }
    const VideoCore::ImageInfo info;

private:
    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::UniqueImage image;
    vk::UniqueImageView storage_view;
    std::vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>> views;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};
} // namespace Vulkan
