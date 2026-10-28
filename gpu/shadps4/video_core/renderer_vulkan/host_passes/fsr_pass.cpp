//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "ps4rt_graphics.h"
#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fsr_easu_comp.h"
#include "video_core/host_shaders/fsr_rcas_comp.h"
#include "video_core/host_shaders/spatial_aa_comp.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#define A_CPU
#include "core/debug_state.h"
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/fsr/ffx_fsr1.h"

typedef u32 uvec4[4];

struct FSRConstants {
    uvec4 Const0;
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    uvec4 Sample;
};

namespace Vulkan::HostPasses {

FsrPass::~FsrPass() = default;

void FsrPass::Create(vk::Device device, VmaAllocator allocator, u32 num_images) {
    this->device = device;
    this->num_images = num_images;

    sampler = Check<"create upscaling sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = -1000.0f,
        .maxLod = 1000.0f,
    }));

    std::array<vk::DescriptorSetLayoutBinding, 3> layoutBindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler.get(),
        },
    }};

    descriptor_set_layout =
        Check<"create fsr descriptor set layout">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = layoutBindings.size(),
            .pBindings = layoutBindings.data(),
        }));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(FSRConstants),
    };

    const auto& cs_easu_module = CompileSPV(FSR_EASU_COMP, device);
    ASSERT(cs_easu_module);
    SetObjectName(device, cs_easu_module, "fsr.comp [EASU]");

    const auto& cs_rcas_module = CompileSPV(FSR_RCAS_COMP, device);
    ASSERT(cs_rcas_module);
    SetObjectName(device, cs_rcas_module, "fsr.comp [RCAS]");

    pipeline_layout = Check<"fsp pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    SetObjectName(device, pipeline_layout.get(), "fsr pipeline layout");

    const vk::ComputePipelineCreateInfo easu_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_easu_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    easu_pipeline =
        Check<"fsp easu compute pipelines">(device.createComputePipelineUnique({}, easu_pinfo));
    SetObjectName(device, easu_pipeline.get(), "fsr easu pipeline");

    const vk::ComputePipelineCreateInfo rcas_pinfo{
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = cs_rcas_module,
            .pName = "main",
        },
        .layout = pipeline_layout.get(),
    };
    rcas_pipeline =
        Check<"fsp rcas compute pipelines">(device.createComputePipelineUnique({}, rcas_pinfo));
    SetObjectName(device, rcas_pipeline.get(), "fsr rcas pipeline");
    const auto aa_module = CompileSPV(SPATIAL_AA_COMP, device);
    const vk::ComputePipelineCreateInfo aa_info{
        .stage{.stage=vk::ShaderStageFlagBits::eCompute, .module=aa_module, .pName="main"},
        .layout=pipeline_layout.get(),
    };
    aa_pipeline = Check<"spatial AA pipeline">(device.createComputePipelineUnique({}, aa_info));
    device.destroyShaderModule(aa_module);

    device.destroyShaderModule(cs_easu_module);
    device.destroyShaderModule(cs_rcas_module);

    available_imgs.resize(num_images);
    for (int i = 0; i < num_images; ++i) {
        auto& img = available_imgs[i];
        img.id = i;
        img.intermediary_image = VideoCore::UniqueImage(device, allocator);
        img.output_image = VideoCore::UniqueImage(device, allocator);
        img.aa_image = VideoCore::UniqueImage(device, allocator);
    }
}

