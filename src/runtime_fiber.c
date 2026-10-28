/* SPDX-FileCopyrightText: Copyright 2024-2025 shadPS4 Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * PS4 fiber contracts adapted from the pinned shadPS4 fiber implementation.
 * Linux SysV context switching uses the caller's guest stack. Explicit-stack
 * fibers suspend/resume; stackless fibers restart their entry after yielding.
 * No host scheduler or signal-mask changes. TLS belongs to the running thread.
 */
#include "runtime.h"
#ifndef _WIN32
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FERR(n) ((int32_t)(UINT32_C(0x80590000)|(n)))
#define MAGIC_START UINT32_C(0xdef1649c)
#define MAGIC_END UINT32_C(0xb37592a0)
#define STACK_MAGIC UINT64_C(0x7149f2ca7149f2ca)
enum { RUNNING=1, IDLE=2, FINALIZED=3 };
typedef void (ABI *FiberEntry)(uint64_t,uint64_t);
typedef struct { uint64_t rbx,rbp,r12,r13,r14,r15,rsp,rip; uint16_t cw,pad; uint32_t mxcsr; } FiberContext;
typedef struct Fiber {
    uint32_t magic_start;
    _Atomic uint32_t state;
    FiberEntry entry;
    uint64_t initial;
    void *stack;
    uint64_t stack_size;
    char name[32];
    FiberContext *context;
    uint32_t flags;
    void *context_start,*context_end;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)==112 && offsetof(Fiber,context)==72 && offsetof(Fiber,magic_end)==104,"fiber ABI");
_Static_assert(offsetof(FiberContext,cw)==64 && offsetof(FiberContext,mxcsr)==68,"switch ABI");
typedef struct {
    FiberContext thread;
    Fiber *current,*previous;
    uint64_t run_arg,return_arg;
} FiberFrame;
static _Thread_local FiberFrame *active_frame;
static _Atomic uint64_t initialized,runs,switches,yields,resumes;
void runtime_fiber_report(void) {
    printf("Runtime: fibers initialized=%llu, runs=%llu, switches=%llu, yields=%llu, resumes=%llu\n",
           (unsigned long long)initialized,(unsigned long long)runs,(unsigned long long)switches,
           (unsigned long long)yields,(unsigned long long)resumes);
}

extern int ABI runtime_fiber_save(FiberContext *) __attribute__((returns_twice));
extern void ABI runtime_fiber_restore(FiberContext *) __attribute__((noreturn));
extern void ABI runtime_fiber_start(FiberEntry,uint64_t,uint64_t,void *,_Atomic uint32_t *,int) __attribute__((noreturn));
void ABI runtime_fiber_returned(void) __attribute__((noreturn));
void ABI runtime_fiber_returned(void) {
    fputs("STOP: fiber entry unexpectedly returned\n",stderr);
    fflush(NULL); _Exit(23);
}
/* Save only SysV callee-preserved registers and FP control. XMM registers are
 * caller-saved. Separate assembly avoids moving RSP inside a C stack frame. */
__asm__(".text\n"
        ".type runtime_fiber_save,@function\n"
        ".globl runtime_fiber_save\nruntime_fiber_save:\n"
        "mov %rbx,0(%rdi)\nmov %rbp,8(%rdi)\nmov %r12,16(%rdi)\nmov %r13,24(%rdi)\n"
        "mov %r14,32(%rdi)\nmov %r15,40(%rdi)\nlea 8(%rsp),%rax\nmov %rax,48(%rdi)\n"
        "mov (%rsp),%rax\nmov %rax,56(%rdi)\nfnstcw 64(%rdi)\nstmxcsr 68(%rdi)\nxor %eax,%eax\nret\n"
        ".size runtime_fiber_save,.-runtime_fiber_save\n"
        ".type runtime_fiber_restore,@function\n"
        ".globl runtime_fiber_restore\nruntime_fiber_restore:\n"
        "fldcw 64(%rdi)\nldmxcsr 68(%rdi)\n"
        "mov 0(%rdi),%rbx\nmov 8(%rdi),%rbp\nmov 16(%rdi),%r12\nmov 24(%rdi),%r13\n"
        "mov 32(%rdi),%r14\nmov 40(%rdi),%r15\nmov 48(%rdi),%rsp\nmov $1,%eax\njmp *56(%rdi)\n"
        ".size runtime_fiber_restore,.-runtime_fiber_restore\n"
        ".type runtime_fiber_start,@function\n"
        ".globl runtime_fiber_start\nruntime_fiber_start:\n"
        "mov %rdi,%r11\nmov %rsi,%rdi\nmov %rdx,%rsi\nmov %rcx,%rsp\nand $-16,%rsp\nxor %ebp,%ebp\n"
        "test %r8,%r8\njz 1f\nmovl $2,(%r8)\n1:\n"
        "test %r9d,%r9d\njz 2f\nmovw $0x37f,-16(%rsp)\nfldcw -16(%rsp)\n"
        "movl $0x9fc0,-12(%rsp)\nldmxcsr -12(%rsp)\n2:\n"
        "call *%r11\ncall runtime_fiber_returned\nud2\n"
        ".size runtime_fiber_start,.-runtime_fiber_start\n");

