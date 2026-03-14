/*
 * flOw Recomp — Entry point
 *
 * Initializes the ps3recomp runtime (virtual memory, syscall table,
 * HLE modules), loads ELF data segments, and enters the recompiled
 * game entry point.
 */

#include "config.h"
#include "elf_loader.h"

/* ps3recomp runtime headers */
#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---------------------------------------------------------------------------
 * VM subsystem (inline in header — we include it to get vm_init etc.)
 * -----------------------------------------------------------------------*/
extern "C" {
    #include "runtime/memory/vm.h"
    #include "runtime/ppu/ppu_context.h"
    #include "runtime/syscalls/lv2_syscall_table.h"
    #include "runtime/syscalls/sys_ppu_thread.h"
}

/* ---------------------------------------------------------------------------
 * Runtime globals
 *
 * These are declared 'extern' in ps3recomp headers and must be defined
 * exactly once by the game project.
 * -----------------------------------------------------------------------*/

/* Virtual memory base pointer (set by vm_init).
 * vm.h declares this extern — we must define it exactly once. */
uint8_t* vm_base = nullptr;

/* Also aliased as vm::g_base for C++ vm::ptr<T> */
namespace vm { uint8_t* g_base = nullptr; }

/* LV2 syscall dispatch table (declared extern in lv2_syscall_table.h) */
lv2_syscall_table g_lv2_syscalls;

/* These are defined in the ps3recomp runtime library
 * (sys_fs.c, sys_ppu_thread.c). Declared extern here for access. */
extern "C" char g_sys_fs_root[512];

/* ---------------------------------------------------------------------------
 * Forward declarations for recompiled code (from stubs.cpp or generated)
 * -----------------------------------------------------------------------*/

struct RecompiledFunc {
    uint32_t    guest_addr;
    void      (*host_func)(void* ctx);
};

extern "C" const RecompiledFunc g_recompiled_funcs[];
extern "C" const size_t         g_recompiled_func_count;
extern "C" void recomp_game_main(void* ctx);
extern "C" void flow_register_hle_modules(void);

/* ---------------------------------------------------------------------------
 * Banner
 * -----------------------------------------------------------------------*/

static void print_banner(void)
{
    printf("================================================\n");
    printf("  flOw - Static Recompilation\n");
    printf("  Title ID: %s\n", FLOW_TITLE_ID);
    printf("  Built with ps3recomp\n");
    printf("================================================\n\n");
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(int argc, char* argv[])
{
    print_banner();

    /* Game data directory (override via CLI) */
    const char* game_dir = FLOW_GAME_DIR;
    if (argc > 1)
        game_dir = argv[1];

    printf("[init] Game directory: %s\n\n", game_dir);

    /* 1. Initialize virtual memory (4 GB address space). */
    printf("[init] Virtual memory...\n");
    int32_t vm_rc = vm_init();
    if (vm_rc != CELL_OK) {
        fprintf(stderr, "ERROR: vm_init failed (0x%08X)\n", (unsigned)vm_rc);
        return 1;
    }
    /* Sync the C++ alias */
    vm::g_base = vm_base;

    /* 2. Initialize LV2 syscall table. */
    printf("[init] LV2 syscalls...\n");
    lv2_syscall_table_init(&g_lv2_syscalls);
    lv2_register_all_syscalls(&g_lv2_syscalls);

    /* 3. Register HLE modules (NID-based import resolution). */
    flow_register_hle_modules();

    /* 4. Set up filesystem path mapping. */
    snprintf(g_sys_fs_root, sizeof(g_sys_fs_root), "%s", game_dir);
    printf("[init] Filesystem root: %s\n", g_sys_fs_root);

    /* 4. Initialize stack allocator. */
    vm_stack_alloc_init(&g_vm_stack_alloc);

    /* 5. Load ELF data segments into virtual memory. */
    char elf_path[512];
    snprintf(elf_path, sizeof(elf_path), "%s/EBOOT.elf", game_dir);
    if (!elf_load_segments(elf_path)) {
        printf("[init] Warning: Could not load ELF segments from %s\n", elf_path);
        printf("[init] Recompiled code may crash if it accesses global data\n");
    }

    /* 6. Report recompiled function table. */
    printf("[init] Loaded %zu recompiled functions\n", g_recompiled_func_count);

    /* 7. Set up PPU context and run. */
    printf("[init] Creating PPU context...\n");
    ppu_context ctx;
    ppu_context_init(&ctx);

    /* Allocate stack for main thread */
    uint32_t stack_addr = vm_stack_allocate(&g_vm_stack_alloc, FLOW_STACK_SIZE);
    if (stack_addr) {
        ppu_set_stack(&ctx, stack_addr, FLOW_STACK_SIZE);
        printf("[init] Stack: 0x%08X (%u KB)\n", stack_addr, FLOW_STACK_SIZE / 1024);
    }

    ctx.cia = FLOW_ENTRY_POINT;
    ctx.gpr[2] = 0x008969A8;  /* TOC base from OPD analysis */

    printf("[init] Entering game at 0x%X...\n\n", FLOW_ENTRY_POINT);

    /* Call the recompiled game entry point */
    recomp_game_main(&ctx);

    /* Cleanup */
    printf("\n[exit] flOw has exited. Shutting down...\n");
    vm_shutdown();

    return 0;
}
