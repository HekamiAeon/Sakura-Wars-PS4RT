// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <magic_enum/magic_enum.hpp>
#include "ps4rt_copy.h"
#include "ps4rt_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

extern thread_local unsigned long long ps4rt_fault_discard_lo, ps4rt_fault_discard_hi;
/// Bytes the faulting write overwrites entirely (see ps4rtgpu_handle_fault), set on the GPU
/// thread before AsyncWriteFault.
static unsigned long long async_fault_discard_lo = 0, async_fault_discard_hi = 0;

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// ps4rt: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const Ps4RtCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const Ps4RtCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !Ps4RtToggle::Disabled(Ps4RtToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    Ps4RtCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("PS4RT_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || Ps4RtToggle::Disabled(Ps4RtToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const u64 discard_lo = is_write ? ps4rt_fault_discard_lo : 0;
    const u64 discard_hi = is_write ? ps4rt_fault_discard_hi : 0;
    const auto flush_request = [this, device_addr, size, is_write, discard_lo, discard_hi] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        async_fault_discard_lo = discard_lo;
        async_fault_discard_hi = discard_hi;
        if (is_write && AsyncWriteFault(arena, device_addr, size, window_start, window_end)) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            return;
        }
        // A write needs no wait for bytes only an asynchronous copy still holds: the publish
        // keeps what the CPU writes (RetireAsyncDownloads) and uploads include it
        // (ForEachPendingCpuWrite).
        DownloadMemory(arena, window_start, window_end - window_start, !is_write);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size,
                                 bool wait_pending) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        if (wait_pending && AsyncDownloadPending(device_addr, size)) {
            scheduler.Finish();
            RetireAsyncDownloads(true);
        }
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    // Older asynchronous copies of these bytes go to guest memory first.
    RetireAsyncDownloads(true);
    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

// ps4rt: a guest CPU write to a page that also holds GPU-written bytes used to download the
// window around it and wait for the GPU (Scheduler::Finish). Games often keep small GPU results
// next to per-frame CPU data (Code Fairy: a 2 KiB brightness grid from cs 27a06d64, read by the
// CPU for auto exposure); on the PS4 they share memory and that write never waits. Here, when
// the faulting bytes are not GPU written, the GPU bytes of the same window are copied back
// without waiting and written to guest memory once the GPU is done (RetireAsyncDownloads),
// a frame or two later. Until then the host copy is the newer one: IsRegionGpuModified stays
// true and uploads skip those bytes, except ones the CPU wrote since (ForEachPendingCpuWrite);
// the publish keeps those too. PS4RT_ASYNC_WRITE_FAULT=0 restores the waiting download.
bool BufferCache::AsyncWriteFault(const Buffer* arena, VAddr device_addr, u64 size,
                                  VAddr window_start, VAddr window_end) {
    if (!async_write_faults || EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Relaxed) {
        return false;
    }
    // The bytes a whole-buffer copy overwrites may be GPU written: they are dropped below.
    const VAddr discard_lo = std::max<VAddr>(async_fault_discard_lo, window_start);
    const VAddr discard_hi = std::min<VAddr>(async_fault_discard_hi, window_end);
    const bool discard = discard_lo < discard_hi;
    const bool fault_discarded =
        discard && device_addr >= discard_lo && device_addr + size <= discard_hi;
    if (!fault_discarded && gpu_modified_ranges.Intersects(device_addr, size)) {
        // The CPU writes GPU-written bytes: they must be in guest memory first.
        return false;
    }
    // The same window as the waiting download: the CPU may read any GPU result near its write
    // (Code Fairy's brightness grid spans two pages; copying only the faulting page left most
    // of it stale and broke auto exposure).
    AsyncDownload entry{};
    u64 total = 0;
    memory_tracker->ForEachDownloadRange<false>(window_start, window_end - window_start,
                                                [&](u64 address, u64 length) {
        gpu_modified_ranges.ForEachInRange(address, length, [&](VAddr start, VAddr end) {
            const auto add = [&](VAddr lo, VAddr hi) {
                if (lo >= hi) {
                    return;
                }
                entry.ranges.push_back({lo, vk::BufferCopy{
                    .srcOffset = lo - arena->cpu_addr, .dstOffset = total, .size = hi - lo}});
                total += Common::AlignUp(hi - lo, 64);
            };
            if (!discard || end <= discard_lo || start >= discard_hi) {
                add(start, end);
                return;
            }
            // Dead GPU bytes under the new buffer's data are not copied back.
            add(start, discard_lo);
            add(discard_hi, end);
        });
    });
    if (discard) {
        gpu_modified_ranges.Subtract(discard_lo, discard_hi - discard_lo);
        // Copies queued while this memory held the old buffer must not land on the new one.
        DropPendingAsync(discard_lo, discard_hi - discard_lo);
    }
    if (entry.ranges.empty()) {
        if (!discard) {
            return false;
        }
        // Nothing else in the window: the write goes ahead without any copy.
        memory_tracker->UnmarkRegionAsGpuModified(window_start, window_end - window_start);
        return true;
    }
    // In Relaxed mode GPU pages are only write protected, and once this returns the whole
    // window is CPU writable: the game may write these very bytes (reset a counter, a
    // constant) before the copy lands. Remember what they held so the publish keeps those
    // writes instead of overwriting them with older GPU data (Sakura: lighting flicker and
    // a GPU hang without this).
    entry.snapshot.resize(total);
    for (const auto& [address, copy] : entry.ranges) {
        memory->CopySparseMemory(address, entry.snapshot.data() + copy.dstOffset, copy.size);
    }
    for (const auto& [address, copy] : entry.ranges) {
        gpu_modified_ranges.Subtract(address, copy.size);
    }
    entry.staging = staging_pool.Request(total, VideoCore::MemoryType::HostCached, 0, true);
    boost::container::small_vector<vk::BufferCopy, 2> copies;
    for (const auto& [address, copy] : entry.ranges) {
        copies.push_back(copy);
        copies.back().dstOffset += entry.staging.offset;
    }
    runtime.CopyBuffer(arena, entry.staging.buffer, copies);
    entry.tick = scheduler.CurrentTick();
    memory_tracker->UnmarkRegionAsGpuModified(window_start, window_end - window_start);
    std::scoped_lock lock{async_mutex};
    for (const auto& [address, copy] : entry.ranges) {
        async_download_ranges.Add(address, copy.size);
    }
    async_downloads.push_back(std::move(entry));
    has_async_downloads.store(true, std::memory_order_release);
    return true;
}