static int32_t validate(Fiber *fiber) {
    if (!fiber) return FERR(1);
    if ((uintptr_t)fiber&7) return FERR(2);
    if (fiber->magic_start!=MAGIC_START || fiber->magic_end!=MAGIC_END) return FERR(4);
    return 0;
}
static void check_stack(Fiber *fiber) {
    if (fiber->stack && *(uint64_t *)fiber->stack!=STACK_MAGIC) {
        fputs("STOP: fiber stack overflow detected\n",stderr);
        fflush(NULL); _Exit(23);
    }
}
static void finish_switch(void) {
    if (active_frame->previous) {
        atomic_store_explicit(&active_frame->previous->state,IDLE,memory_order_release);
        active_frame->previous=NULL;
    }
}
static void dispatch(Fiber *fiber,uint64_t arg) __attribute__((noreturn));
static void dispatch(Fiber *fiber,uint64_t arg) {
    FiberFrame *frame=active_frame;
    frame->current=fiber;
    frame->run_arg=arg;
    if (fiber->stack && fiber->context) {
        atomic_fetch_add(&resumes,1);
        runtime_fiber_restore(fiber->context);
    }
    _Atomic uint32_t *previous=frame->previous ? &frame->previous->state : NULL;
    frame->previous=NULL;
    void *top=fiber->stack ? (unsigned char *)fiber->stack+fiber->stack_size : (void *)frame->thread.rsp;
    runtime_fiber_start(fiber->entry,fiber->initial,arg,top,previous,!!(fiber->flags&0x100));
}
static ABI int32_t fiber_initialize(Fiber *fiber,const char *name,FiberEntry entry,uint64_t arg,
                                   void *stack,uint64_t size,const uint32_t *opt,uint32_t version) {
    if (!fiber || !name || !entry) return FERR(1);
    if (((uintptr_t)fiber&7) || ((uintptr_t)stack&15) || ((uintptr_t)opt&7)) return FERR(2);
    if (size && size<512) return FERR(3);
    if ((size&15) || (!!stack!=!!size) || (stack && size>UINTPTR_MAX-(uintptr_t)stack)) return FERR(4);
    if (opt && *opt!=UINT32_C(0xbb40e64d)) return FERR(4);
    memset(fiber,0,sizeof(*fiber));
    fiber->entry=entry; fiber->initial=arg; fiber->stack=stack; fiber->stack_size=size;
    strncpy(fiber->name,name,31);
    fiber->flags=version>=0x3500000 ? 0x100 : 0;
    fiber->context_start=stack;
    fiber->context_end=stack ? (unsigned char *)stack+size : NULL;
    fiber->magic_start=MAGIC_START; fiber->magic_end=MAGIC_END;
    if (stack) *(uint64_t *)stack=STACK_MAGIC;
    atomic_init(&fiber->state,IDLE);
    atomic_fetch_add(&initialized,1);
    return 0;
}
static ABI int32_t fiber_finalize(Fiber *fiber) {
    int32_t error=validate(fiber); if (error) return error;
    uint32_t expected=IDLE;
    return atomic_compare_exchange_strong(&fiber->state,&expected,FINALIZED) ? 0 : FERR(6);
}
static ABI int32_t fiber_self(Fiber **out) {
    if (!out) return FERR(1);
    if (!active_frame) return FERR(5);
    *out=active_frame->current; return 0;
}
static ABI int32_t fiber_run(Fiber *fiber,uint64_t arg,uint64_t *out) {
    int32_t error=validate(fiber); if (error) return error;
    if (active_frame) return FERR(5);
    uint32_t expected=IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&expected,RUNNING)) return FERR(6);
    atomic_fetch_add(&runs,1);
    FiberFrame frame={0}; frame.current=fiber;
    active_frame=&frame;
    if (!runtime_fiber_save(&frame.thread)) dispatch(fiber,arg);
    /* Read TLS again: a suspended fiber can resume on a different host thread. */
    FiberFrame *returned=active_frame;
    atomic_store_explicit(&returned->current->state,IDLE,memory_order_release);
    if (out) *out=returned->return_arg;
    active_frame=NULL;
    return 0;
}
static ABI int32_t fiber_switch(Fiber *fiber,uint64_t arg,uint64_t *out) {
    int32_t error=validate(fiber); if (error) return error;
    if (!active_frame) return FERR(5);
    uint32_t expected=IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&expected,RUNNING)) return FERR(6);
    Fiber *current=active_frame->current;
    FiberContext context;
    if (current->stack && runtime_fiber_save(&context)) {
        finish_switch();
        if (out) *out=active_frame->run_arg;
        return 0;
    }
    current->context=current->stack ? &context : NULL;
    check_stack(current);
    atomic_fetch_add(&switches,1);
    active_frame->previous=current;
    dispatch(fiber,arg);
}
static ABI int32_t fiber_return(uint64_t arg,uint64_t *out) {
    if (!active_frame) return FERR(5);
    Fiber *current=active_frame->current;
    FiberContext context;
    if (current->stack && runtime_fiber_save(&context)) {
        finish_switch();
        if (out) *out=active_frame->run_arg;
        return 0;
    }
    current->context=current->stack ? &context : NULL;
    check_stack(current);
    atomic_fetch_add(&yields,1);
    active_frame->return_arg=arg;
    runtime_fiber_restore(&active_frame->thread);
}
static const RuntimeExport exports[]={
    {"sceFiberInitialize",fiber_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberGetSelf",fiber_self}, {"sceFiberRun",fiber_run},
    {"sceFiberSwitch",fiber_switch}, {"sceFiberReturnToThread",fiber_return},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
#else
uintptr_t runtime_fiber_resolve(const char *name) { (void)name; return 0; }
void runtime_fiber_report(void) {}
#endif
