// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <memory>
#include "video_core/renderer_vulkan/vk_transition_snapshot.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/ps4rt_transition.h"
#include <vk_mem_alloc.h>

int main() {
    using namespace VideoCore;
    using namespace Vulkan;
    ImageInfo policy{};
    policy.type=AmdGpu::ImageType::Color2D; policy.size={1920,1080,1};
    policy.resources={1,1}; policy.num_samples=policy.guest_samples=1;
    policy.num_bits=32; policy.pixel_format=vk::Format::eR8G8B8A8Unorm;
    policy.props.is_tiled=true; policy.tile_mode=AmdGpu::TileMode::Thin1DThin;
    policy.array_mode=AmdGpu::ArrayMode::Array1DTiledThin1; policy.pitch=1920;
    policy.guest_address=0x10000; policy.guest_size=1920*1080*4;
    policy.mips_layout[0]={policy.guest_size,1920,1080,0};
    assert(Ps4RtTransition::Supported(policy));
    auto wrong=policy;wrong.pixel_format=vk::Format::eR8G8B8A8Srgb;
    assert(!Ps4RtTransition::Supported(wrong));
    wrong=policy;wrong.tile_mode=AmdGpu::TileMode::Display2DThin;
    assert(!Ps4RtTransition::Supported(wrong));
    wrong=policy;wrong.num_samples=4;assert(!Ps4RtTransition::Supported(wrong));
    wrong=policy;wrong.size.width=1280;assert(!Ps4RtTransition::Supported(wrong));
    Ps4RtGpuWorkarounds::Configure("",false,false,true);
    assert(Ps4RtTransition::Consumer(0x06ffbe54));
    assert(Ps4RtTransition::Consumer(0x50a37319));
    assert(!Ps4RtTransition::Consumer(0x13d64dc6));
    Ps4RtGpuWorkarounds::Configure("",false,false,false);
    assert(!Ps4RtTransition::Consumer(0x06ffbe54));
    Ps4RtGpuWorkarounds::Configure("");
    assert(!Ps4RtTransition::Consumer(0x06ffbe54));

    Instance instance(0,false); Scheduler scheduler(instance);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());dispatch.init(instance.GetDevice());
    constexpr unsigned Width=31,Height=17,Pitch=40,Bytes=Pitch*Height*4;
    ImageInfo info=policy;info.size={Width,Height,1};info.pitch=Pitch;
    UniqueImage source(instance.GetDevice(),instance.GetAllocator());
    source.Create({.imageType=vk::ImageType::e2D,.format=info.pixel_format,
        .extent={Width,Height,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
        .tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst});
    VkBuffer upload{},readback{}; VmaAllocation ua{},ra{}; VmaAllocationInfo ui{},ri{};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    const VkBufferCreateInfo ub{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=Bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    const VkBufferCreateInfo rb{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=Bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(),&ub,&ac,&upload,&ua,&ui)==VK_SUCCESS);
    assert(vmaCreateBuffer(instance.GetAllocator(),&rb,&ac,&readback,&ra,&ri)==VK_SUCCESS);
    auto* pixels=static_cast<unsigned char*>(ui.pMappedData);
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Pitch;++x) {
        const auto i=(y*Pitch+x)*4;
        pixels[i]=x*7;pixels[i+1]=y*11;pixels[i+2]=(x+y)*5;pixels[i+3]=255;
    }
    assert(vmaFlushAllocation(instance.GetAllocator(),ua,0,VK_WHOLE_SIZE)==VK_SUCCESS);
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
    const auto transition=[&](vk::Image image,vk::ImageLayout old_l,vk::ImageLayout new_l) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=old_l,.newLayout=new_l,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
        scheduler.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b},dispatch);
    };
    const vk::BufferImageCopy copy{.bufferRowLength=Pitch,.bufferImageHeight=Height,
        .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={Width,Height,1}};
    transition(source.image,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
    scheduler.CommandBuffer().copyBufferToImage(upload,source.image,vk::ImageLayout::eTransferDstOptimal,copy,dispatch);
    transition(source.image,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eTransferSrcOptimal);
    auto snapshot=std::make_shared<TransitionSnapshot>(instance,scheduler,info,source.image);
    // Destroy/recycle the original after capture. No original backing may supply RGB.
    transition(source.image,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eTransferDstOptimal);
    scheduler.CommandBuffer().clearColorImage(source.image,vk::ImageLayout::eTransferDstOptimal,
        vk::ClearColorValue(std::array<float,4>{1,0,1,0}),range,dispatch);
    scheduler.Finish(); source.Destroy();
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Pitch;++x) {
        const auto i=(y*Pitch+x)*4;
        pixels[i]=255;pixels[i+1]=0;pixels[i+2]=255;pixels[i+3]=(x+y*3)%256;
    }
    assert(vmaFlushAllocation(instance.GetAllocator(),ua,0,VK_WHOLE_SIZE)==VK_SUCCESS);
    snapshot->UpdateAlpha({upload,0,Bytes},Pitch);
    ImageViewInfo vi{};vi.format=info.pixel_format;
    vi.mapping={vk::ComponentSwizzle::eB,vk::ComponentSwizzle::eG,vk::ComponentSwizzle::eR,vk::ComponentSwizzle::eA};
    assert(snapshot->View(vi));assert(snapshot->View(vi)==snapshot->View(vi));
    transition(snapshot->Image(),vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferSrcOptimal);
    scheduler.CommandBuffer().copyImageToBuffer(snapshot->Image(),vk::ImageLayout::eTransferSrcOptimal,readback,copy,dispatch);
    const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask=vk::AccessFlagBits2::eHostRead};
    scheduler.CommandBuffer().pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host},dispatch);
    std::weak_ptr<TransitionSnapshot> weak=snapshot;
    scheduler.DeferOperation([keep=std::move(snapshot)]{});
    assert(!weak.expired());scheduler.Finish();scheduler.PopPendingOperations();assert(weak.expired());
    assert(vmaInvalidateAllocation(instance.GetAllocator(),ra,0,VK_WHOLE_SIZE)==VK_SUCCESS);
    const auto* result=static_cast<const unsigned char*>(ri.pMappedData);
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x) {
        const auto i=(y*Pitch+x)*4;
        assert(result[i]==x*7 && result[i+1]==y*11 && result[i+2]==(x+y)*5);
        assert(result[i+3]==(x+y*3)%256);
    }
    vmaDestroyBuffer(instance.GetAllocator(),upload,ua);vmaDestroyBuffer(instance.GetAllocator(),readback,ra);
    std::puts("PASS: title/layout gates, independent RGB after source destruction, live alpha, deferred lifetime");
}
