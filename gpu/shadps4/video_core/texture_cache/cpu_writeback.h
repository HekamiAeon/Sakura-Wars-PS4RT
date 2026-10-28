// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <optional>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {

struct CpuImageWritebackRange {
    VAddr address;
    u64 size;
};

// Head/tail pages can already be writable after an adjacent CPU access. Those
// bytes may now belong to a new allocation, even while SafeToDownload is true.
// Only the still-tracked interval retains GPU ownership during preservation.
inline std::optional<CpuImageWritebackRange> GetCpuImageWritebackRange(
    const ImageInfo& info, VAddr tracked_begin, VAddr tracked_end) {
    if (!tracked_begin || tracked_end <= tracked_begin ||
        info.guest_size > ~VAddr{0} - info.guest_address) {
        return std::nullopt;
    }
    const VAddr begin = std::max(info.guest_address, tracked_begin);
    const VAddr end = std::min(info.guest_address + info.guest_size, tracked_end);
    if (end <= begin) {
        return std::nullopt;
    }
    return CpuImageWritebackRange{begin, end - begin};
}

// The current CPU-write preservation path supports the observed single-sample
// RGBA8 microtiled layout. Extend this only with matching transfer coverage.
inline bool CanPreserveCpuImageWrite(const ImageInfo& info, VAddr address, u64 size) {
    if (!size || !info.guest_size || address >= info.guest_address + info.guest_size ||
        (address < info.guest_address && size <= info.guest_address - address)) {
        return false;
    }
    return info.type == AmdGpu::ImageType::Color2D && info.size.depth == 1 &&
           info.resources.levels == 1 && info.resources.layers == 1 &&
           info.num_samples == 1 && info.guest_samples == 1 && info.num_bits == 32 &&
           info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
           !info.props.is_depth && !info.props.is_block && info.props.is_tiled &&
           info.tile_mode == AmdGpu::TileMode::Thin1DThin &&
           info.array_mode == AmdGpu::ArrayMode::Array1DTiledThin1 &&
           info.pitch && info.pitch % 8 == 0 && info.size.width <= info.pitch &&
           info.size.width && info.size.height &&
           info.mips_layout[0].size && info.mips_layout[0].size <= info.guest_size &&
           !info.meta_info.cmask_addr && !info.meta_info.fmask_addr && !info.meta_info.htile_addr;
}

} // namespace VideoCore