void BufferCache::RetireAsyncDownloads(bool all) {
    if (!has_async_downloads.load(std::memory_order_acquire)) {
        return;
    }
    // Called on every buffer bind: check the GPU tick known from the last submit and read
    // the semaphore (a kernel call) only on every 64th check.
    static std::atomic<u32> polls{0};
    const auto done = [this](u64 tick) {
        Vulkan::Semaphore* const semaphore = scheduler.GetWorkSemaphore();
        if (semaphore->IsFree(tick)) {
            return true;
        }
        if ((polls.fetch_add(1, std::memory_order_relaxed) & 63) != 0) {
            return false;
        }
        semaphore->Refresh();
        return semaphore->IsFree(tick);
    };
    std::scoped_lock lock{async_mutex};
    bool retired = false;
    static std::vector<u8> current, merged;
    while (!async_downloads.empty() && (all || done(async_downloads.front().tick))) {
        auto& entry = async_downloads.front();
        entry.staging.Invalidate();
        for (const auto& [address, copy] : entry.ranges) {
            const u8* gpu = entry.staging.mapped + copy.dstOffset;
            const u8* before = entry.snapshot.data() + copy.dstOffset;
            current.resize(copy.size);
            memory->CopySparseMemory(address, current.data(), copy.size);
            const u8* src = gpu;
            bool skipped = false;
            for (const auto& [skip_lo, skip_hi] : entry.skips) {
                const VAddr lo = std::max<VAddr>(skip_lo, address);
                const VAddr hi = std::min<VAddr>(skip_hi, address + copy.size);
                if (lo < hi) {
                    if (!skipped) {
                        merged.assign(gpu, gpu + copy.size);
                        skipped = true;
                    }
                    std::memcpy(merged.data() + (lo - address), current.data() + (lo - address),
                                hi - lo);
                }
            }
            if (skipped) {
                // Keep the new buffer's bytes; the merge below works on `merged`.
                gpu = merged.data();
                src = gpu;
            }
            if (std::memcmp(current.data(), before, copy.size) != 0) {
                // The CPU wrote some of these bytes after the copy was queued: keep them.
                if (!skipped) {
                    merged.assign(gpu, gpu + copy.size);
                }
                for (u64 i = 0; i < copy.size; ++i) {
                    if (current[i] != before[i]) {
                        merged[i] = current[i];
                    }
                }
                src = merged.data();
            }
            memory->TryWriteBacking(std::bit_cast<u8*>(address), src, copy.size);
            // A later pending copy of the same bytes saw the guest value from before this
            // publish; update it so the publish is not mistaken for a CPU write.
            for (std::size_t later = 1; later < async_downloads.size(); ++later) {
                auto& next = async_downloads[later];
                for (const auto& [next_address, next_copy] : next.ranges) {
                    const VAddr lo = std::max<VAddr>(address, next_address);
                    const VAddr hi = std::min<VAddr>(address + copy.size,
                                                     next_address + next_copy.size);
                    if (lo < hi) {
                        std::memcpy(next.snapshot.data() + next_copy.dstOffset +
                                        (lo - next_address),
                                    src + (lo - address), hi - lo);
                    }
                }
            }
        }
        staging_pool.FreeDeferred(entry.staging);
        async_downloads.pop_front();
        retired = true;
    }
    if (retired) {
        // Entries can share bytes (one per frame): rebuild from the ones still pending.
        async_download_ranges.Clear();
        for (const auto& entry : async_downloads) {
            AddPendingRanges(entry);
        }
        has_async_downloads.store(!async_downloads.empty(), std::memory_order_release);
    }
}

