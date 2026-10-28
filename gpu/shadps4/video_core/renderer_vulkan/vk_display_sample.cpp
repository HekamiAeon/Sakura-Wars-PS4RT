// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_display_sample.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
namespace {
void CopyDisplay(Scheduler& scheduler, vk::Image source, vk::Extent2D source_size,
                 vk::Image destination, vk::Extent2D native_size,
                 vk::ImageLayout old_layout, vk::ImageLayout final_layout,
                 vk::AccessFlags2 final_access) {
    scheduler.EndRendering();
    const auto cmd = scheduler.CommandBuffer();
    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout, vk::ImageLayout layout,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage, .srcAccessMask = src_access,
            .dstStageMask = dst_stage, .dstAccessMask = dst_access,
            .oldLayout = old_layout, .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
    barrier(source, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
        all, vk::AccessFlagBits2::eMemoryWrite, transfer, vk::AccessFlagBits2::eTransferRead);
    barrier(destination, old_layout,
        vk::ImageLayout::eTransferDstOptimal, all,
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        transfer, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageBlit blit{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
        .srcOffsets = std::array<vk::Offset3D,2>{{{0,0,0},{s32(source_size.width),s32(source_size.height),1}}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
        .dstOffsets = std::array<vk::Offset3D,2>{{{0,0,0},{s32(native_size.width),s32(native_size.height),1}}}};
    cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, destination,
        vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
    barrier(source, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
        transfer, vk::AccessFlagBits2::eTransferRead, all,
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
    barrier(destination, vk::ImageLayout::eTransferDstOptimal, final_layout,
        transfer, vk::AccessFlagBits2::eTransferWrite, all, final_access);
}
} // namespace

void CopyDisplayForSampling(Scheduler& scheduler, vk::Image source, vk::Extent2D source_size,
                            vk::Image destination, vk::Extent2D native_size, bool initialized) {
    CopyDisplay(scheduler, source, source_size, destination, native_size,
        initialized ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
        vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead);
}

void CopyDisplayForRendering(Scheduler& scheduler, vk::Image source, vk::Extent2D source_size,
                             vk::Image destination, vk::Extent2D native_size,
                             vk::ImageLayout destination_layout) {
    CopyDisplay(scheduler, source, source_size, destination, native_size,
        destination_layout, destination_layout,
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
}
} // namespace Vulkan
