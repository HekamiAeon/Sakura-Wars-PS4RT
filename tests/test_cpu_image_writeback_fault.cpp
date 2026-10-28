// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>

#include "video_core/texture_cache/cpu_writeback.h"

#if defined(__linux__) && defined(__x86_64__)
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr size_t Page = 4096;
constexpr size_t MappingSize = 3 * Page;
constexpr size_t ImageOffset = 0x100;
constexpr unsigned char CpuByte = 0x35;

struct FaultState {
    unsigned char* guest{};
    VideoCore::ImageInfo image{};
    std::array<unsigned char, MappingSize - ImageOffset> gpu{};
    VAddr tracked_begin{}, tracked_end{};
    volatile sig_atomic_t faults = 0;
    bool legacy_whole_image = false;
    bool allow_shared_head_fault = false;
};

FaultState* active_state = nullptr;

void PublishTracked(FaultState& state) {
    const auto range = VideoCore::GetCpuImageWritebackRange(
        state.image, state.tracked_begin, state.tracked_end);
    if (range) {
        std::memcpy(reinterpret_cast<void*>(range->address),
                    state.gpu.data() + (range->address - state.image.guest_address), range->size);
    }
}

void WriteFault(int, siginfo_t* info, void*) {
    auto* state = active_state;
    if (!state) {
        _exit(2);
    }
    const auto address = reinterpret_cast<VAddr>(info->si_addr);
    const auto base = reinterpret_cast<VAddr>(state->guest);
    if (address < base || address >= base + MappingSize) {
        _exit(3);
    }
    state->faults = state->faults + 1;
    if (address < state->image.guest_address) {
        if (!state->allow_shared_head_fault || state->faults != 1) {
            _exit(4);
        }
        // InvalidateMemory's nonoverlap branch calls UntrackImageHead without
        // making the image CpuDirty. The resumed bulk write can touch its head.
        state->tracked_begin = base + Page;
        if (mprotect(state->guest, Page, PROT_READ | PROT_WRITE) != 0) {
            _exit(5);
        }
        return;
    }
    // Model backing-view publication without generating nested test faults.
    if (mprotect(state->guest, MappingSize, PROT_READ | PROT_WRITE) != 0) {
        _exit(6);
    }
    if (state->legacy_whole_image) {
        std::memcpy(reinterpret_cast<void*>(state->image.guest_address), state->gpu.data(),
                    state->image.guest_size);
    } else {
        PublishTracked(*state);
    }
    state->tracked_begin = state->tracked_end = 0;
}

