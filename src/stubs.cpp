/*
 * flOw Recomp -- Game-specific stubs and overrides
 *
 * This file provides:
 *   1. A placeholder recompiled function table (until the recompiler generates one)
 *   2. A stub game entry point
 *   3. Game-specific NID overrides and patches for flOw (NPUA80001)
 *
 * Once the recompiler generates real code, the function table and entry point
 * in this file will be superseded by the generated versions.
 */

#include "config.h"

#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>

#include <cstdio>

/* ---------------------------------------------------------------------------
 * Placeholder recompiled function table
 *
 * The recompiler generates this in recomp/func_table.cpp.  This placeholder
 * lets the project link and run before any functions have been recompiled.
 * -----------------------------------------------------------------------*/

struct RecompiledFunc {
    uint32_t    guest_addr;
    void      (*host_func)(void* ctx);
};

extern "C" const RecompiledFunc g_recompiled_funcs[] = {
    /* { 0x00846AE0, recomp_func_00846AE0 }, */
    { 0, nullptr }   /* sentinel */
};

extern "C" const size_t g_recompiled_func_count = 0;

/* Placeholder game main -- replaced by the recompiled entry point. */
extern "C" void recomp_game_main(void* ctx)
{
    (void)ctx;
    printf("[stub] recomp_game_main called -- no recompiled code loaded yet\n");
    printf("[stub] Entry point would be 0x%X\n", FLOW_ENTRY_POINT);
    printf("[stub] Run the recompiler first:\n");
    printf("[stub]   python tools/recompile.py game/EBOOT.elf\n");
}

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
