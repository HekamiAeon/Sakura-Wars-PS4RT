/* Videodec2 compatibility, incrementally adapted from the existing FFmpeg
 * decoder. ABI/error values from shadPS4, GPL-2.0-or-later, Copyright 2024-2026
 * shadPS4 Emulator Project. */
#include "runtime.h"
#include "runtime_videodec2.h"
#include <pthread.h>
#define V2_ERROR(n) ((int32_t)(UINT32_C(0x811d0000)|(n)))
#define COMPUTE_MEMORY_SIZE (UINT64_C(16)<<20)
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static void *compute_queues[16];

static ABI int32_t query_compute(Vdec2ComputeMemory *memory) {
    if (!memory) return V2_ERROR(0x102);
    if (memory->this_size!=sizeof(*memory)) return V2_ERROR(0x101);
    memory->cpu_gpu_memory=NULL;
    memory->cpu_gpu_memory_size=COMPUTE_MEMORY_SIZE;
    return 0;
}
static ABI int32_t allocate_compute(const Vdec2ComputeConfig *config,
                                   const Vdec2ComputeMemory *memory,void **queue) {
    if (!config || !memory || !queue) return V2_ERROR(0x102);
    if (config->this_size!=sizeof(*config) || memory->this_size!=sizeof(*memory))
        return V2_ERROR(0x101);
    if (config->reserved0 || config->reserved1) return V2_ERROR(0x200);
    if (config->compute_pipe_id>4) return V2_ERROR(0x201);
    if (config->compute_queue_id>7) return V2_ERROR(0x202);
    if (!memory->cpu_gpu_memory) return V2_ERROR(0x105);
    if (memory->cpu_gpu_memory_size<COMPUTE_MEMORY_SIZE) return V2_ERROR(0x104);
    pthread_mutex_lock(&queue_lock);
    int free_slot=-1;
    for (int i=0;i<16;++i) {
        if (compute_queues[i]==memory->cpu_gpu_memory) {
            pthread_mutex_unlock(&queue_lock); return V2_ERROR(0x110);
        }
        if (!compute_queues[i] && free_slot<0) free_slot=i;
    }
    if (free_slot<0) { pthread_mutex_unlock(&queue_lock); return V2_ERROR(0x100); }
    compute_queues[free_slot]=memory->cpu_gpu_memory;
    *queue=memory->cpu_gpu_memory;
    pthread_mutex_unlock(&queue_lock);
    return 0;
}
static ABI int32_t release_compute(void *queue) {
    if (!queue) return V2_ERROR(0x110);
    pthread_mutex_lock(&queue_lock);
    for (int i=0;i<16;++i) if (compute_queues[i]==queue) {
        compute_queues[i]=NULL;
        pthread_mutex_unlock(&queue_lock); return 0;
    }
    pthread_mutex_unlock(&queue_lock);
    return V2_ERROR(0x110);
}
static const RuntimeExport exports[]={
    {"sceVideodec2QueryComputeMemoryInfo",query_compute},
    {"sceVideodec2AllocateComputeQueue",allocate_compute},
    {"sceVideodec2ReleaseComputeQueue",release_compute},
};
uintptr_t runtime_videodec2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