void BufferCache::DropPendingAsync(VAddr addr, u64 size) {
    if (!has_async_downloads.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{async_mutex};
    for (auto& pending : async_downloads) {
        for (const auto& [address, copy] : pending.ranges) {
            if (address < addr + size && address + copy.size > addr) {
                pending.skips.push_back({addr, addr + size});
                break;
            }
        }
    }
    // The guest bytes are the new ones now: uploads must not skip them as pending (they did,
    // and draws used the old buffer's data for a frame: flickering lighting).
    async_download_ranges.Subtract(addr, size);
}

// Pending bytes of an entry, without the ranges a new buffer overwrote (DropPendingAsync).
void BufferCache::AddPendingRanges(const AsyncDownload& entry) {
    for (const auto& [address, copy] : entry.ranges) {
        boost::container::small_vector<std::pair<VAddr, VAddr>, 4> pieces{
            {address, address + copy.size}};
        for (const auto& [skip_lo, skip_hi] : entry.skips) {
            boost::container::small_vector<std::pair<VAddr, VAddr>, 4> next;
            for (const auto& [lo, hi] : pieces) {
                if (skip_hi <= lo || skip_lo >= hi) {
                    next.push_back({lo, hi});
                    continue;
                }
                if (lo < skip_lo) {
                    next.push_back({lo, skip_lo});
                }
                if (skip_hi < hi) {
                    next.push_back({skip_hi, hi});
                }
            }
            pieces = std::move(next);
        }
        for (const auto& [lo, hi] : pieces) {
            async_download_ranges.Add(lo, hi - lo);
        }
    }
}

bool BufferCache::AsyncDownloadPending(VAddr addr, u64 size) const {
    if (!has_async_downloads.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lock{async_mutex};
    return async_download_ranges.Intersects(addr, size);
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    RetireAsyncDownloads(false);

    
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        
        // ps4rt: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !Ps4RtToggle::Disabled(Ps4RtToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);

    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!Ps4RtToggle::Disabled(Ps4RtToggle::DeferredUploads)) {
        // ps4rt: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const Ps4RtCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                Ps4RtCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size) || AsyncDownloadPending(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }


    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

// ps4rt: [addr, addr + size) is CPU modified and holds bytes of pending asynchronous copies,
// whose host copy is newer than the guest's: an upload skips them. But the CPU may have written
// some of them since the copy was queued (the publish keeps those, RetireAsyncDownloads), and
// the GPU must see those writes: they are found against the copy's snapshot and uploaded.
// (Sakura: 64 KiB and 38 KiB read-only buffers, about once every 20 s.) A CPU write of the
// value the guest already held cannot be told apart and stays skipped.
void BufferCache::ForEachPendingCpuWrite(VAddr addr, u64 size, auto&& func) {
    static std::vector<u8> guest, state;
    async_download_ranges.ForEachInRange(addr, size, [&](VAddr lo, VAddr hi) {
        guest.resize(hi - lo);
        memory->CopySparseMemory(lo, guest.data(), hi - lo);
        // 1: same as the snapshot of the newest copy holding the byte, 2: changed since.
        state.assign(hi - lo, 0);
        for (const auto& entry : async_downloads) {
            for (const auto& [address, copy] : entry.ranges) {
                const VAddr from = std::max<VAddr>(address, lo);
                const VAddr to = std::min<VAddr>(address + copy.size, hi);
                const u8* before = entry.snapshot.data() + copy.dstOffset;
                for (VAddr byte = from; byte < to; ++byte) {
                    state[byte - lo] = guest[byte - lo] != before[byte - address] ? 2 : 1;
                }
            }
        }
        for (VAddr byte = lo; byte < hi;) {
            if (state[byte - lo] != 2) {
                ++byte;
                continue;
            }
            const VAddr run = byte;
            while (byte < hi && state[byte - lo] == 2) {
                ++byte;
            }
            func(run, byte - run);
        }
    });
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            if (AsyncDownloadPending(addr, size)) {
                // The guest copy of these bytes is older than the host's (AsyncWriteFault).
                std::scoped_lock lock{async_mutex};
                const auto add = [&](VAddr start, u64 length) {
                    copies.emplace_back(total_size_bytes, start, length);
                    total_size_bytes += length;
                };
                async_download_ranges.ForEachNotInRange(addr, size, add);
                ForEachPendingCpuWrite(addr, size, add);
                return;
            }
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    ;
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // ps4rt: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!Ps4RtToggle::Disabled(Ps4RtToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!Ps4RtToggle::Disabled(Ps4RtToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            Ps4RtCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        Ps4RtCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // ps4rt: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !Ps4RtToggle::Disabled(Ps4RtToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // ps4rt: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetSparseQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
