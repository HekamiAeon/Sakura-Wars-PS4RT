// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
namespace Vulkan {
class DisplayRegistry {
public:
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);
};
}
