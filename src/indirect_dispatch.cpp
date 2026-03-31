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
#include <csetjmp>
#ifdef _WIN32
#include <windows.h>
#endif

/* The recompiled function table (defined in func_table.cpp) */
struct RecompiledFunc {
    uint32_t    guest_addr;
    void      (*host_func)(void* ctx);
    const char* name;
};

extern "C" const RecompiledFunc g_recompiled_funcs[];
extern "C" const size_t g_recompiled_func_count;

/* ---------------------------------------------------------------------------
 * Hash table for fast guest→host dispatch
 * -----------------------------------------------------------------------*/

#define DISPATCH_TABLE_SIZE  (1 << 18)  /* 256K entries — ~2MB, <50% load for 101K funcs */
#define DISPATCH_HASH_MASK   (DISPATCH_TABLE_SIZE - 1)

typedef struct {
    uint32_t guest_addr;
    void   (*host_func)(void* ctx);
} DispatchEntry;

static DispatchEntry s_dispatch[DISPATCH_TABLE_SIZE];
static int s_dispatch_initialized = 0;

/* ---------------------------------------------------------------------------
 * Trampoline for split-function fallthrough chains
 *
 * Split functions used to call the next fragment directly, creating O(N)
 * native stack depth for chains of thousands of fragments.  Now each
 * fallthrough sets g_trampoline_fn to the next fragment's host function
 * pointer and returns.  Every call site drains the trampoline chain.
 * -----------------------------------------------------------------------*/
/* Thread-local trampoline state — each thread has its own chain */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;
extern "C" __declspec(thread) uint64_t g_trampoline_iters = 0;

extern "C" int g_sp_trace_enabled = 0;
extern "C" uint32_t g_sp_lowest = 0xFFFFFFFF;

static uint32_t hash_addr(uint32_t addr)
{
    /* Simple hash: shift right by 2 (addresses are 4-byte aligned) and mask */
    return (addr >> 2) ^ (addr >> 14) ^ (addr >> 22);
}

/* ---------------------------------------------------------------------------
 * Manual stubs for mid-function entry points not in the function table
 * -----------------------------------------------------------------------*/

/* 0x00100B64: epilogue stub — addi r1,r1,0x90; lwz r3,0(r9); blr */
static void stub_00100B64(void* vctx) {
    ppu_context* ctx = (ppu_context*)vctx;
    ctx->gpr[1] = (int64_t)(int32_t)((uint32_t)ctx->gpr[1] + 0x90);
    ctx->gpr[3] = (int64_t)(int32_t)vm_read32((uint32_t)ctx->gpr[9]);
}

extern uint32_t vm_read32(uint64_t addr);