class Fixture {
public:
    Fixture() {
        assert(sysconf(_SC_PAGESIZE) == Page);
        state.guest = static_cast<unsigned char*>(
            mmap(nullptr, MappingSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        assert(state.guest != MAP_FAILED);
        std::fill_n(state.guest, MappingSize, 0x11);
        state.image.guest_address = reinterpret_cast<VAddr>(state.guest) + ImageOffset;
        state.image.guest_size = state.gpu.size();
        state.tracked_begin = state.image.guest_address;
        state.tracked_end = state.image.guest_address + state.image.guest_size;
        constexpr unsigned char Pattern[] = {0x00, 0x7f, 0x7f, 0xff};
        for (size_t i = 0; i < state.gpu.size(); ++i) {
            state.gpu[i] = Pattern[i % 4];
        }
        struct sigaction handler{};
        handler.sa_sigaction = WriteFault;
        handler.sa_flags = SA_SIGINFO;
        sigemptyset(&handler.sa_mask);
        assert(active_state == nullptr);
        assert(sigaction(SIGSEGV, &handler, &previous_handler) == 0);
        active_state = &state;
    }

    ~Fixture() {
        active_state = nullptr;
        assert(sigaction(SIGSEGV, &previous_handler, nullptr) == 0);
        assert(munmap(state.guest, MappingSize) == 0);
    }

    void Protect() {
        assert(mprotect(state.guest, MappingSize, PROT_READ) == 0);
    }

    FaultState state;

private:
    struct sigaction previous_handler{};
};

void TestSharedHead(bool legacy) {
    Fixture fixture;
    auto& state = fixture.state;
    state.legacy_whole_image = legacy;
    state.allow_shared_head_fault = true;
    fixture.Protect();
    auto* destination = state.guest;
    size_t count = MappingSize;
    // Keep the bulk instruction and its restart semantics explicit. Its first
    // fault precedes the image, and its second fault is on the following page.
    asm volatile("rep stosb" : "+D"(destination), "+c"(count) : "a"(CpuByte) : "memory");
    assert(state.faults == 2);
    assert(std::all_of(state.guest, state.guest + ImageOffset,
                       [](auto byte) { return byte == CpuByte; }));
    assert(std::all_of(state.guest + Page, state.guest + MappingSize,
                       [](auto byte) { return byte == CpuByte; }));
    if (legacy) {
        // Negative control: the old algorithm corrupts exactly 0xf00 bytes,
        // matching an image at page+0x100 and a preservation fault on page+0x1000.
        assert(std::equal(state.guest + ImageOffset, state.guest + Page, state.gpu.begin()));
    } else {
        assert(std::all_of(state.guest, state.guest + MappingSize,
                           [](auto byte) { return byte == CpuByte; }));
    }
}

void TestFullyTrackedAlphaEdit() {
    Fixture fixture;
    auto& state = fixture.state;
    fixture.Protect();
    auto* pixels = reinterpret_cast<volatile unsigned char*>(state.image.guest_address);
    for (size_t offset = 3; offset < state.image.guest_size; offset += 4) {
        pixels[offset] = 0x42;
    }
    assert(state.faults == 1);
    const auto* result = reinterpret_cast<const unsigned char*>(state.image.guest_address);
    for (size_t offset = 0; offset < state.image.guest_size; ++offset) {
        assert(result[offset] == (offset % 4 == 3 ? 0x42 : state.gpu[offset]));
    }
}

void TestCpuOwnedTail() {
    Fixture fixture;
    auto& state = fixture.state;
    // A previous adjacent write has untracked the final page. Those bytes now
    // contain newer CPU data; protected bytes still need their GPU contents.
    state.tracked_end = reinterpret_cast<VAddr>(state.guest) + 2 * Page;
    std::fill(state.guest + 2 * Page, state.guest + MappingSize, CpuByte);
    PublishTracked(state);
    const auto protected_size = state.tracked_end - state.image.guest_address;
    assert(std::equal(state.guest + ImageOffset, state.guest + 2 * Page, state.gpu.begin()));
    assert(protected_size > 0);
    assert(std::all_of(state.guest + 2 * Page, state.guest + MappingSize,
                       [](auto byte) { return byte == CpuByte; }));
}

void TestUntrackedDoesNotPublish() {
    Fixture fixture;
    auto& state = fixture.state;
    std::fill_n(state.guest, MappingSize, CpuByte);
    for (const auto& interval : {std::pair<VAddr, VAddr>{0, 0},
                                {state.image.guest_address, state.image.guest_address},
                                {state.tracked_end, state.tracked_end + Page}}) {
        state.tracked_begin = interval.first;
        state.tracked_end = interval.second;
        assert(!VideoCore::GetCpuImageWritebackRange(state.image, interval.first, interval.second));
        PublishTracked(state);
        assert(std::all_of(state.guest, state.guest + MappingSize,
                           [](auto byte) { return byte == CpuByte; }));
    }
}

void TestTrackedBounds() {
    Fixture fixture;
    const auto& image = fixture.state.image;
    const auto end = image.guest_address + image.guest_size;
    const auto clipped = VideoCore::GetCpuImageWritebackRange(
        image, image.guest_address - ImageOffset, end + Page);
    assert(clipped && clipped->address == image.guest_address &&
           clipped->size == image.guest_size);
    auto invalid = image;
    invalid.guest_address = ~VAddr{0} - Page;
    invalid.guest_size = 2 * Page;
    assert(!VideoCore::GetCpuImageWritebackRange(invalid, invalid.guest_address, ~VAddr{0}));
}
} // namespace
#endif

void RunCpuImageWritebackFaultTests() {
#if defined(__linux__) && defined(__x86_64__)
    TestSharedHead(true);
    TestSharedHead(false);
    TestFullyTrackedAlphaEdit();
    TestCpuOwnedTail();
    TestUntrackedDoesNotPublish();
    TestTrackedBounds();
    std::puts("cpu-image-writeback-fault: passed (real faults, legacy negative control, alpha, tail, untracked)");
#else
    std::puts("cpu-image-writeback-fault: skipped (requires Linux x86-64 page faults)");
#endif
}
