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

/* VM bridge functions */
extern "C" void vm_write32(uint64_t addr, uint32_t val);
extern "C" uint32_t vm_read32(uint64_t addr);

/* Guest malloc HLE (from malloc_override.cpp) */
extern "C" void hle_guest_malloc(ppu_context* ctx);
extern "C" void hle_guest_free(ppu_context* ctx);
extern "C" void hle_guest_calloc(ppu_context* ctx);
extern "C" void hle_guest_realloc(ppu_context* ctx);

/* Recompiled function entry (from func_table.cpp) */
extern "C" void func_006B738C(ppu_context* ctx);  /* malloc */

/* RSX null backend (from ps3recomp runtime) */
extern "C" int rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_shutdown(void);
extern "C" int rsx_null_backend_pump_messages(void);

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


#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "[CRASH-VEH] AV at RIP=%p addr=%p\n",
                (void*)ep->ContextRecord->Rip,
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
int main(int argc, char* argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_handler);
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[boot] flow.exe starting...\n");
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
    fprintf(stderr, "[init] vm_base = %p\n", (void*)vm_base);

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

    /* 5. Commit memory regions.
     * The PS3 has 256MB main RAM. We commit:
     *   - BSS/heap area (0x00900000 - 0x10000000) — CRT malloc uses this
     *   - RSX region (0x10000000 - 0x20000000) — ELF rodata + RSX local mem
     *   - Extra heap (0x20000000 - 0x30000000) — additional malloc space
     */
    {
        /* Low memory guard + ELF region (0x00000000 - 0x00900000) */
        vm_commit(0x00000000, 0x00900000);

        /* Main heap / BSS region */
        int32_t heap_rc = vm_commit(0x00900000, 0x10000000 - 0x00900000);
        if (heap_rc != CELL_OK) {
            fprintf(stderr, "WARNING: Failed to commit heap region (0x%08X)\n", (unsigned)heap_rc);
        } else {
            printf("[init] Heap region committed: 0x00900000 - 0x10000000 (%u MB)\n",
                   (0x10000000 - 0x00900000) / (1024 * 1024));
        }

        /* RSX region (rodata, data, local memory) */
        int32_t rsx_rc = vm_commit(VM_RSX_BASE, VM_RSX_SIZE);
        if (rsx_rc != CELL_OK) {
            fprintf(stderr, "ERROR: Failed to commit RSX region (0x%08X)\n", (unsigned)rsx_rc);
            return 1;
        }
        printf("[init] RSX region committed: 0x%08X - 0x%08X\n",
               VM_RSX_BASE, VM_RSX_BASE + VM_RSX_SIZE);

        /* Extra heap for sys_memory_allocate */
        int32_t extra_rc = vm_commit(0x20000000, 0x10000000);
        if (extra_rc == CELL_OK) {
            printf("[init] Extra heap committed: 0x20000000 - 0x30000000 (256 MB)\n");
        }

        /* General purpose region (0x30000000 - 0xC0000000) for game allocations */
        vm_commit(0x30000000, 0x90000000);
        printf("[init] General region committed: 0x30000000 - 0xC0000000\n");

        /* RSX VRAM region — commit in chunks to avoid huge single allocation.
         * PS3 maps RSX local memory at high addresses. */
        vm_commit(0xC0000000, 0x10000000); /* 0xC0000000 - 0xD0000000 */
        vm_commit(0xE0000000, 0x10000000); /* 0xE0000000 - 0xF0000000 */
        vm_commit(0xF0000000, 0x0FFF0000); /* 0xF0000000 - 0xFFFF0000 */
        printf("[init] RSX VRAM committed: 0xC0-0xD0, 0xE0-0xF0, 0xF0-0xFF (~768 MB)\n");
    }

    /* 6. Load ELF data segments into virtual memory. */
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

    /* Set LR to a safe return address. On the real PS3, LR points to the
     * process exit handler when the main thread starts. We can't use 0
     * because any function that saves/restores LR will propagate the 0.
     * Set it to the sys_process_exit import stub address (0x008175FC)
     * so returning from main() cleanly calls exit. */
    ctx.lr = 0x008175FC;  /* sys_process_exit import stub */

    /* 8. Initialize null graphics backend (Win32 window for RSX clear color). */
    {
        if (rsx_null_backend_init(FLOW_WINDOW_WIDTH, FLOW_WINDOW_HEIGHT,
                                   FLOW_WINDOW_TITLE) == 0) {
            printf("[init] RSX null backend: %ux%u window\n",
                   FLOW_WINDOW_WIDTH, FLOW_WINDOW_HEIGHT);
        } else {
            printf("[init] RSX null backend: failed (continuing without window)\n");
        }
    }

    /* 9. Initialize CRT heap.
     * The PS3 CRT Dinkumware malloc reads heap metadata from BSS.
     * Rather than reverse-engineering the exact heap structure, we
     * initialize the critical BSS variables that the CRT checks:
     *
     * TOC-0x3160 → 0x101EC334: heap count (must be >= 1)
     * TOC-0x3290 → 0x1035BA88: heap init flag (must be non-zero)
     * TOC-0x3294 → 0x1035BA8C: heap descriptor table pointer
     *
     * The heap descriptor at 0x1035BA8C needs to match what the CRT
     * expects — a linked list of memory regions with free block tracking.
     * For now, we initialize the count and flag so the CRT's lock
     * function doesn't bail early, and rely on the malloc itself
     * finding available memory in the committed regions. */
    {
        /* Set heap count to 2 (CRT creates heap 0 and heap 1) */
        vm_write32(0x101EC334, 2);

        /* Set heap init flag */
        vm_write32(0x1035BA88, 1);

        /* Initialize the heap descriptor table at 0x1035BA8C.
         * On PS3, each heap entry is a struct with base, size, etc.
         * The CRT reads (descriptor + 0x0) as a pointer and dereferences
         * (descriptor + 0x10) to check available memory.
         * We set up a plausible descriptor pointing to our heap region. */
        uint32_t heap_base = 0x00A00000;
        uint32_t heap_size = 0x06000000; /* 96 MB */
        uint32_t heap_desc = 0x1035BA8C;

        /* Heap descriptor: write a self-referencing structure that
         * makes the CRT think memory is available. */
        vm_write32(heap_desc + 0x00, heap_base); /* base address */
        vm_write32(heap_desc + 0x04, heap_size); /* total size */
        vm_write32(heap_desc + 0x08, heap_base); /* current free pointer */
        vm_write32(heap_desc + 0x0C, heap_size); /* remaining size */
        vm_write32(heap_desc + 0x10, heap_base); /* free list head */
        vm_write32(heap_desc + 0x14, 0);         /* used count */
        vm_write32(heap_desc + 0x18, heap_size); /* max alloc */
        vm_write32(heap_desc + 0x1C, 0);         /* flags */

        /* Also set up a basic free block header at heap_base */
        vm_write32(heap_base + 0x00, heap_size - 0x20); /* usable size */
        vm_write32(heap_base + 0x04, 0);                /* flags: free */
        vm_write32(heap_base + 0x08, 0);                /* next = NULL */
        vm_write32(heap_base + 0x0C, 0);                /* prev = NULL */
        vm_write32(heap_base + 0x10, heap_base + 0x20); /* data ptr */

        printf("[init] CRT heap: base=0x%08X, size=%u MB\n",
               heap_base, heap_size / (1024 * 1024));
    }

    printf("[init] Entering game at 0x%X...\n\n", FLOW_ENTRY_POINT);

#ifdef _WIN32
    /* Install SEH handler to catch crashes in recompiled code */
    __try {
#endif
    /* Call the recompiled game entry point */
    recomp_game_main(&ctx);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        fprintf(stderr, "\n[CRASH] Exception 0x%08lX in recompiled code\n", code);
        fprintf(stderr, "[CRASH] Last CIA: 0x%08X, r1(SP)=0x%llX, r2(TOC)=0x%llX\n",
                ctx.cia, (unsigned long long)ctx.gpr[1], (unsigned long long)ctx.gpr[2]);
        fprintf(stderr, "[CRASH] r3=0x%llX  r4=0x%llX  r13=0x%llX  LR=0x%llX\n",
                (unsigned long long)ctx.gpr[3], (unsigned long long)ctx.gpr[4],
                (unsigned long long)ctx.gpr[13], (unsigned long long)ctx.lr);
        return 1;
    }
#endif

    /* Cleanup */
    printf("\n[exit] flOw has exited. Shutting down...\n");
    vm_shutdown();

    return 0;
}
