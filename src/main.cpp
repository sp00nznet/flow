/*
 * flOw Recomp — Entry point
 *
 * Initializes the ps3recomp runtime (virtual memory, syscalls, HLE modules),
 * sets up the host window, and jumps to the recompiled game entry point.
 */

#include "config.h"

#include <ps3emu/ps3types.h>
#include <ps3emu/error_codes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Forward declarations for runtime init (from ps3recomp) */
extern "C" {
    /* Virtual memory */
    int  vm_init(void);
    void vm_shutdown(void);

    /* LV2 syscall table */
    struct lv2_syscall_table;
    extern lv2_syscall_table g_lv2_syscalls;
    void lv2_syscall_table_init(lv2_syscall_table* tbl);
    void lv2_register_all_syscalls(lv2_syscall_table* tbl);

    /* PPU context */
    struct ppu_context;
    ppu_context* ppu_context_alloc(void);
    void ppu_context_free(ppu_context* ctx);

    /* Filesystem path mapping */
    void cellfs_set_root_path(const char* path);

    /* Recompiled entry point (generated) */
    void recomp_func_00846AE0(ppu_context* ctx);
}

static void print_banner(void)
{
    printf("================================================\n");
    printf("  flOw - Static Recompilation\n");
    printf("  Title ID: %s\n", FLOW_TITLE_ID);
    printf("  Built with ps3recomp\n");
    printf("================================================\n\n");
}

int main(int argc, char* argv[])
{
    print_banner();

    const char* game_dir = FLOW_GAME_DIR;
    if (argc > 1)
        game_dir = argv[1];

    printf("[init] Game directory: %s\n", game_dir);

    /* Initialize virtual memory (4 GB address space) */
    printf("[init] Virtual memory...\n");
    if (vm_init() != 0) {
        fprintf(stderr, "ERROR: Failed to initialize virtual memory\n");
        return 1;
    }

    /* Initialize LV2 syscall table */
    printf("[init] LV2 syscalls...\n");
    lv2_syscall_table_init(&g_lv2_syscalls);
    lv2_register_all_syscalls(&g_lv2_syscalls);

    /* Set up filesystem path mapping */
    char dev_hdd0[512];
    snprintf(dev_hdd0, sizeof(dev_hdd0), "%s/dev_hdd0", game_dir);
    cellfs_set_root_path(game_dir);

    printf("[init] Filesystem root: %s\n", game_dir);

    /* Allocate PPU context for main thread */
    printf("[init] PPU context...\n");
    ppu_context* ctx = ppu_context_alloc();
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to allocate PPU context\n");
        vm_shutdown();
        return 1;
    }

    /* TODO: Load ELF segments into VM */
    /* TODO: Initialize TLS */
    /* TODO: Set up stack pointer (r1) */
    /* TODO: Create host window for rendering */

    printf("[init] Ready. Jumping to entry point 0x%X...\n\n", FLOW_ENTRY_POINT);

    /* Jump to recompiled game code */
    /* recomp_func_00846AE0(ctx); */

    printf("\n[exit] flOw has exited.\n");

    ppu_context_free(ctx);
    vm_shutdown();
    return 0;
}
