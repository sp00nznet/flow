/*
 * flOw — Indirect call dispatch (bctrl / bctr)
 *
 * When recompiled code encounters an indirect call (bctrl), the target
 * address is in the CTR register as a GUEST address. We need to look up
 * the corresponding HOST function in the recompiled function table.
 *
 * On the real PS3, bctrl would jump to the address in CTR. In recompiled
 * code, we maintain a hash map from guest addresses to host function
 * pointers, populated from g_recompiled_funcs[].
 */

#include "recomp/ppu_recomp.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

/* The recompiled function table (defined in func_table.cpp) */
struct RecompiledFunc {
    uint32_t    guest_addr;
    void      (*host_func)(void* ctx);
};

extern "C" const RecompiledFunc g_recompiled_funcs[];
extern "C" const size_t g_recompiled_func_count;

/* ---------------------------------------------------------------------------
 * Hash table for fast guest→host dispatch
 * -----------------------------------------------------------------------*/

#define DISPATCH_TABLE_SIZE  (1 << 17)  /* 128K entries — ~512KB */
#define DISPATCH_HASH_MASK   (DISPATCH_TABLE_SIZE - 1)

typedef struct {
    uint32_t guest_addr;
    void   (*host_func)(void* ctx);
} DispatchEntry;

static DispatchEntry s_dispatch[DISPATCH_TABLE_SIZE];
static int s_dispatch_initialized = 0;

static uint32_t hash_addr(uint32_t addr)
{
    /* Simple hash: shift right by 2 (addresses are 4-byte aligned) and mask */
    return (addr >> 2) ^ (addr >> 14) ^ (addr >> 22);
}

static void dispatch_init(void)
{
    if (s_dispatch_initialized) return;

    memset(s_dispatch, 0, sizeof(s_dispatch));

    for (size_t i = 0; i < g_recompiled_func_count; i++) {
        uint32_t addr = g_recompiled_funcs[i].guest_addr;
        void (*func)(void*) = g_recompiled_funcs[i].host_func;

        uint32_t h = hash_addr(addr) & DISPATCH_HASH_MASK;
        /* Linear probing */
        for (uint32_t j = 0; j < DISPATCH_TABLE_SIZE; j++) {
            uint32_t idx = (h + j) & DISPATCH_HASH_MASK;
            if (s_dispatch[idx].guest_addr == 0) {
                s_dispatch[idx].guest_addr = addr;
                s_dispatch[idx].host_func = func;
                break;
            }
        }
    }

    s_dispatch_initialized = 1;
    fprintf(stderr, "[dispatch] Initialized with %zu functions\n",
            g_recompiled_func_count);
}

static void (*dispatch_lookup(uint32_t guest_addr))(void*)
{
    uint32_t h = hash_addr(guest_addr) & DISPATCH_HASH_MASK;
    for (uint32_t j = 0; j < 64; j++) { /* max 64 probes */
        uint32_t idx = (h + j) & DISPATCH_HASH_MASK;
        if (s_dispatch[idx].guest_addr == guest_addr)
            return s_dispatch[idx].host_func;
        if (s_dispatch[idx].guest_addr == 0)
            break;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bctrl dispatcher — called from recompiled code
 *
 * The lifter emits: ((void(*)(ppu_context*))ctx->ctr)(ctx);
 * We replace that with: ps3_indirect_call(ctx);
 * -----------------------------------------------------------------------*/

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    if (!s_dispatch_initialized)
        dispatch_init();

    /* CTR contains the guest address.
     * On PS3, bctrl through an OPD loads the function address from
     * the OPD entry (first word) and the TOC from (second word).
     * The CRT may set CTR to the OPD address or the actual code address. */
    uint32_t target = (uint32_t)ctx->ctr;

    /* Try direct lookup first */
    void (*func)(void*) = dispatch_lookup(target);

    if (!func) {
        /* Maybe CTR points to an OPD (Official Procedure Descriptor).
         * OPD format: [4 bytes function_addr] [4 bytes TOC]
         * Read the function address from the OPD. */
        extern uint8_t* vm_base;
        extern uint32_t vm_read32(uint64_t addr);

        if (target > 0 && target < 0x20000000 && vm_base) {
            uint32_t opd_func = vm_read32(target);
            uint32_t opd_toc  = vm_read32(target + 4);

            func = dispatch_lookup(opd_func);
            if (func) {
                /* Update TOC to the OPD's TOC value */
                ctx->gpr[2] = opd_toc;
                target = opd_func;
            }
        }
    }

    if (func) {
        static int s_log_count = 0;
        static int s_call_count = 0;
        s_call_count++;
        if (s_log_count < 200 || (s_call_count % 1000 == 0)) {
            fprintf(stderr, "[dispatch] bctrl -> 0x%08X (call #%d) [func=%p]\n",
                    target, s_call_count, (void*)func);
            fflush(stderr);
            s_log_count++;
        }
#ifdef _WIN32
        __try {
#endif
        func((void*)ctx);
#ifdef _WIN32
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fprintf(stderr, "[dispatch] CRASH in bctrl 0x%08X (call #%d)\n", target, s_call_count);
            fprintf(stderr, "[dispatch] r3=0x%llX r4=0x%llX SP=0x%X\n",
                    (unsigned long long)ctx->gpr[3],
                    (unsigned long long)ctx->gpr[4],
                    (uint32_t)ctx->gpr[1]);
            fflush(stderr);
            /* Don't re-throw — let execution continue past this constructor */
            return;
        }
#endif
        if (s_call_count <= 20) {
            fprintf(stderr, "[dispatch] returned from 0x%08X\n", target);
            fflush(stderr);
        }
    } else {
        static int s_miss_count = 0;
        if (s_miss_count < 20) {
            fprintf(stderr, "[dispatch] MISS: bctrl -> 0x%08X (not in function table)\n",
                    target);
            s_miss_count++;
        }
        /* Return gracefully — the calling code may check for errors */
    }
}

/* ---------------------------------------------------------------------------
 * bctr dispatcher (unconditional indirect branch, no link)
 * -----------------------------------------------------------------------*/

extern "C" void ps3_indirect_branch(ppu_context* ctx)
{
    /* Same as bctrl but doesn't save LR */
    ps3_indirect_call(ctx);
}
