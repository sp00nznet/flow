/*
 * flOw Recomp -- Game-specific stubs and overrides
 *
 * This file provides:
 *   1. A placeholder recompiled function table (when no generated code exists)
 *   2. A stub game entry point
 *   3. Game-specific NID overrides and patches for flOw (NPUA80001)
 *
 * When the recompiler generates src/recomp/func_table.cpp, define
 * FLOW_HAS_RECOMP in CMakeLists.txt to exclude the placeholders.
 */

#include "config.h"

#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>

#include <cstdio>

/* ---------------------------------------------------------------------------
 * Placeholder recompiled function table
 *
 * These symbols are provided by the generated func_table.cpp once the
 * recompiler has run.  The placeholders here let the project link and
 * produce a runnable (stub) executable before any recompilation.
 * -----------------------------------------------------------------------*/

#ifndef FLOW_HAS_RECOMP

struct RecompiledFunc {
    uint32_t    guest_addr;
    void      (*host_func)(void* ctx);
    const char* name;
};

extern "C" const RecompiledFunc g_recompiled_funcs[] = {
    { 0, nullptr, nullptr }   /* sentinel */
};

extern "C" const size_t g_recompiled_func_count = 0;

/* Placeholder game main -- replaced by the recompiled entry point. */
extern "C" void recomp_game_main(void* ctx)
{
    (void)ctx;
    printf("[stub] recomp_game_main called -- no recompiled code loaded yet\n");
    printf("[stub] Entry point would be 0x%X\n", FLOW_ENTRY_POINT);
    printf("[stub] Run the recompiler first:\n");
    printf("[stub]   python tools/recompile.py\n");
}

#endif /* !FLOW_HAS_RECOMP */

/* ---------------------------------------------------------------------------
 * flOw-specific NID overrides
 *
 * Uncomment and modify these to override default HLE behavior for
 * functions that need game-specific handling.
 * -----------------------------------------------------------------------*/

/*
 * The unresolved sceNp NID (0xe7dcd3b4) -- stub it to return CELL_OK.
 *
 *   static int32_t stub_sceNp_e7dcd3b4(void* ctx)
 *   {
 *       printf("[stub] sceNp unknown NID e7dcd3b4 called\n");
 *       return CELL_OK;
 *   }
 *
 * Register in init:
 *   ps3::modules::override_nid(0xe7dcd3b4,
 *       reinterpret_cast<void*>(stub_sceNp_e7dcd3b4));
 */

/* ---------------------------------------------------------------------------
 * flOw-specific patches
 *
 * PhyreEngine quirks or checks that need to be bypassed for the recomp.
 * -----------------------------------------------------------------------*/

/*
 * Example: Skip a PhyreEngine platform check
 *   ps3::patches::nop_range(0x00XXXXXX, 0x00XXXXXX);
 *
 * Example: Force a function to return success
 *   ps3::patches::force_return(0x00XXXXXX, 0);
 */
