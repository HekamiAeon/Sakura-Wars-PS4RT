// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "video_core/texture_cache/types.h"

namespace VideoCore {

struct ImageInfo;

// Same-format pool reuse cannot change how guest bytes are tiled. Different-
// format reinterpretation keeps its existing overlap rules.
[[nodiscard]] bool HaveCompatibleOverlapLayout(const ImageInfo& lhs, const ImageInfo& rhs);

// Initial same-address overlap gate; padded allocation pitch alone establishes
// neither normalized sampling extent nor the guest upload layout.
[[nodiscard]] bool HaveCompatibleOverlapDimensions(const ImageInfo& lhs, const ImageInfo& rhs);

[[nodiscard]] std::optional<Extent3D> GetAliasCopyExtent(const ImageInfo& src,
                                                         const ImageInfo& dst);
// A single 1D thin-tiled surface can describe extra trailing allocation rows
// without changing the texel layout of its identical logical image.
[[nodiscard]] bool CanReusePaddedImage(const ImageInfo& cached, const ImageInfo& requested);

// A single-level view can omit trailing pow2 allocation padding of a parent
// mip, while retaining identical active microtile rows and the parent's stride.
[[nodiscard]] std::optional<u32> GetPaddedMipSlice(const ImageInfo& requested,
                                                 const ImageInfo& parent, u32 mip);

} // namespace VideoCore
