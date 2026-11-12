// SPDX-License-Identifier: GPL-2.0-or-later
// ps4rt: whether the Vulkan pipeline-binary disk cache (VK_KHR_pipeline_binary) is used.
// Players on NVIDIA reported crashes while it was set up (InitializePipelineBinaryCache), so it
// is off there by default and pipelines compile the normal way (the driver keeps its own shader
// cache). PS4RT_PIPELINE_BINARIES=0 turns it off on any GPU; any other value forces it on,
// NVIDIA included (to try newer drivers).

#pragma once

#include <cstdint>

namespace Vulkan {

inline constexpr std::uint32_t NvidiaVendorId = 0x10de;

enum class PipelineBinaryChoice { On, OffBySetting, OffOnNvidia };

/// `setting`: PS4RT_PIPELINE_BINARIES (null when unset); `vendor_id`:
/// VkPhysicalDeviceProperties::vendorID of the GPU Vulkan uses.
inline PipelineBinaryChoice ChoosePipelineBinaries(const char* setting, std::uint32_t vendor_id) {
    if (setting && *setting) {
        return *setting == '0' ? PipelineBinaryChoice::OffBySetting : PipelineBinaryChoice::On;
    }
    return vendor_id == NvidiaVendorId ? PipelineBinaryChoice::OffOnNvidia : PipelineBinaryChoice::On;
}

} // namespace Vulkan
