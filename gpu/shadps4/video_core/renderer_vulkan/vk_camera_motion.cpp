// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/ps4rt_camera.h"
#include "video_core/renderer_vulkan/sakura_scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ps4rt_toggles.h"
#include "ps4rt_settings.h"

#include "video_core/host_shaders/camera_motion_comp.h"

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
};

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {

    const char* upscaler = std::getenv("PS4RT_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless PS4RT_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    const auto device = instance.GetDevice();
    if (for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 3> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    return 2.0f * std::atan(1.0f / current.proj[1]);
}

float CameraMotion::Near() const noexcept {
    // depth = zs + zo / z is 0 at the near plane.
    return DepthInverted() ? current.proj[3] / (1-current.proj[2])
                           : -current.proj[3] / current.proj[2];
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : vk::Image{};
}

void CameraMotion::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView motion_view, u32 width, u32 height) {
    CommitFrameCamera();
    bool object_valid = false;
    const vk::ImageView object_view = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(cmdbuf, width, height, object_valid) : depth_view;
    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = object_valid ? 1u : 0u,
    };
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void CameraMotion::OnConstants(const float* data) {
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

void CameraMotion::OnPs4RtConstants(const float* data, u64 bytes) {
    if (pass_camera.HasCamera() || !data || !depth_id || bytes != 1424) return;
    Ps4RtCamera::Camera camera;
    if (!Ps4RtCamera::Read({data, bytes/sizeof(float)}, camera)) return;
    Camera candidate;
    candidate.view = camera.view;
    candidate.inv_view = camera.inverse_view;
    candidate.proj = camera.projection;
    candidate.depth_transform = pending_depth_transform;
    candidate.valid = true;
    pass_camera.SetCamera(candidate);
    const auto& image = texture_cache.GetImage(depth_id);
    const std::array<u32,2> size{image.info.size.width,image.info.size.height};
    if (render_size != size) {
        std::printf("PS4RT: scene camera %ux%u, reversed depth, near %.3f far %.1f\n",
                    size[0],size[1],candidate.proj[3]/(1-candidate.proj[2]),
                    -candidate.proj[3]/candidate.proj[2]);
        render_size = size;
    }
    frame_has_camera = true;
}

void CameraMotion::OnSakuraView(const float* data, u64 bytes) {
    if (frame_has_camera || !data) return;
    // Bounded scope report: the first few rejected bindings and why.
    static unsigned reports = 0;
    SakuraScene::Camera camera;
    const char* why = "size";
    if (bytes != SakuraScene::ViewBytes ||
        !SakuraScene::ReadView({data, bytes / sizeof(float)}, camera, &why)) {
        if (reports < 4) {
            ++reports;
            std::printf("PS4RT: Sakura view rejected (%llu bytes): %s; proj %.4g %.4g %.4g %.4g\n",
                        (unsigned long long)bytes, why, bytes >= 128 ? data[16] : 0.f,
                        bytes >= 128 ? data[21] : 0.f, bytes >= 128 ? data[26] : 0.f,
                        bytes >= 128 ? data[30] : 0.f);
        }
        return;
    }
    previous = current;
    current.view = camera.view;
    current.inv_view = camera.inverse_view;
    current.proj = camera.projection;
    current.depth_transform = {1.f, 0.f, 0.f};
    current.valid = true;
    frame_has_camera = true;
    if (render_size[0] != 1920 || render_size[1] != 1080) {
        render_size = {1920, 1080};
        std::printf("PS4RT: Sakura scene camera 1920x1080, reversed depth, near %.3f far %.1f\n",
                    Near(), Far());
    }
}

float CameraMotion::Far() const noexcept {
    if (DepthInverted()) return -current.proj[3]/current.proj[2];
    return current.valid && current.proj[2]>1.f ?
        -current.proj[3]/(current.proj[2]-1.f) : 3000.f;
}



std::string CameraMotion::StateDescription() const {
    std::string result;
    const auto append = [&](const char* label, const auto& values) {
        result += label;
        for (float value : values) {
            char number[48];
            std::snprintf(number, sizeof(number), " %.9g", double(value));
            result += number;
        }
        result += '\n';
    };
    append("current_view", current.view);
    append("previous_view", previous.view);
    append("current_projection", current.proj);
    append("previous_projection", previous.proj);
    append("reproject", Multiply(previous.view, current.inv_view));
    append("depth_viewport_scale_offset_minusw", current.depth_transform);
    append("jitter", jitter);
    append("previous_jitter", previous_jitter);
    return result;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth, std::array<float, 3> depth_transform) {
    if (Ps4RtSettings::Get().native_scene && pass_camera.BeginPass(depth)) {
        // This title renders other camera views before the main scene. A valid
        // camera from an earlier depth target must not stand in for this pass.
        frame_has_camera = false;
    }
    depth_id = depth;
    pending_depth_transform = depth_transform;
}

void CameraMotion::CommitFrameCamera() {
    if (Ps4RtSettings::Get().native_scene) pass_camera.Commit(current,previous);
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    CommitFrameCamera();
    
    if (!frame_has_camera) InvalidateHistory();
    frame_has_camera = false;
    pass_camera.EndFrame();
    depth_id = {};
}



} // namespace Vulkan