vk::ImageView FsrPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                              vk::Extent2D input_size, vk::Extent2D output_size, Settings settings,
                              bool hdr) {
    // ps4rt: FSR 1 upscales when the window is larger; RCAS alone also runs at the same size.
    const bool upscale = Ps4RtGraphics::FsrPassRuns(settings.enable, settings.use_rcas, int(input_size.width),
                                   int(input_size.height), int(output_size.width),
                                   int(output_size.height));
    // SDR spatial AA is independent of FSR: filter at source resolution first,
    // including when scaling/sharpening is off. HDR transfer curves need a
    // separate integration; preserve the existing HDR path for now.
    const bool aa = settings.anti_aliasing && !hdr;
    DebugState.is_using_fsr = upscale;
    if (!upscale && !aa) {
        return input;
    }
    auto& img = available_imgs[cur_image];
    if (++cur_image >= available_imgs.size()) cur_image = 0;
    if (aa) input = AntiAlias(cmdbuf,img,input,input_size,settings.srgb_input);
    if (!upscale) return input;

    if (output_size != cur_size) {
        ResizeAndInvalidate(output_size.width, output_size.height);
    }
    auto [width, height] = cur_size;

    if (img.dirty) {
        CreateImages(img);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/FSR",
        });
    }

    static const int thread_group_work_region_dim = 16;
    int dispatch_x = (width + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
    int dispatch_y = (height + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array enter_barrier{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = img.intermediary_image,
            .subresourceRange = simple_subresource,
        },
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = enter_barrier.size(),
        .pImageMemoryBarriers = enter_barrier.data(),
    });

    FSRConstants consts{};
    FsrEasuCon(reinterpret_cast<AU1*>(&consts.Const0), reinterpret_cast<AU1*>(&consts.Const1),
               reinterpret_cast<AU1*>(&consts.Const2), reinterpret_cast<AU1*>(&consts.Const3),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height), (AF1)width,
               (AF1)height);
    consts.Sample[0] = hdr && !settings.use_rcas ? 1 : 0;

    if (settings.use_rcas) {

        { // easu
            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {
                    .imageView = input,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                },
                {
                    .imageView = img.intermediary_image_view.get(),
                    .imageLayout = vk::ImageLayout::eGeneral,
                },
                {
                    .sampler = sampler.get(),
                },
            }};

            std::array<vk::WriteDescriptorSet, 3> set_writes{{
                {
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampledImage,
                    .pImageInfo = &img_info[0],
                },
                {
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage,
                    .pImageInfo = &img_info[1],
                },
                {
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampler,
                    .pImageInfo = &img_info[2],
                },
            }};

            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, easu_pipeline.get());
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                        set_writes);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(FSRConstants), &consts);
            cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
        }

        std::array img_barrier{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = img.intermediary_image,
                .subresourceRange = simple_subresource,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = img.output_image,
                .subresourceRange = simple_subresource,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = img_barrier.size(),
            .pImageMemoryBarriers = img_barrier.data(),
        });

        { // rcas
            consts = {};
            FsrRcasCon(reinterpret_cast<AU1*>(&consts.Const0), settings.rcas_attenuation);
            consts.Sample[0] = hdr ? 1 : 0;

            std::array<vk::DescriptorImageInfo, 3> img_info{{
                {
                    .imageView = img.intermediary_image_view.get(),
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                },
                {
                    .imageView = img.output_image_view.get(),
                    .imageLayout = vk::ImageLayout::eGeneral,
                },
                {
                    .sampler = sampler.get(),
                },
            }};

            std::array<vk::WriteDescriptorSet, 3> set_writes{{
                {
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampledImage,
                    .pImageInfo = &img_info[0],
                },
                {
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eStorageImage,
                    .pImageInfo = &img_info[1],
                },
                {
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eSampler,
                    .pImageInfo = &img_info[2],
                },
            }};

            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, rcas_pipeline.get());
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                        set_writes);
            cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(FSRConstants), &consts);
            cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
        }

    } else {
        // only easu
        std::array<vk::DescriptorImageInfo, 3> img_info{{
            {
                .imageView = input,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            },
            {
                .imageView = img.output_image_view.get(),
                .imageLayout = vk::ImageLayout::eGeneral,
            },
            {
                .sampler = sampler.get(),
            },
        }};

        std::array<vk::WriteDescriptorSet, 3> set_writes{{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &img_info[0],
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &img_info[1],
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampler,
                .pImageInfo = &img_info[2],
            },
        }};

        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, easu_pipeline.get());
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0,
                                    set_writes);
        cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(FSRConstants), &consts);
        cmdbuf.dispatch(dispatch_x, dispatch_y, 1);
    }

    const std::array return_barrier{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = img.output_image,
            .subresourceRange = simple_subresource,
        },
    };
    cmdbuf.pipelineBarrier2({
        .imageMemoryBarrierCount = return_barrier.size(),
        .pImageMemoryBarriers = return_barrier.data(),
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    return img.output_image_view.get();
}

vk::ImageView FsrPass::AntiAlias(vk::CommandBuffer cmdbuf, Img& img, vk::ImageView input,
                                 vk::Extent2D size, bool srgb_input) {
    if (img.aa_size != size) {
        img.aa_view.reset();
        img.aa_image.Destroy();
        img.aa_image.Create({.imageType=vk::ImageType::e2D,
            .format=vk::Format::eR16G16B16A16Sfloat, .extent={size.width,size.height,1},
            .mipLevels=1, .arrayLayers=1, .samples=vk::SampleCountFlagBits::e1,
            .tiling=vk::ImageTiling::eOptimal,
            .usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled});
        img.aa_view = Check<"spatial AA image view">(device.createImageViewUnique({
            .image=img.aa_image, .viewType=vk::ImageViewType::e2D,
            .format=vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
        img.aa_size = size;
    }
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask=vk::AccessFlagBits2::eShaderWrite,
        .oldLayout=vk::ImageLayout::eUndefined, .newLayout=vk::ImageLayout::eGeneral,
        .image=img.aa_image, .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
    const std::array<vk::DescriptorImageInfo,3> infos{{
        {.imageView=input,.imageLayout=vk::ImageLayout::eShaderReadOnlyOptimal},
        {.imageView=*img.aa_view,.imageLayout=vk::ImageLayout::eGeneral},
        {.sampler=*sampler}}};
    std::array<vk::WriteDescriptorSet,3> writes{};
    for (u32 i=0;i<3;++i) {
        writes[i]={.dstBinding=i,.descriptorCount=1,
            .descriptorType=i==0 ? vk::DescriptorType::eSampledImage :
                            i==1 ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampler,
            .pImageInfo=&infos[i]};
    }
    FSRConstants constants{};
    constants.Sample[1]=srgb_input;
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,*aa_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*pipeline_layout,0,writes);
    cmdbuf.pushConstants(*pipeline_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(constants),&constants);
    cmdbuf.dispatch((size.width+7)/8,(size.height+7)/8,1);
    barrier.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader;
    barrier.srcAccessMask=vk::AccessFlagBits2::eShaderWrite;
    barrier.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands;
    barrier.dstAccessMask=vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout=vk::ImageLayout::eGeneral;
    barrier.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
    return *img.aa_view;
}

void FsrPass::ResizeAndInvalidate(u32 width, u32 height) {
    this->cur_size = vk::Extent2D{
        .width = width,
        .height = height,
    };
    for (int i = 0; i < num_images; ++i) {
        available_imgs[i].dirty = true;
    }
}

void FsrPass::CreateImages(Img& img) const {
    img.dirty = false;

    // Destroy previous resources before re-creating at new size.
    // Views first, then images (views reference the images).
    img.intermediary_image_view.reset();
    img.output_image_view.reset();
    img.intermediary_image.Destroy();
    img.output_image.Destroy();

    vk::ImageCreateInfo image_create_info{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{
            .width = cur_size.width,
            .height = cur_size.height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        // .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    img.intermediary_image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.intermediary_image),
                  "FSR Intermediary Image #{}", img.id);
    image_create_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                              vk::ImageUsageFlagBits::eColorAttachment;
    img.output_image.Create(image_create_info);
    SetObjectName(device, static_cast<vk::Image>(img.output_image), "FSR Output Image #{}", img.id);

    vk::ImageViewCreateInfo image_view_create_info{
        .image = img.intermediary_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    img.intermediary_image_view = Check<"create fsr intermediary image view">(
        device.createImageViewUnique(image_view_create_info));
    SetObjectName(device, img.intermediary_image_view.get(), "FSR Intermediary ImageView #{}",
                  img.id);

    image_view_create_info.image = img.output_image;
    img.output_image_view =
        Check<"create fsr output image view">(device.createImageViewUnique(image_view_create_info));
    SetObjectName(device, img.output_image_view.get(), "FSR Output ImageView #{}", img.id);
}

} // namespace Vulkan::HostPasses
