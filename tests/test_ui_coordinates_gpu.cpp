// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production FragCoord emitter and UI/display coordinate contract on Vulkan.
// Synthetic gradients only: no game shaders, assets, or captures.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_common.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include <vk_mem_alloc.h>

static std::vector<u32> Fragment(float width, float height) {
    using namespace Shader;
    using namespace Shader::Backend::SPIRV;
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    info.loads.Set(IR::Attribute::FragCoord, 0);
    info.loads.Set(IR::Attribute::FragCoord, 1);
    info.stores.Set(IR::Attribute::RenderTarget0);
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    Profile profile{};
    Shader::Backend::Bindings bindings{};
    EmitContext ctx(profile, runtime, info, bindings);
    const auto main = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                     ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel(ctx.OpLabel());
    const auto x = ctx.OpFDiv(ctx.F32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0),
                              ctx.ConstF32(width));
    const auto y = ctx.OpFDiv(ctx.F32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0),
                              ctx.ConstF32(height));
    ctx.OpStore(ctx.frag_outputs[0].id, ctx.OpCompositeConstruct(ctx.F32[4],
        std::array{x, y, ctx.ConstF32(0.f), ctx.ConstF32(1.f)}));
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::Fragment, main, "main",
                     std::span(ctx.interfaces.data(), ctx.interfaces.size()));
    ctx.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    return ctx.Assemble();
}

int main() {
    using namespace Vulkan;
    Instance instance(0, false);
    Scheduler scheduler(instance);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    const auto vertex = device.createShaderModuleUnique({.codeSize=sizeof(FS_TRI_VERT),
        .pCode=FS_TRI_VERT}, nullptr, d).value;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment, 0, sizeof(Shader::PushData)};
    const auto layout = device.createPipelineLayoutUnique({.pushConstantRangeCount=1,
        .pPushConstantRanges=&push}, nullptr, d).value;
    constexpr auto format = vk::Format::eR32G32B32A32Sfloat;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{}; VmaAllocation allocation{}; VmaAllocationInfo mapped{};
    assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ai,&buffer,&allocation,&mapped)==VK_SUCCESS);
    const auto* pixels = static_cast<const float*>(mapped.pMappedData);
    for (const auto size : {vk::Extent2D{1920,1080}, vk::Extent2D{2560,1440}, vk::Extent2D{3840,2160}}) {
        VideoCore::UniqueImage image(device,instance.GetAllocator());
        image.Create({.imageType=vk::ImageType::e2D,.format=format,
            .extent={size.width,size.height,1},.mipLevels=1,.arrayLayers=1,
            .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
            .usage=vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc});
        const auto view = device.createImageViewUnique({.image=image.image,.viewType=vk::ImageViewType::e2D,
            .format=format,.subresourceRange=range},nullptr,d).value;
        for (const bool native_ui : {true,false}) {
            // Mask users divide logical pixel coordinates by native dimensions. A physical
            // display fetch uses the actual output dimensions; shrinking it zooms the image.
            const auto code = Fragment(native_ui?1920.f:float(size.width),
                                       native_ui?1080.f:float(size.height));
            const auto fragment = device.createShaderModuleUnique({.codeSize=code.size()*4,
                .pCode=code.data()},nullptr,d).value;
            const std::array stages{
                vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eVertex,.module=*vertex,.pName="main"},
                vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eFragment,.module=*fragment,.pName="main"}};
            const vk::PipelineVertexInputStateCreateInfo vi{};
            const vk::PipelineInputAssemblyStateCreateInfo ia{.topology=vk::PrimitiveTopology::eTriangleList};
            const vk::Viewport vp{0,0,float(size.width),float(size.height),0,1};
            const vk::Rect2D rect{{0,0},size};
            const vk::PipelineViewportStateCreateInfo vs{.viewportCount=1,.pViewports=&vp,
                .scissorCount=1,.pScissors=&rect};
            const vk::PipelineRasterizationStateCreateInfo rs{.polygonMode=vk::PolygonMode::eFill,
                .cullMode=vk::CullModeFlagBits::eNone,.frontFace=vk::FrontFace::eCounterClockwise,.lineWidth=1};
            const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples=vk::SampleCountFlagBits::e1};
            const vk::PipelineColorBlendAttachmentState blend{.colorWriteMask=vk::ColorComponentFlagBits::eR|
                vk::ColorComponentFlagBits::eG|vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA};
            const vk::PipelineColorBlendStateCreateInfo bs{.attachmentCount=1,.pAttachments=&blend};
            const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
            const auto pipeline = device.createGraphicsPipelineUnique({}, {.pNext=&rendering,
                .stageCount=u32(stages.size()),.pStages=stages.data(),.pVertexInputState=&vi,
                .pInputAssemblyState=&ia,.pViewportState=&vs,.pRasterizationState=&rs,
                .pMultisampleState=&ms,.pColorBlendState=&bs,.layout=*layout},nullptr,d).value;
            Shader::PushData data{};
            data.scene_size = UiComposition::FragmentSize(native_ui,1920,1080,size.width,size.height);
            const auto cmd = scheduler.CommandBuffer();
            vk::ImageMemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask=vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eColorAttachmentOptimal,
                .image=image.image,.subresourceRange=range};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},d);
            const vk::RenderingAttachmentInfo attachment{.imageView=*view,
                .imageLayout=vk::ImageLayout::eColorAttachmentOptimal,.loadOp=vk::AttachmentLoadOp::eClear,
                .storeOp=vk::AttachmentStoreOp::eStore};
            cmd.beginRendering({.renderArea=rect,.layerCount=1,.colorAttachmentCount=1,
                .pColorAttachments=&attachment},d);
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*pipeline,d);
            cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eFragment,0,sizeof(data),&data,d);
            cmd.draw(3,1,0,0,d); cmd.endRendering(d);
            barrier.srcStageMask=vk::PipelineStageFlagBits2::eColorAttachmentOutput;
            barrier.srcAccessMask=vk::AccessFlagBits2::eColorAttachmentWrite;
            barrier.dstStageMask=vk::PipelineStageFlagBits2::eCopy;
            barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead;
            barrier.oldLayout=vk::ImageLayout::eColorAttachmentOptimal;
            barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},d);
            const std::array points{vk::Offset3D{0,0,0},vk::Offset3D{int(size.width/2),int(size.height/2),0},
                vk::Offset3D{int(size.width-1),int(size.height-1),0},vk::Offset3D{int(size.width-1),0,0}};
            std::array<vk::BufferImageCopy,4> copies{};
            for(u32 i=0;i<4;++i)copies[i]={.bufferOffset=i*16u,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .imageOffset=points[i],.imageExtent={1,1,1}};
            cmd.copyImageToBuffer(image.image,vk::ImageLayout::eTransferSrcOptimal,buffer,copies,d);
            scheduler.Finish();
            assert(vmaInvalidateAllocation(instance.GetAllocator(),allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS);
            for(u32 i=0;i<4;++i) {
                assert(std::abs(pixels[4*i]-(float(points[i].x)+.5f)/size.width)<1e-5f);
                assert(std::abs(pixels[4*i+1]-(float(points[i].y)+.5f)/size.height)<1e-5f);
                assert(pixels[4*i+3]==1.f);
            }
        }
    }
    vmaDestroyBuffer(instance.GetAllocator(),buffer,allocation);
    std::puts("ui-coordinates-gpu: native UI and physical display coordinates agree at 1080p/1440p/4K");
}
