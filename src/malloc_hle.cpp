/*
 * flОw — guest allocator HLE (clean boot harness).
 *
 * flОw's Dinkumware/SNC CRT heap-init reads a firmware-set memory-size global
 * that the real liblv2 process init would populate; we run the game CRT directly
 * (proper static recomp, boot from _start) so that global is 0 and the heap stays
 * uninitialised -> the first malloc returns NULL -> "### Sorry, there is not
 * enough memory to initialize mutex." -> abort. We replace the guest allocator
 * with a host-side bump allocator over the (demand-paged) guest heap region. This
 * is a standard recomp HLE of malloc (NOT a boot bypass): the real CRT still runs
 * top-to-bottom; only the broken allocator is substituted.
 *
 * Wired by patching func_006B738C (malloc) in src/recomp_v2 to call
 * hle_guest_malloc. Re-apply that patch after any re-lift.
 */
#include "ppu_recomp.h"   /* ppu_context (from src/recomp_v2) */
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint8_t* vm_base;
extern "C" void vm_write32(uint64_t addr, uint32_t val);

/* Pre-run hook (boot_main.cpp calls this weak symbol after the function table is
 * registered + vm_base is live, before _start). Pre-initialise the SNC/Dinkumware
 * CRT heap descriptor so the CRT's heap-lock + free-list walk (the ~243x
 * get_thread_id/lwmutex loop) sees an initialised heap and doesn't abort. These
 * are flОw guest BSS globals (stable game addresses); actual allocations are
 * served by hle_guest_malloc. The descriptor region (0x900000) is kept clear of
 * the malloc bump (0xA00000+) so neither clobbers the other. */
extern "C" void flow_heap_init(void)   /* called from prx_init.cpp's ps3_load_prx_modules */
{
    /* Pre-fill the REAL Dinkumware heap descriptor so the lifted malloc/free
     * (func_006B738C etc.) run for real. Layout reverse-engineered from
     * func_006B738C: descriptor HD = *(0x893728) = 0x101EC148; HD+0x8 = available
     * bytes (bump counter), HD+0x14 = current free block (bump pointer). The CRT's
     * own heap-create leaves avail=0 (firmware mem-size global is 0) -> malloc
     * returns NULL -> abort. Filling avail + region here makes the real allocator
     * work, so it writes proper Dinkumware block headers and the heap validation
     * (the abort) passes. Heap region 0xC00000..0xD000000 is clear of GCM io
     * (0xB00000), TLS (0xE000000) and the stack (0xFF00000); demand-paged. */
    const uint32_t HD = 0x101EC148;
    const uint32_t heap_base = 0x00C00000;
    const uint32_t heap_size = 0x0C400000;          /* ~196 MB up to 0x0D000000 */
    vm_write32(HD + 0x08, heap_size);               /* available bytes */
    vm_write32(HD + 0x14, heap_base);               /* current free block / bump ptr */
    vm_write32(0x101EC334, 1);                       /* heap count (>=1) */
    vm_write32(heap_base + 0x04, heap_size | 1u);   /* initial free-block header */
    fprintf(stderr, "[flow-init] Dinkumware heap desc @0x%08X: base=0x%08X avail=0x%X (real malloc)\n",
            HD, heap_base, heap_size);
}

/* Bump allocator over flОw's guest heap window (above the loaded image/data,
 * below the RSX region). Demand-paged by the boot harness crash handler. */
static uint32_t g_heap_ptr = 0x00A00000;
/* Ceiling MUST stay below the CRT TLS block (sys_initialize_tls @0x0E000000) and the
 * main stack (0x0FF00000); overlapping them corrupted TLS -> a timing-dependent
 * heisenbug (boot aborted or reached graphics depending on alloc count). */
static const uint32_t g_heap_end = 0x0D000000;
static uint32_t g_alloc_count = 0;

/* C++ linkage to match the recomp chunk's `extern void hle_guest_malloc(...)`. */
void hle_guest_malloc(ppu_context* ctx)
{
    uint32_t size = (uint32_t)ctx->gpr[3];
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;
    if (g_heap_ptr + size > g_heap_end) {
        fprintf(stderr, "[malloc-hle] OOM: %u bytes (used %u MB)\n",
                size, (g_heap_ptr - 0x00A00000) / (1024 * 1024));
        ctx->gpr[3] = 0;
        return;
    }
    uint32_t ptr = g_heap_ptr;
    g_heap_ptr += size;
    /* Zero the block: the Dinkumware CRT links malloc'd nodes into lists and walks
     * them to a NULL terminator; uninitialised "next" pointers would derail the
     * walk into an abort. (Bump allocator never reuses, so this is safe + cheap.) */
    if (vm_base) memset(vm_base + ptr, 0, size);
    if (++g_alloc_count <= 8 && getenv("FLOW_MEMTRACE"))
        fprintf(stderr, "[malloc-hle] #%u size=%u -> 0x%08X\n", g_alloc_count, size, ptr);
    ctx->gpr[3] = ptr;
}
