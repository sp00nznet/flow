/*
 * CRT startup tracing — wraps early init functions to find crash point.
 * Compile with FLOW_CRT_TRACE defined to enable.
 */
#ifdef FLOW_CRT_TRACE

#include "recomp/ppu_recomp.h"
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

/* Forward declarations of CRT functions we want to trace */
extern "C" {
    void func_006C2A6C(ppu_context* ctx);
    void func_006C3490(ppu_context* ctx);
    void func_006C2A9C(ppu_context* ctx);
    void func_006C480C(ppu_context* ctx);
    void func_00010200(ppu_context* ctx);
    void func_006B43A4(ppu_context* ctx);
}

static void trace_call(ppu_context* ctx, const char* name, void(*fn)(ppu_context*))
{
    fprintf(stderr, "[TRACE] calling %s (SP=0x%08X, TOC=0x%08X, r3=0x%llX)\n",
            name, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[2],
            (unsigned long long)ctx->gpr[3]);
    fflush(stderr);

#ifdef _WIN32
    __try {
#endif
        fn(ctx);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[TRACE] *** CRASH in %s! Exception 0x%08lX\n",
                name, GetExceptionCode());
        fprintf(stderr, "[TRACE]   SP=0x%08X TOC=0x%08X r3=0x%llX r13=0x%llX\n",
                (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[2],
                (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[13]);
        fflush(stderr);
        ExitProcess(1);
    }
#endif

    fprintf(stderr, "[TRACE] returned from %s (r3=0x%llX)\n",
            name, (unsigned long long)ctx->gpr[3]);
    fflush(stderr);
}

/* Public entry point — called from main instead of recomp_game_main
 * to trace each CRT init step individually. */
extern "C" void crt_trace_main(ppu_context* ctx)
{
    fprintf(stderr, "[TRACE] === CRT trace mode ===\n");

    /* First, run the OPD entry point setup (func_00010230) which calls func_00010254.
     * func_00010254 is the real CRT init that calls all the sub-functions.
     * Instead of tracing through the monolithic function, let's call it
     * normally but with the SEH wrapper in main.cpp catching the crash. */

    /* Actually just run the game entry normally — the SEH in main.cpp
     * will catch it. This file is here for future per-function tracing. */
    extern void recomp_game_main(void* ctx);
    recomp_game_main(ctx);
}

#endif /* FLOW_CRT_TRACE */
