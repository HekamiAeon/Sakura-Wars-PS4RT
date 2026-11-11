
// ps4rt: glue between the C loader and the vendored shadPS4 video core.
#include "ps4rt_overlay.h"
#include "ps4rt_png.h"
#include "ps4rt_settings.h"
#include "ps4rt_gpu_workarounds.h"
#include "ps4rt_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "ps4rt_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"

#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../ps4rtgpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
// runtime.c: exit counters (and the ISA site report), as on the guest's own exit
void runtime_report(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterKernelEventFlag(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const Ps4RtGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // ps4rt: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !Ps4RtCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    Ps4RtCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    ;
    const bool written = runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
    // The write skipped page tracking: textures over it must reload (#138).
    // ponytail: fence and label writes (<= 8 bytes) never hold texels, so they are left out;
    // every one would send all texture bindings through the locked path.
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(reinterpret_cast<VAddr>(address), size);
    }
    return written;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {

    const u64 pages = (size + 4095) / 4096;
    ;
    ;
    if (!True(perms & MemoryPermission::Write)) {
        ;
        ;
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 Ps4RtDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#ifdef PS4RT_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (PS4RT_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("ps4rt:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int ps4rtgpu_init(const Ps4RtGpuConfig* config) {
    const char* bc6_decode = std::getenv("PS4RT_BC6_FLOAT");
    const char* wqm_helpers = std::getenv("PS4RT_WQM_HELPERS");

    const char* transition_snapshot = std::getenv("PS4RT_TRANSITION_SNAPSHOT");
    Ps4RtGpuWorkarounds::Configure(bc6_decode ? bc6_decode : "",
                              wqm_helpers && std::strcmp(wqm_helpers, "1") == 0,
                              false,
                              transition_snapshot && std::strcmp(transition_snapshot, "1") == 0);
    if (Ps4RtGpuWorkarounds::Current().preserve_transition_snapshot)
        std::puts("PS4RT: GPU transition snapshot preservation enabled");
    
    
    
    Ps4RtSettings::Load();
#ifdef PS4RT_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("PS4RT_GPU_USER_DIR")) _putenv_s("PS4RT_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("PS4RT_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "PS4RT";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("ps4rt:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        runtime_report();
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void ps4rtgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::Kernel::RegisterKernelEventFlag(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t ps4rtgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

// ps4rt: bytes the faulting write is part of overwriting entirely, so any GPU data in them is
// dead (0, 0: unknown); read by BufferCache::ReadMemory and the texture cache's CPU write
// preservation on the faulting thread.
thread_local unsigned long long ps4rt_fault_discard_lo = 0, ps4rt_fault_discard_hi = 0;
// The replacing copy running on this thread (a new buffer's data or a whole-surface texture
// tile, below). volatile: only the fault handler reads it, so the compiler would otherwise drop
// the stores around the copy.
static thread_local volatile unsigned long long new_buffer_lo = 0, new_buffer_hi = 0;

// ps4rt: Sakura Wars' RsdxDevice11::CreateBuffer copies the initial data of a new buffer into
// memory freshly allocated from the GPU heap, which the GPU often wrote while it belonged to an
// earlier buffer; the loader routes that one memcpy here. These copies are ~99% of the write
// faults on GPU-written pages, and each used to download the GPU bytes around it and wait for
// the GPU (Scheduler::Finish: 50 FPS and a slowed game at 4K). The old GPU bytes under the new
// buffer are dead, so the fault drops them instead (BufferCache::ReadMemory).
extern "C" void* ps4rtgpu_new_buffer_copy(void* dst, const void* src, uint64_t size) {
    if (auto* memory = Core::Memory::Instance(); memory && memory->GetRasterizer() && size) {
        // Asynchronous downloads queued before must not land on the new data.
        memory->GetRasterizer()->GetBufferCache().DropPendingAsync(
            reinterpret_cast<unsigned long long>(dst), size);
    }
    new_buffer_lo = reinterpret_cast<unsigned long long>(dst);
    new_buffer_hi = new_buffer_lo + size;
    void* result = std::memcpy(dst, src, size);
    new_buffer_lo = new_buffer_hi = 0;
    return result;
}

// ps4rt: Sakura Wars uploads its textures (text lines and labels too) with the PS4 SDK's
// GpuAddress::tileSurfaceRegion, linked into the game, often into memory that held an earlier
// GPU-rendered surface; the loader routes its two calls of a region loop (1D and 2D tiling)
// here (args: the internal tiling parameters, the tiled destination, the linear source, the
// SurfaceRegion {left, top, front, right, bottom, back} and the source pitches). A region that
// covers the whole surface rewrites all of its pixels, so any GPU data in its bytes is dead:
// the write faults inside skip the GPU wait like the new-buffer copies above (the padding
// pixels the loop skips are never sampled).
using TileRegionLoop = int (*)(const void*, void*, const void*, const uint32_t*, uint32_t,
                               uint32_t);
static TileRegionLoop tile_region_loop = nullptr;

extern "C" void ps4rtgpu_set_tile_region(uintptr_t loop) {
    tile_region_loop = reinterpret_cast<TileRegionLoop>(loop);
}

static int TileSurfaceRegion(TileRegionLoop loop, const void* params, void* dst,
                             const void* src, const uint32_t* region, uint32_t src_pitch,
                             uint32_t src_slice_pitch) {
    // The internal parameters (seen in Sakura's calls, the same for both loops): u32 words 3-5
    // the surface's width, height and depth in elements (the SDK's whole-surface tile builds its
    // region from them), 6-8 the same padded to whole tiles, 9 the bits per element, 10-11 the
    // linear size and 12-13 the tiled size in bytes (u64). The tiled size from `dst` is the
    // memory the new surface occupies; anything else leaves the faults as they were.
    const auto* surface = static_cast<const uint32_t*>(params);
    const bool whole = params && dst && region && region[0] == 0 && region[1] == 0 &&
                       region[2] == 0 && region[3] == surface[3] && region[4] == surface[4] &&
                       region[5] == surface[5] && surface[5] == 1;
    unsigned long long bytes = 0;
    if (whole) {
        using U64 = unsigned long long;
        const U64 linear = surface[10] | (U64(surface[11]) << 32);
        const U64 tiled = surface[12] | (U64(surface[13]) << 32);
        const uint32_t bits = surface[9];
        const bool sane = (bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128) &&
                          surface[6] >= surface[3] && surface[7] >= surface[4] &&
                          linear == U64(surface[3]) * surface[4] * bits / 8 &&
                          tiled == U64(surface[6]) * surface[7] * bits / 8 &&
                          tiled <= (256ULL << 20);
        if (sane) {
            bytes = tiled;
        }
    }
    if (bytes) {
        if (auto* memory = Core::Memory::Instance(); memory && memory->GetRasterizer()) {
            memory->GetRasterizer()->GetBufferCache().DropPendingAsync(
                reinterpret_cast<unsigned long long>(dst), bytes);
        }
    }
    if (bytes) {
        new_buffer_lo = reinterpret_cast<unsigned long long>(dst);
        new_buffer_hi = new_buffer_lo + bytes;
    }
    const int result = loop(params, dst, src, region, src_pitch, src_slice_pitch);
    new_buffer_lo = new_buffer_hi = 0;
    return result;
}

extern "C" int ps4rtgpu_tile_surface_region(const void* params, void* dst, const void* src,
                                            const uint32_t* region, uint32_t src_pitch,
                                            uint32_t src_slice_pitch) {
    return TileSurfaceRegion(tile_region_loop, params, dst, src, region, src_pitch,
                             src_slice_pitch);
}

// The same for tileSurfaceRegion's 2D-tiled loop (array modes 4-15).
static TileRegionLoop tile_region_loop_2d = nullptr;

extern "C" void ps4rtgpu_set_tile_region_2d(uintptr_t loop) {
    tile_region_loop_2d = reinterpret_cast<TileRegionLoop>(loop);
}

extern "C" int ps4rtgpu_tile_surface_region_2d(const void* params, void* dst, const void* src,
                                               const uint32_t* region, uint32_t src_pitch,
                                               uint32_t src_slice_pitch) {
    return TileSurfaceRegion(tile_region_loop_2d, params, dst, src, region, src_pitch,
                             src_slice_pitch);
}

extern "C" int ps4rtgpu_handle_fault(void* ucontext, void* address) {
    ps4rt_fault_discard_lo = ps4rt_fault_discard_hi = 0;
    if (const auto fault = reinterpret_cast<unsigned long long>(address);
        fault >= new_buffer_lo && fault < new_buffer_hi) {
        ps4rt_fault_discard_lo = new_buffer_lo;
        ps4rt_fault_discard_hi = new_buffer_hi;
    }
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned ps4rtgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("ps4rt:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

extern "C" int ps4rtgpu_vibration_percent(void) {
    return Ps4RtSettings::Get().vibration.load(std::memory_order_relaxed);
}

extern "C" int ps4rtgpu_overlay_captures_input(void) {
    return Ps4RtOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int ps4rtgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int ps4rtgpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                    int count, int focus) {
    if (!g_window || count <= 0) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<Ps4RtPng::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !Ps4RtPng::LoadRgba(images[i], pictures[i])) {
            LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}", images[i]);
        }
    }
    Ps4RtOverlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                             std::move(pictures), focus);
    return 1;
}

extern "C" int ps4rtgpu_choice_poll(void) {
    return Ps4RtOverlay::PollChoice();
}

extern "C" int ps4rtgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
