/* Videodec2 guest ABI, from shadPS4 (GPL-2.0-or-later), Copyright 2024-2026
 * shadPS4 Emulator Project. Native software decoding uses the supplied queue
 * memory as a guest-visible reservation token, without a GPU compute queue. */
#ifndef PS4RT_VIDEODEC2_H
#define PS4RT_VIDEODEC2_H
#include <stdint.h>
typedef struct {
    uint64_t this_size, cpu_gpu_memory_size;
    void *cpu_gpu_memory;
} Vdec2ComputeMemory;
typedef struct {
    uint64_t this_size;
    uint16_t compute_pipe_id, compute_queue_id;
    uint8_t check_memory_type, reserved0;
    uint16_t reserved1;
} Vdec2ComputeConfig;
_Static_assert(sizeof(Vdec2ComputeMemory)==24 && sizeof(Vdec2ComputeConfig)==16,
               "Videodec2 compute layouts");
#endif
