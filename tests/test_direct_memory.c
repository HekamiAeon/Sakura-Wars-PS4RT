/* Physical free-range queries against real PS4RT allocation/release records. */
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#define PAGE UINT64_C(16384)
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))

/* The tested NIDs resolve directly; unrelated named imports and POSIX errno
 * are isolated so this test needs neither a GPU nor a guest process. */
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
int32_t *runtime_errno(void) { static _Thread_local int32_t value; return &value; }

typedef uint64_t (ABI *PoolSize)(void);
typedef int32_t (ABI *Available)(uint64_t,uint64_t,uint64_t,uint64_t *,uint64_t *);
typedef int32_t (ABI *Allocate)(int64_t,int64_t,uint64_t,uint64_t,int,int64_t *);
typedef int32_t (ABI *Release)(uint64_t,uint64_t);

int main(void) {
    PoolSize pool_size=(PoolSize)runtime_memory_resolve("pO96TwzOm5E#libkernel");
    Available available=(Available)runtime_memory_resolve("C0f7TJcbfac#libkernel");
    Allocate allocate=(Allocate)runtime_memory_resolve("rTXw65xmLIA#libkernel");
    Release release=(Release)runtime_memory_resolve("MBuItvba6z8#libkernel");
    assert(pool_size && available && allocate && release);
    uint64_t pool=pool_size(), address=99, size=99;
    assert(available(0,pool,0,&address,&size)==0 && address==0 && size==pool);
    assert(available(0,pool,0,&address,&size)==0 && address==0 && size==pool);
    int64_t b,a,c;
    assert(allocate(4*PAGE,5*PAGE,PAGE,PAGE,0,&b)==0 && b==4*PAGE);
    assert(allocate(PAGE,2*PAGE,PAGE,PAGE,0,&a)==0 && a==PAGE);
    assert(allocate(8*PAGE,9*PAGE,PAGE,PAGE,0,&c)==0 && c==8*PAGE);
    assert(available(0,12*PAGE,0,&address,&size)==0 && address==5*PAGE && size==3*PAGE);
    assert(available(0,12*PAGE,4*PAGE,&address,&size)==0 && address==0 && size==PAGE);
    assert(available(3*PAGE,7*PAGE,2*PAGE,&address,&size)==0 && address==6*PAGE && size==PAGE);
    assert(available(2*PAGE+1,4*PAGE-1,0,&address,&size)==0);
    assert(address==2*PAGE+1 && size==2*PAGE-2);
    assert(available(pool-PAGE,UINT64_MAX,0,&address,&size)==0);
    assert(address==pool-PAGE && size==PAGE);
    address=size=99;
    assert(available(PAGE,2*PAGE,0,&address,&size)==NO_MEMORY && address==99 && size==99);
    assert(available(pool,pool+PAGE,0,&address,&size)==NO_MEMORY);
    assert(available(0,0,0,&address,&size)==INVALID);
    assert(available(0,pool,3*PAGE,&address,&size)==INVALID);
    assert(available(0,pool,0,NULL,&size)==INVALID);
    assert(available(0,pool,0,&address,NULL)==INVALID);
    assert(address==99 && size==99);
    assert(release((uint64_t)a,PAGE)==0);
    assert(release((uint64_t)b,PAGE)==0);
    assert(release((uint64_t)c,PAGE)==0);
    assert(available(0,pool,0,&address,&size)==0 && address==0 && size==pool);
    puts("Direct memory availability passed");
    return 0;
}
