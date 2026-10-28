// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <array>
#include "video_core/renderer_vulkan/vk_display_sample.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include <vk_mem_alloc.h>

int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    Instance instance(0,false); Scheduler scheduler(instance);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());dispatch.init(instance.GetDevice());
    constexpr unsigned W=31,H=17,SW=W*2,SH=H*2,Bytes=SW*SH*4;
    const VkBufferCreateInfo buffer_info{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=Bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_info{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer upload{},readback{};VmaAllocation ua{},ra{};VmaAllocationInfo ui{},ri{};
    assert(vmaCreateBuffer(instance.GetAllocator(),&buffer_info,&alloc_info,&upload,&ua,&ui)==VK_SUCCESS);
    assert(vmaCreateBuffer(instance.GetAllocator(),&buffer_info,&alloc_info,&readback,&ra,&ri)==VK_SUCCESS);
    auto* bytes=static_cast<unsigned char*>(ui.pMappedData);
    const auto* result=static_cast<const unsigned char*>(ri.pMappedData);
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
    const auto barrier=[&](vk::Image image,vk::ImageLayout from,vk::ImageLayout to) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=from,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
        scheduler.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b},dispatch);
    };
    for(const auto format:{vk::Format::eR8G8B8A8Unorm,vk::Format::eB8G8R8A8Unorm,
                           vk::Format::eR8G8B8A8Srgb,vk::Format::eB8G8R8A8Srgb}) {
        UniqueImage host(instance.GetDevice(),instance.GetAllocator());
        UniqueImage native(instance.GetDevice(),instance.GetAllocator());
        for(auto* image:{&host,&native}) {
            const bool large=image==&host;
            image->Create({.flags=vk::ImageCreateFlagBits::eMutableFormat,.imageType=vk::ImageType::e2D,
                .format=format,.extent={large?SW:W,large?SH:H,1},.mipLevels=1,.arrayLayers=1,
                .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
                .usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst|
                       vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eColorAttachment});
        }
        // Same view contract as the recorded saved-image producer: sRGB storage
        // may be viewed as UNORM, with the game's component mapping retained.
        const auto unorm=(format==vk::Format::eR8G8B8A8Srgb)?vk::Format::eR8G8B8A8Unorm:
                         (format==vk::Format::eB8G8R8A8Srgb)?vk::Format::eB8G8R8A8Unorm:format;
        const auto view=Check(instance.GetDevice().createImageView({.image=native.image,
            .viewType=vk::ImageViewType::e2D,.format=unorm,
            .components={vk::ComponentSwizzle::eB,vk::ComponentSwizzle::eG,
                         vk::ComponentSwizzle::eR,vk::ComponentSwizzle::eA},
            .subresourceRange=range},nullptr,dispatch));
        assert(view);
        const auto attachment_view=Check(instance.GetDevice().createImageView({.image=native.image,
            .viewType=vk::ImageViewType::e2D,.format=format,.subresourceRange=range},nullptr,dispatch));
        for(const bool rendering:{false,true})for(unsigned generation=0;generation<2;++generation) {
            for(unsigned y=0;y<SH;++y)for(unsigned x=0;x<SW;++x) {
                const unsigned i=(y*SW+x)*4,nx=x/2,ny=y/2;
                bytes[i]=(nx*7+generation*29)%256;
                bytes[i+1]=(ny*11+generation*47)%256;
                bytes[i+2]=((nx+ny)*5+generation*61)%256;
                bytes[i+3]=(nx+ny*3+generation*17)%256;
            }
            assert(vmaFlushAllocation(instance.GetAllocator(),ua,0,VK_WHOLE_SIZE)==VK_SUCCESS);
            barrier(host.image,(rendering||generation)?vk::ImageLayout::eGeneral:vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal);
            const vk::BufferImageCopy upload_copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .imageExtent={SW,SH,1}};
            scheduler.CommandBuffer().copyBufferToImage(upload,host.image,vk::ImageLayout::eTransferDstOptimal,
                upload_copy,dispatch);
            barrier(host.image,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
            vk::ImageLayout native_layout=vk::ImageLayout::eGeneral;
            if(rendering) {
                // The guest display is still black/stale after redirected FSR
                // writes. Restore it before a native attachment LOAD, not after.
                barrier(native.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferDstOptimal);
                scheduler.CommandBuffer().clearColorImage(native.image,vk::ImageLayout::eTransferDstOptimal,
                    vk::ClearColorValue(std::array<float,4>{0,0,0,1}),range,dispatch);
                native_layout=vk::ImageLayout::eColorAttachmentOptimal;
                barrier(native.image,vk::ImageLayout::eTransferDstOptimal,native_layout);
                CopyDisplayForRendering(scheduler,host.image,{SW,SH},native.image,{W,H},native_layout);
            } else {
                CopyDisplayForSampling(scheduler,host.image,{SW,SH},native.image,{W,H},generation!=0);
            }
            // A later host write cannot change the already-resolved freeze input.
            barrier(host.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferDstOptimal);
            scheduler.CommandBuffer().clearColorImage(host.image,vk::ImageLayout::eTransferDstOptimal,
                vk::ClearColorValue(std::array<float,4>{0,0,0,0}),range,dispatch);
            barrier(host.image,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);
            const auto native_draw=[&](vk::Rect2D rect) {
                const vk::RenderingAttachmentInfo attachment{.imageView=attachment_view,
                    .imageLayout=vk::ImageLayout::eColorAttachmentOptimal,
                    .loadOp=vk::AttachmentLoadOp::eLoad,.storeOp=vk::AttachmentStoreOp::eStore};
                scheduler.CommandBuffer().beginRendering({.renderArea={{0,0},{W,H}},.layerCount=1,
                    .colorAttachmentCount=1,.pColorAttachments=&attachment},dispatch);
                const vk::ClearAttachment clear{.aspectMask=vk::ImageAspectFlagBits::eColor,
                    .colorAttachment=0,.clearValue=vk::ClearColorValue(std::array<float,4>{0,0,0,1})};
                const vk::ClearRect area{.rect=rect,.baseArrayLayer=0,.layerCount=1};
                scheduler.CommandBuffer().clearAttachments(clear,area,dispatch);
                scheduler.CommandBuffer().endRendering(dispatch);
            };
            if(rendering) native_draw({{4,3},{7,5}});
            barrier(native.image,native_layout,vk::ImageLayout::eTransferSrcOptimal);
            const vk::BufferImageCopy download_copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .imageExtent={W,H,1}};
            scheduler.CommandBuffer().copyImageToBuffer(native.image,vk::ImageLayout::eTransferSrcOptimal,
                readback,download_copy,dispatch);
            barrier(native.image,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eGeneral);
            const vk::MemoryBarrier2 host_read{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead};
            scheduler.CommandBuffer().pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host_read},dispatch);
            scheduler.Finish();
            assert(vmaInvalidateAllocation(instance.GetAllocator(),ra,0,VK_WHOLE_SIZE)==VK_SUCCESS);
            for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)for(unsigned c=0;c<4;++c) {
                const bool changed=rendering&&x>=4&&x<11&&y>=3&&y<8;
                const auto expected=changed?(c==3?255:0):bytes[((y*2)*SW+x*2)*4+c];
                assert(result[(y*W+x)*4+c]==expected);
            }
            if(rendering) {
                // An intentional subsequent full black frame must still win;
                // the handoff must not resurrect the saved display over it.
                barrier(native.image,vk::ImageLayout::eGeneral,vk::ImageLayout::eColorAttachmentOptimal);
                native_draw({{0,0},{W,H}});
                barrier(native.image,vk::ImageLayout::eColorAttachmentOptimal,vk::ImageLayout::eTransferSrcOptimal);
                scheduler.CommandBuffer().copyImageToBuffer(native.image,vk::ImageLayout::eTransferSrcOptimal,
                    readback,download_copy,dispatch);
                barrier(native.image,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eGeneral);
                scheduler.CommandBuffer().pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host_read},dispatch);
                scheduler.Finish();
                assert(vmaInvalidateAllocation(instance.GetAllocator(),ra,0,VK_WHOLE_SIZE)==VK_SUCCESS);
                for(unsigned i=0;i<W*H*4;++i) assert(result[i]==(i%4==3?255:0));
            }
        }
        instance.GetDevice().destroyImageView(attachment_view,nullptr,dispatch);
        instance.GetDevice().destroyImageView(view,nullptr,dispatch);
        host.Destroy();native.Destroy();
    }
    vmaDestroyBuffer(instance.GetAllocator(),upload,ua);vmaDestroyBuffer(instance.GetAllocator(),readback,ra);
    std::puts("PASS: native display sampling and attachment handoff; RGBA/BGRA UNORM/sRGB, alpha, refresh, partial native writes and intentional black frames");
}
