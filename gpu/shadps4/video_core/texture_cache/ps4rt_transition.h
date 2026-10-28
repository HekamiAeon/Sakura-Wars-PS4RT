// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ps4rt_gpu_workarounds.h"
#include "video_core/texture_cache/cpu_writeback.h"
#include "video_core/texture_cache/image_view.h"

namespace VideoCore::Ps4RtTransition {
inline bool Consumer(u64 shader) {
    return Ps4RtGpuWorkarounds::Current().preserve_transition_snapshot &&
           (shader==0x06ffbe54 || shader==0x50a37319);
}
inline bool Supported(const ImageInfo& info) {
    return info.size.width==1920 && info.size.height==1080 && info.pitch==1920 &&
           CanPreserveCpuImageWrite(info,info.guest_address,info.guest_size);
}
inline bool Matches(const ImageInfo& saved, const ImageInfo& request,
                    const ImageViewInfo& view) {
    return Supported(request) && saved.guest_address==request.guest_address &&
           saved.guest_size==request.guest_size && saved.pixel_format==request.pixel_format &&
           saved.tile_mode==request.tile_mode && saved.array_mode==request.array_mode &&
           view.type==AmdGpu::ImageType::Color2D && view.format==saved.pixel_format &&
           !view.is_storage && view.range.base.level==0 && view.range.base.layer==0 &&
           view.range.extent.levels==1 && view.range.extent.layers==1 && view.min_lod==0;
}
} // namespace VideoCore::Ps4RtTransition
