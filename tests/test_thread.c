/* Exercise the libkernel destructor contract through real guest host threads. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

typedef void *(ABI *Entry)(void *);
typedef int32_t (ABI *Create)(void **,void *,Entry,void *,const char *);
typedef int32_t (ABI *Join)(void *,void **);
typedef void (ABI *SetDtors)(GuestCallback);
typedef void (ABI *Exit)(void *);
static Exit guest_exit;
static _Thread_local int phase;
static int expected_phase, calls, cleanups;

/* Isolate the existing memory allocator and key cleanup; the sentinel verifies
 * the destructor runs before the key cleanup hook on the exiting thread. */
void *runtime_low_map(size_t size,int prot) {
    void *p=mmap(NULL,size,prot,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED ? NULL : p;
}
void runtime_thread_keys_cleanup(void) {
    assert(phase==expected_phase);
    ++cleanups;
}
static ABI void first_dtor(void) { assert(phase==0); phase=1; ++calls; }
static ABI void second_dtor(void) { assert(phase==0); phase=2; ++calls; }
static ABI void exiting_dtor(void) {
    assert(phase==0); phase=3; ++calls;
    guest_exit((void *)42);
}
static ABI void *returning_entry(void *argument) { phase=0; return argument; }
static ABI void *exiting_entry(void *argument) { phase=0; guest_exit(argument); return NULL; }
static ABI void *cpu_entry(void *argument) {
    (void)argument;
    phase=0;
    int32_t (ABI *cpu)(void)=(void *)runtime_thread_resolve("g0VTBxfJyu0#libkernel");
    return (void *)(uintptr_t)cpu();
}

static void run(Entry entry,void *expected) {
    Create create=(Create)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    Join join=(Join)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    assert(create && join);
    void *thread=NULL, *result=NULL;
    assert(create(&thread,NULL,entry,(void *)17,"dtor-test")==0);
    assert(join(thread,&result)==0);
    assert(result==expected);
}
int main(void) {
    SetDtors set=(SetDtors)runtime_thread_resolve("rNhWz+lvOMU#libkernel");
    guest_exit=(Exit)runtime_thread_resolve("FJrT5LuUBAU#libkernel");
    assert(set && guest_exit);
    expected_phase=0; run(returning_entry,(void *)17); assert(calls==0);
    set(first_dtor); expected_phase=1;
    run(returning_entry,(void *)17); assert(calls==1);
    run(exiting_entry,(void *)17); assert(calls==2);
    set(second_dtor); expected_phase=2;
    run(returning_entry,(void *)17); assert(calls==3);
    set(exiting_dtor); expected_phase=3;
    run(returning_entry,(void *)42); assert(calls==4);
    set(NULL); expected_phase=0;
    run(exiting_entry,(void *)17); assert(calls==4);
    assert(cleanups==6);
    int32_t (ABI *cpu)(void)=(void *)runtime_thread_resolve("g0VTBxfJyu0#libkernel");
    void *(ABI *self)(void)=(void *)runtime_thread_resolve("EotR8a3ASf4#libkernel");
    int32_t (ABI *set_affinity)(void *,uint64_t)=(void *)runtime_thread_resolve("bt3CTBKmGyI#libkernel");
    int32_t (ABI *attr_init)(void **)=(void *)runtime_thread_resolve("nsYoNRywwNg#libkernel");
    int32_t (ABI *attr_affinity)(void **,uint64_t)=(void *)runtime_thread_resolve("3qxgM4ezETA#libkernel");
    int32_t (ABI *attr_destroy)(void **)=(void *)runtime_thread_resolve("62KCwEMmzcM#libkernel");
    assert(cpu && self && set_affinity && attr_init && attr_affinity && attr_destroy);
    assert(cpu()==0);
    assert(set_affinity(self(),UINT64_C(1)<<6)==0 && cpu()==6);
    assert(set_affinity(self(),0x7f)==0 && cpu()==0);
    void *attr=NULL, *thread=NULL, *result=NULL;
    Create create=(Create)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    Join join=(Join)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    assert(attr_init(&attr)==0 && attr_affinity(&attr,UINT64_C(1)<<5)==0);
    assert(create(&thread,&attr,cpu_entry,NULL,"cpu-test")==0);
    assert(join(thread,&result)==0 && (uintptr_t)result==5);
    assert(attr_destroy(&attr)==0 && cleanups==7);
    puts("Guest thread destructor lifecycle passed");
    return 0;
}
