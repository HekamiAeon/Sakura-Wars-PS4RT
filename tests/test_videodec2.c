#include "runtime.h"
#include "runtime_videodec2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Resolve the tested table by symbol; full NID resolution is exercised by the
 * running game. No GPU or FFmpeg is needed for compute reservation tests. */
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
#define ERROR(n) ((int32_t)(UINT32_C(0x811d0000)|(n)))
int main(void) {
    int32_t (ABI *query)(Vdec2ComputeMemory *)=(void *)runtime_videodec2_resolve("sceVideodec2QueryComputeMemoryInfo");
    int32_t (ABI *allocate)(const Vdec2ComputeConfig *,const Vdec2ComputeMemory *,void **)=(void *)runtime_videodec2_resolve("sceVideodec2AllocateComputeQueue");
    int32_t (ABI *release)(void *)=(void *)runtime_videodec2_resolve("sceVideodec2ReleaseComputeQueue");
    assert(query && allocate && release);
    Vdec2ComputeMemory memory={0};
    assert(query(NULL)==ERROR(0x102));
    assert(query(&memory)==ERROR(0x101));
    memory.this_size=sizeof(memory);
    assert(query(&memory)==0 && memory.cpu_gpu_memory==NULL);
    assert(memory.cpu_gpu_memory_size==(16u<<20));
    memory.cpu_gpu_memory=malloc(memory.cpu_gpu_memory_size);
    assert(memory.cpu_gpu_memory);
    Vdec2ComputeConfig config={.this_size=sizeof(config)};
    void *queue=(void *)99;
    assert(allocate(NULL,&memory,&queue)==ERROR(0x102) && queue==(void *)99);
    config.this_size=0;
    assert(allocate(&config,&memory,&queue)==ERROR(0x101));
    config.this_size=sizeof(config); config.reserved0=1;
    assert(allocate(&config,&memory,&queue)==ERROR(0x200));
    config.reserved0=0; config.compute_pipe_id=5;
    assert(allocate(&config,&memory,&queue)==ERROR(0x201));
    config.compute_pipe_id=4; config.compute_queue_id=8;
    assert(allocate(&config,&memory,&queue)==ERROR(0x202));
    config.compute_queue_id=7; --memory.cpu_gpu_memory_size;
    assert(allocate(&config,&memory,&queue)==ERROR(0x104));
    ++memory.cpu_gpu_memory_size;
    assert(allocate(&config,&memory,&queue)==0 && queue==memory.cpu_gpu_memory);
    assert(allocate(&config,&memory,&queue)==ERROR(0x110));
    assert(release(NULL)==ERROR(0x110));
    assert(release(queue)==0 && release(queue)==ERROR(0x110));
    assert(allocate(&config,&memory,&queue)==0 && release(queue)==0);
    free(memory.cpu_gpu_memory);
    puts("Videodec2 compute lifecycle passed");
    return 0;
}
