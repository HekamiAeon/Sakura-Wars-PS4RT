// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_transition_snapshot.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/transition_alpha_comp.h"

namespace Vulkan {
namespace {
constexpr vk::ImageSubresourceRange Range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
void Barrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout old_layout,
             vk::ImageLayout new_layout, vk::PipelineStageFlags2 src_stage,
             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
             vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask=src_stage,.srcAccessMask=src_access,
        .dstStageMask=dst_stage,.dstAccessMask=dst_access,
        .oldLayout=old_layout,.newLayout=new_layout,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,.subresourceRange=Range};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
}
}

TransitionSnapshot::TransitionSnapshot(const Instance& instance_, Scheduler& scheduler_,
                                       const VideoCore::ImageInfo& info_, vk::Image source)
    : info(info_), instance(instance_), scheduler(scheduler_),
      image(instance.GetDevice(),instance.GetAllocator()) {
    ASSERT(info.pixel_format==vk::Format::eR8G8B8A8Unorm && info.num_samples==1);
    const auto device=instance.GetDevice();
    image.Create({.imageType=vk::ImageType::e2D,.format=info.pixel_format,
        .extent={info.size.width,info.size.height,1},.mipLevels=1,.arrayLayers=1,
        .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc|
               vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eStorage});
    storage_view=Check(device.createImageViewUnique({.image=image.image,
        .viewType=vk::ImageViewType::e2D,.format=info.pixel_format,.subresourceRange=Range}));
    const std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,
         .stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=1,.descriptorType=vk::DescriptorType::eStorageImage,.descriptorCount=1,
         .stageFlags=vk::ShaderStageFlagBits::eCompute}}};
    descriptors=Check(device.createDescriptorSetLayoutUnique({
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=2,.pBindings=bindings.data()}));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,12};
    layout=Check(device.createPipelineLayoutUnique({.setLayoutCount=1,
        .pSetLayouts=&*descriptors,.pushConstantRangeCount=1,.pPushConstantRanges=&push}));
    const auto module=CompileSPV(TRANSITION_ALPHA_COMP,device);
    pipeline=Check(device.createComputePipelineUnique({},vk::ComputePipelineCreateInfo{
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=module,.pName="main"},
        .layout=*layout}));
    device.destroyShaderModule(module);
    scheduler.EndRendering();
    const auto cmd=scheduler.CommandBuffer();
    Barrier(cmd,image.image,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,
        vk::PipelineStageFlagBits2::eNone,{},vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite);
    // Caller transitions the completed producer through Runtime before copying.
    cmd.copyImage(source,vk::ImageLayout::eTransferSrcOptimal,image.image,
        vk::ImageLayout::eTransferDstOptimal,vk::ImageCopy{
            .srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .extent={info.size.width,info.size.height,1}});
    Barrier(cmd,image.image,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral,
        vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
        vk::PipelineStageFlagBits2::eAllCommands,
        vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite);
}

TransitionSnapshot::~TransitionSnapshot() = default;

void TransitionSnapshot::UpdateAlpha(vk::DescriptorBufferInfo linear_guest,u32 pitch) {
    scheduler.EndRendering();
    const auto cmd=scheduler.CommandBuffer();
    Barrier(cmd,image.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eGeneral,
        vk::PipelineStageFlagBits2::eAllCommands,
        vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite);
    const vk::DescriptorImageInfo output{.imageView=*storage_view,.imageLayout=vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet,2> writes{{
        {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,
         .pBufferInfo=&linear_guest},
        {.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,
         .pImageInfo=&output}}};
    const std::array<u32,3> push{info.size.width,info.size.height,pitch};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes);
    cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(push),push.data());
    cmd.dispatch((info.size.width+7)/8,(info.size.height+7)/8,1);
    Barrier(cmd,image.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eGeneral,
        vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,
        vk::PipelineStageFlagBits2::eAllCommands,vk::AccessFlagBits2::eShaderRead);
}

vk::ImageView TransitionSnapshot::View(const VideoCore::ImageViewInfo& view) {
    for (const auto& [key,handle]:views) if(key==view) return *handle;
    if(views.size()>=4) return {}; // fail open to original binding, never unbounded view growth
    auto handle=Check(instance.GetDevice().createImageViewUnique({.image=image.image,
        .viewType=vk::ImageViewType::e2D,.format=view.format,.components=view.mapping,
        .subresourceRange=Range}));
    const auto result=*handle;
    views.emplace_back(view,std::move(handle));
    return result;
}
} // namespace Vulkan
