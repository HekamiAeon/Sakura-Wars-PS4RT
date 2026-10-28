// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/amdgpu/resource.h"

namespace Shader {

// A repeating normalized sample cannot reach the sampler's border color. Restrict
// folding to float 2D images so integer conversion, comparison and cube semantics
// cannot change the descriptor's literal zero/one components.
inline bool ConstantSampleImage(const AmdGpu::Image& image, u32 components) {
    if (!image || image.GetType() != AmdGpu::ImageType::Color2D ||
        image.GetNumberFmt() != AmdGpu::NumberFormat::Float ||
        image.GetNumberConversion() != AmdGpu::NumberConversion::None ||
        AmdGpu::IsFmask(image.GetDataFmt()) || components == 0 || (components & ~15u)) {
        return false;
    }
    const auto swizzle = image.DstSelect();
    for (u32 i = 0; i < 4; ++i) {
        if ((components & (1u << i)) && swizzle.array[i] != AmdGpu::CompSwizzle::Zero &&
            swizzle.array[i] != AmdGpu::CompSwizzle::One) {
            return false;
        }
    }
    return true;
}

inline bool ConstantSampleSampler(const AmdGpu::Sampler& sampler) {
    return sampler && !sampler.force_unnormalized && !sampler.force_degamma &&
           sampler.clamp_x == AmdGpu::ClampMode::Wrap &&
           sampler.clamp_y == AmdGpu::ClampMode::Wrap &&
           sampler.clamp_z == AmdGpu::ClampMode::Wrap;
}

} // namespace Shader
