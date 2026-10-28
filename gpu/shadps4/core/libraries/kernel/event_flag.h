// SPDX-License-Identifier: GPL-2.0-or-later
// Native port declarations for the reused shadPS4 event-flag provider.
#pragma once
#include "common/types.h"

namespace Core::Loader { class SymbolsResolver; }
namespace Libraries::Kernel {
class EventFlagInternal;
using OrbisKernelEventFlag = EventFlagInternal*;
struct OrbisKernelEventFlagOptParam { size_t size; };
void RegisterKernelEventFlag(Core::Loader::SymbolsResolver* sym);
}