static void dispatch_register(uint32_t addr, void (*func)(void*))
{
    uint32_t h = hash_addr(addr) & DISPATCH_HASH_MASK;
    for (uint32_t j = 0; j < DISPATCH_TABLE_SIZE; j++) {
        uint32_t idx = (h + j) & DISPATCH_HASH_MASK;
        if (s_dispatch[idx].guest_addr == addr) {
            /* Overwrite existing entry */
            fprintf(stderr, "[dispatch] Overwriting 0x%08X at slot %u (old=%p new=%p)\n",
                    addr, idx, (void*)s_dispatch[idx].host_func, (void*)func);
            s_dispatch[idx].host_func = func;
            return;
        }
        if (s_dispatch[idx].guest_addr == 0) {
            s_dispatch[idx].guest_addr = addr;
            s_dispatch[idx].host_func = func;
            fprintf(stderr, "[dispatch] Registered 0x%08X at slot %u (func=%p)\n",
                    addr, idx, (void*)func);
            return;
        }
    }
    fprintf(stderr, "[dispatch] FAILED to register 0x%08X — table full!\n", addr);
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

    /* Intercept printf (0x006BF0D0) to log caller info */
    dispatch_register(0x006BF0D0, [](void* p) {
        ppu_context* ctx = (ppu_context*)p;
        fprintf(stderr, "[PRINTF-INTERCEPT] LR=0x%08X SP=0x%08X\n",
                (uint32_t)ctx->lr, (uint32_t)ctx->gpr[1]);
        fflush(stderr);

        extern void func_006BF0D0(ppu_context*);
        func_006BF0D0(ctx);
    });

    /* Register manual stubs for mid-function entry points */
    dispatch_register(0x00100B64, stub_00100B64);
    /* Mid-function printf entry � needed for PhyreEngine error messages */
    dispatch_register(0x006BF0D0, (void(*)(void*))func_006BF0D0);
    dispatch_register(0x006BF410, (void(*)(void*))func_006BF410);
    /* Intercept vsnprintf (0x006BCD28) to dump the format string for
     * PhyreEngine assertion messages. Read format from r5 (guest ptr). */
    dispatch_register(0x006BCD28, [](void* p) {
        ppu_context* ctx = (ppu_context*)p;
        extern uint8_t vm_read8(uint64_t addr);
        uint32_t fmt_addr = (uint32_t)ctx->gpr[5];
        char fmt[512] = {};
        if (fmt_addr) {
            for (int i = 0; i < 511; i++) {
                fmt[i] = (char)vm_read8(fmt_addr + i);
                if (!fmt[i]) break;
            }
        }
        static int s_call = 0;
        s_call++;
        fprintf(stderr, "[vsnprintf #%d] fmt_addr=0x%08X fmt='%.200s' r6=0x%llX r7=0x%llX\n",
                s_call, fmt_addr, fmt,
                (unsigned long long)ctx->gpr[6],
                (unsigned long long)ctx->gpr[7]);
        fflush(stderr);
        ctx->gpr[3] = 0;
    });

    s_dispatch_initialized = 1;
    fprintf(stderr, "[dispatch] Initialized with %zu functions + manual stubs\n",
            g_recompiled_func_count);
}

