// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/texture_cache/cpu_writeback.h"

using namespace VideoCore;

void RunCpuImageWritebackFaultTests();

int main() {
    ImageInfo info{};
    info.guest_address = 0x100100;
    info.guest_size = 8294400;
    info.size = {1920, 1080, 1};
    info.pitch = 1920;
    info.num_bits = 32;
    info.num_samples = info.guest_samples = 1;
    info.resources = {1, 1};
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.props.is_tiled = true;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;
    info.mips_layout[0] = {8294400, 1920, 1080, 0};
    const auto needs_copy = [](const ImageInfo& image, u64 offset = 3) {
        return CanPreserveCpuImageWrite(image, image.guest_address + offset, 8);
    };
    // Alpha-only CPU writes must preserve the rendered RGB, including at the
    // final pixel. The reported fault span does not describe a full overwrite.
    assert(needs_copy(info));
    assert(needs_copy(info, info.guest_size - 1));
    assert(CanPreserveCpuImageWrite(info, info.guest_address, info.guest_size));
    assert(!CanPreserveCpuImageWrite(info, info.guest_address - 8, 8));
    assert(!CanPreserveCpuImageWrite(info, info.guest_address + info.guest_size, 8));
    assert(!CanPreserveCpuImageWrite(info, info.guest_address, 0));
    // Keep unverified transfer/layout paths out of this capability.
    auto other = info;
    other.num_samples = 4;
    assert(!needs_copy(other));
    other = info; other.guest_samples = 4;
    assert(!needs_copy(other));
    other = info; other.resources.levels = 2;
    assert(!needs_copy(other));
    other = info; other.resources.layers = 2;
    assert(!needs_copy(other));
    other = info; other.props.is_depth = true;
    assert(!needs_copy(other));
    other = info; other.props.is_block = true;
    assert(!needs_copy(other));
    other = info; other.tile_mode = AmdGpu::TileMode::DisplayLinearAligned;
    assert(!needs_copy(other));
    other = info; other.array_mode = AmdGpu::ArrayMode::Array2DTiledThin1;
    assert(!needs_copy(other));
    other = info; other.pixel_format = vk::Format::eR32Sfloat;
    assert(!needs_copy(other));
    other = info; other.meta_info.fmask_addr = 0x200000;
    assert(!needs_copy(other));
    other = info; other.guest_size = 256;
    assert(!needs_copy(other));
    RunCpuImageWritebackFaultTests();
    std::puts("cpu-image-writeback-test: passed (partial CPU writes and layout exclusions)");
}
