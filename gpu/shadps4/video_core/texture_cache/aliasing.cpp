// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <optional>

#include "video_core/texture_cache/aliasing.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {

bool HaveCompatibleOverlapLayout(const ImageInfo& lhs, const ImageInfo& rhs) {
    // A same-format VkImage view cannot reinterpret CPU linear bytes as tiled
    // bytes. Pool reuse can change tiling even when dimensions match and the
    // old allocation is larger only because its final microtile row is padded.
    // Keep explicit cross-format reinterpretation on its existing paths.
    return lhs.pixel_format != rhs.pixel_format ||
           (lhs.tile_mode == rhs.tile_mode && lhs.array_mode == rhs.array_mode &&
            lhs.props.is_tiled == rhs.props.is_tiled);
}

bool HaveCompatibleOverlapDimensions(const ImageInfo& lhs, const ImageInfo& rhs) {
    // Neither image can back the other's views when one has more layers but
    // fewer mip levels. SubresourceExtent's ordering is lexicographic; it is
    // not a containment test. Leave these pool allocations on the existing
    // separate-image path instead of reusing or expanding the stale image.
    const bool crossed_resources =
        (lhs.resources.levels < rhs.resources.levels &&
         lhs.resources.layers > rhs.resources.layers) ||
        (lhs.resources.levels > rhs.resources.levels &&
         lhs.resources.layers < rhs.resources.layers);
    return HaveCompatibleOverlapLayout(lhs, rhs) && lhs.BlockDim() == rhs.BlockDim() &&
           lhs.num_bits * lhs.guest_samples == rhs.num_bits * rhs.guest_samples &&
           (lhs.pixel_format != rhs.pixel_format || !crossed_resources) &&
           // BlockDim uses pitch rather than active width. A view cannot give a
           // same-format VkImage a different normalized-coordinate extent.
           // Cross-format block reinterpretation retains its existing path.
           (lhs.pixel_format != rhs.pixel_format || lhs.size.width == rhs.size.width);
}

std::optional<u32> GetPaddedMipSlice(const ImageInfo& requested, const ImageInfo& parent,
                                   u32 mip) {
    if (requested.resources.levels != 1 || requested.resources.layers != 1 ||
        !parent.resources.layers || mip >= parent.resources.levels ||
        mip >= parent.mips_layout.size()) {
        return std::nullopt;
    }
    const auto& layout = parent.mips_layout[mip];
    const u32 slice_bytes = layout.size / parent.resources.layers;
    const VAddr mip_address = parent.guest_address + layout.offset;
    if (!slice_bytes || requested.guest_size >= slice_bytes ||
        requested.guest_address < mip_address) {
        return std::nullopt;
    }
    const u64 offset = requested.guest_address - mip_address;
    if (offset % slice_bytes || offset / slice_bytes >= parent.resources.layers) {
        return std::nullopt;
    }
    auto physical_mip = parent;
    physical_mip.guest_address = requested.guest_address;
    physical_mip.guest_size = slice_bytes;
    physical_mip.resources = {1, 1};
    physical_mip.size = {std::max(parent.size.width >> mip, 1u),
                         std::max(parent.size.height >> mip, 1u), parent.size.depth};
    physical_mip.pitch = layout.pitch;
    // The parent flag chose its physical mip offset/stride above. A view of
    // this one mip has no further pow2 level packing to perform.
    physical_mip.props.is_pow2 = false;
    if (!CanReusePaddedImage(physical_mip, requested)) {
        return std::nullopt;
    }
    return u32(offset / slice_bytes);
}

bool CanReusePaddedImage(const ImageInfo& cached, const ImageInfo& requested) {
    // Unlike mip chains, arrays and macro tiling, a single 1D thin-tiled image
    // addresses 8x8 tiles using pitch alone. Allocation height affects only
    // trailing padding. Both allocations must cover every logical tile row.
    if (cached.type != AmdGpu::ImageType::Color2D || cached.size != requested.size ||
        cached.pixel_format != requested.pixel_format || cached.size.depth != 1 ||
        cached.array_mode != AmdGpu::ArrayMode::Array1DTiledThin1 ||
        cached.tile_mode != AmdGpu::TileMode::Thin1DThin ||
        cached.resources.levels != 1 || requested.resources.levels != 1 ||
        cached.resources.layers != 1 || requested.resources.layers != 1 ||
        cached.num_samples != 1 || requested.num_samples != 1 ||
        cached.guest_samples != 1 || requested.guest_samples != 1 ||
        !cached.props.is_tiled || !requested.props.is_tiled ||
        cached.props.is_depth || requested.props.is_depth ||
        cached.props.is_block || requested.props.is_block ||
        cached.props.is_pow2 || requested.props.is_pow2 ||
        cached.meta_info.cmask_addr || requested.meta_info.cmask_addr ||
        cached.meta_info.fmask_addr || requested.meta_info.fmask_addr ||
        cached.meta_info.htile_addr || requested.meta_info.htile_addr ||
        !cached.num_bits || cached.num_bits % 8 || cached.pitch % 8 ||
        cached.pitch < cached.size.width || !cached.size.height ||
        cached.size.height > UINT32_MAX - 7) {
        return false;
    }
    const auto extent = GetAliasCopyExtent(cached, requested);
    if (!extent || *extent != cached.size) {
        return false;
    }
    const u64 row_bytes = u64{cached.pitch} * (cached.num_bits / 8);
    const u64 tile_rows = (cached.size.height + 7) & ~7u;
    return row_bytes <= std::min(cached.guest_size, requested.guest_size) / tile_rows;
}

std::optional<Extent3D> GetAliasCopyExtent(const ImageInfo& src, const ImageInfo& dst) {
    const bool same_layout =
        src.guest_address == dst.guest_address && src.num_bits == dst.num_bits &&
        src.num_samples == dst.num_samples && !src.props.is_depth && !dst.props.is_depth &&
        src.props.is_block == dst.props.is_block && src.props.is_pow2 == dst.props.is_pow2 &&
        src.type == dst.type && src.pitch == dst.pitch && src.tile_mode == dst.tile_mode &&
        src.array_mode == dst.array_mode && src.bank_swizzle == dst.bank_swizzle &&
        src.alt_tile == dst.alt_tile;
    if (!same_layout) {
        return std::nullopt;
    }

    if (src.pixel_format == dst.pixel_format && src.resources.levels == 1 &&
        dst.resources.levels == 1 && src.resources.layers >= dst.resources.layers &&
        src.size.width >= dst.size.width && src.size.height >= dst.size.height &&
        src.size.depth >= dst.size.depth) {
        return dst.size;
    }

    const bool can_copy_intersection = !src.props.is_block && src.resources.levels == 1 &&
                                       dst.resources.levels == 1 && src.resources.layers == 1 &&
                                       dst.resources.layers == 1 && src.size.depth == 1 &&
                                       dst.size.depth == 1 && src.size.width == dst.size.width &&
                                       IsVulkanFormatCompatible(src.pixel_format, dst.pixel_format);
    if (!can_copy_intersection) {
        return std::nullopt;
    }

    return Extent3D{
        .width = src.size.width,
        .height = std::min(src.size.height, dst.size.height),
        .depth = 1,
    };
}

} // namespace VideoCore