static void (*dispatch_lookup(uint32_t guest_addr))(void*)
{
    uint32_t h = hash_addr(guest_addr) & DISPATCH_HASH_MASK;
    for (uint32_t j = 0; j < 256; j++) { /* max 256 probes */
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

    /* Null target = end of function pointer table or uninitialized CTR */
    if (target == 0) {
        ctx->gpr[3] = 0;
        return;
    }

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
                /* Update TOC to the OPD's TOC value.
                 * Don't overwrite with 0 — preserve caller's TOC. */
                if (opd_toc != 0)
                    ctx->gpr[2] = opd_toc;
                target = opd_func;
            }
        }
    }

    if (func) {
        static int s_log_count = 0;
        static int s_call_count = 0;
        s_call_count++;

        /* No constructor skipping — let them crash (SEH catches).
         * CRT will abort → longjmp redirect to game main. */

        /* Enable SP tracing for constructors known to have issues */
        bool trace_sp = false;

        /* (skip list removed — constructors depend on each other) */
        if (s_log_count < 200 || (s_call_count % 1000 == 0)) {
            fprintf(stderr, "[dispatch] bctrl -> 0x%08X (call #%d) SP=0x%08X [func=%p]\n",
                    target, s_call_count, (uint32_t)ctx->gpr[1], (void*)func);
            fflush(stderr);
            s_log_count++;
        }
        /* Log caller info for printf calls — dump the assertion struct */
        if (target == 0x006BF0D0) {
            extern uint32_t vm_read32(uint64_t addr);
            extern uint8_t vm_read8(uint64_t addr);
            uint32_t struct_addr = (uint32_t)ctx->gpr[4];
            fprintf(stderr, "[dispatch-printf] LR=0x%08X r3=0x%08X r4=0x%08X\n",
                    (uint32_t)ctx->lr, (uint32_t)ctx->gpr[3], struct_addr);
            /* Dump struct contents */
            for (int i = 0; i < 8; i++) {
                fprintf(stderr, "  [%+02d] = 0x%08X\n", i*4, vm_read32(struct_addr + i*4));
            }
            /* Read string at offset 0 (file path) and offset 8 (message) */
            uint32_t file_ptr = vm_read32(struct_addr);
            uint32_t msg_ptr  = vm_read32(struct_addr + 0x8);
            char file_str[128] = {}; char msg_str[128] = {};
            if (file_ptr && file_ptr < 0x20000000) {
                for (int i = 0; i < 127; i++) { file_str[i] = vm_read8(file_ptr + i); if (!file_str[i]) break; }
            }
            if (msg_ptr && msg_ptr < 0x20000000) {
                for (int i = 0; i < 127; i++) { msg_str[i] = vm_read8(msg_ptr + i); if (!msg_str[i]) break; }
            }
            fprintf(stderr, "  file_ptr=0x%08X -> '%s'\n  msg_ptr=0x%08X -> '%s'\n",
                    file_ptr, file_str, msg_ptr, msg_str);
            fflush(stderr);
        }

        /* Intercept CRT printf/vsnprintf — the CRT's FILE* structures are
         * corrupted (fd=0x7474) because CRT stdio wasn't fully initialized.
         * Skip the recompiled CRT code and just return 0 (no output). */
        if (target == 0x006BF0D0 || target == 0x006BCD28) {
            /* printf/vsnprintf: set return value to 0 (chars written) and skip */
            ctx->gpr[3] = 0;
            goto done_dispatch;
        }

        /* Guest SP guard: detect guest stack overflow early */
        {
            uint32_t sp32 = (uint32_t)ctx->gpr[1];
            static uint32_t s_sp_low = 0xFFFFFFFF;
            if (sp32 < s_sp_low) {
                s_sp_low = sp32;
                if (sp32 < 0xD0100000 && sp32 >= 0xD0000000) {
                    fprintf(stderr, "[SP-GUARD] SP=0x%08X dangerously low at call #%d target=0x%08X\n",
                            sp32, s_call_count, target);
                    fflush(stderr);
                }
            }
        }
        {
            /* Save TOC and SP before call — constructors may corrupt these */
            uint64_t saved_toc = ctx->gpr[2];
            uint64_t saved_sp = ctx->gpr[1];
            ppu_context saved_ctx;
#ifdef _WIN32
            saved_ctx = *ctx;
            __try {
#endif
            g_trampoline_fn = nullptr;
            func((void*)ctx);
            /* Trampoline: drain fallthrough chain iteratively */
            { int _tc = 0;
            while (g_trampoline_fn) {
                void (*tfunc)(void*) = g_trampoline_fn;
                g_trampoline_fn = nullptr;
                _tc++;
                tfunc((void*)ctx);
            }
            if (_tc > 0 && s_call_count <= 30) {
                fprintf(stderr, "[trampoline] call #%d: drained %d fallthroughs, SP=0x%llX\n",
                        s_call_count, _tc, (unsigned long long)ctx->gpr[1]);
                fflush(stderr);
            } }
#ifdef _WIN32
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                fprintf(stderr, "[dispatch] CRASH in bctrl 0x%08X (call #%d) - RECOVERING\n",
                        target, s_call_count);
                fflush(stderr);
                /* Restore full context to pre-call state */
                *ctx = saved_ctx;
                ctx->gpr[3] = 0;
                g_trampoline_fn = nullptr;
                if (trace_sp) {
                    fprintf(stderr, "[SP-TRACE] CRASH: 0x%08X SP_start=0x%08X SP_lowest=0x%08X used=%u KB\n",
                            target, (uint32_t)saved_sp, g_sp_lowest,
                            ((uint32_t)saved_sp - g_sp_lowest) / 1024);
                    fflush(stderr);
                    g_sp_trace_enabled = 0;
                }
            }
#endif
            if (trace_sp && g_sp_trace_enabled) {
                fprintf(stderr, "[SP-TRACE] OK: 0x%08X SP_start=0x%08X SP_lowest=0x%08X used=%u KB\n",
                        target, (uint32_t)saved_sp, g_sp_lowest,
                        ((uint32_t)saved_sp - g_sp_lowest) / 1024);
                fflush(stderr);
                g_sp_trace_enabled = 0;
            }
            /* Check if TOC was corrupted by this call */
            if (ctx->gpr[2] != saved_toc && s_call_count >= 20) {
                fprintf(stderr, "[dispatch] TOC corrupted by call #%d: was 0x%llX now 0x%llX\n",
                        s_call_count, (unsigned long long)saved_toc,
                        (unsigned long long)ctx->gpr[2]);
                fflush(stderr);
            }
            /* Always restore TOC to game's known value (single-module) */
            ctx->gpr[2] = 0x008969A8;
            /* Restore SP if it was corrupted (sign extension) */
            if ((ctx->gpr[1] >> 32) != 0 && (ctx->gpr[1] >> 32) != 0xFFFFFFFF)
                ctx->gpr[1] = saved_sp;
        }
        if (s_call_count <= 20) {
            fprintf(stderr, "[dispatch] returned from 0x%08X\n", target);
            fflush(stderr);
        }
done_dispatch: ;
    } else {
        static int s_miss_count = 0;
        if (s_miss_count < 50) {
            fprintf(stderr, "[dispatch] MISS: bctrl -> 0x%08X (CIA=0x%08X LR=0x%08X SP=0x%08X)\n",
                    target, ctx->cia, (uint32_t)ctx->lr, (uint32_t)ctx->gpr[1]);
            s_miss_count++;
        }
        ctx->gpr[3] = 0;  /* Return CELL_OK for unresolvable targets */

        /* Detect spin on unresolvable targets (CRT stuck in constructor
         * table with garbage function pointers).  After enough misses to
         * the same address, trigger the CRT abort redirect. */
        static uint32_t s_last_miss = 0;
        static int s_repeat_count = 0;
        if (target == s_last_miss) {
            s_repeat_count++;
            if (s_repeat_count > 5) {
                extern jmp_buf g_abort_jmp;
                extern int g_abort_redirect;
                if (g_abort_redirect == 0) {
                    fprintf(stderr, "[dispatch] Spin detected on bctrl -> 0x%08X, aborting CRT\n", target);
                    g_abort_redirect = 1;
                    longjmp(g_abort_jmp, 1);
                }
            }
        } else {
            s_last_miss = target;
            s_repeat_count = 0;
        }
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

/* ---------------------------------------------------------------------------
 * Trampoline runner — call a function and drain fallthrough chain
 *
 * Used by main.cpp for the entry point call.
 * -----------------------------------------------------------------------*/

extern "C" void ps3_trampoline_run(ppu_context* ctx, void (*func)(void*))
{
    if (!s_dispatch_initialized)
        dispatch_init();

    g_trampoline_fn = nullptr;
    func((void*)ctx);

    while (g_trampoline_fn) {
        void (*tfunc)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tfunc((void*)ctx);
    }
}

/* ---------------------------------------------------------------------------
 * Thread entry trampoline — called by the runtime when a new PPU thread
 * starts.  Looks up the entry address (ctx->cia) in the dispatch table
 * and runs it with the trampoline chain.
 * -----------------------------------------------------------------------*/

extern "C" void ps3_thread_entry(ppu_context* ctx)
{
    if (!s_dispatch_initialized)
        dispatch_init();

    uint32_t entry = (uint32_t)ctx->cia;

    /* Try direct lookup */
    void (*func)(void*) = dispatch_lookup(entry);

    if (!func) {
        /* Try OPD resolution */
        extern uint8_t* vm_base;
        extern uint32_t vm_read32(uint64_t addr);
        if (entry > 0 && entry < 0x20000000 && vm_base) {
            uint32_t opd_func = vm_read32(entry);
            uint32_t opd_toc  = vm_read32(entry + 4);
            func = dispatch_lookup(opd_func);
            if (func) {
                if (opd_toc != 0)
                    ctx->gpr[2] = opd_toc;
                entry = opd_func;
            }
        }
    }

    if (func) {
        fprintf(stderr, "[thread] Thread %llu starting at 0x%08X [func=%p]\n",
                (unsigned long long)ctx->thread_id, entry, (void*)func);
        fflush(stderr);
        ps3_trampoline_run(ctx, func);
        fprintf(stderr, "[thread] Thread %llu exited (r3=0x%llX)\n",
                (unsigned long long)ctx->thread_id,
                (unsigned long long)ctx->gpr[3]);
        fflush(stderr);
    } else {
        fprintf(stderr, "[thread] Thread %llu: entry 0x%08X not found!\n",
                (unsigned long long)ctx->thread_id, entry);
        fflush(stderr);
    }
}
