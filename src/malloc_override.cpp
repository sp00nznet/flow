/*
 * flOw — Guest malloc/free HLE override
 *
 * Replaces the CRT's Dinkumware malloc with a simple bump allocator.
 * Instead of redefining the symbols (which causes link errors), we
 * patch the function pointers at runtime via flow_patch_malloc().
 */

#include "recomp/ppu_recomp.h"
#include <cstdio>
#include <cstring>

extern "C" uint8_t* vm_base;

/* Bump allocator state */
static uint32_t g_heap_ptr = 0x00A00000;
static const uint32_t g_heap_end = 0x10000000;
static uint32_t g_alloc_count = 0;

/* HLE malloc — called from patched func_006B738C */
extern "C" void hle_guest_malloc(ppu_context* ctx);
void hle_guest_malloc(ppu_context* ctx)
{
    uint32_t size = (uint32_t)ctx->gpr[3];
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;

    if (g_heap_ptr + size > g_heap_end) {
        fprintf(stderr, "[malloc] OOM: %u bytes, used %u MB\n",
                size, (g_heap_ptr - 0x00A00000) / (1024*1024));
        ctx->gpr[3] = 0;
        return;
    }

    uint32_t ptr = g_heap_ptr;
    g_heap_ptr += size;
    g_alloc_count++;
    memset(vm_base + ptr, 0, size);
    ctx->gpr[3] = ptr;

    if (g_alloc_count <= 30 || ptr == 0x00A000C0 ||
        (ptr <= 0x01800000 && ptr + size > 0x01800000))
        fprintf(stderr, "[malloc] %u -> 0x%08X (%u allocs)\n", size, ptr, g_alloc_count);
}

/* HLE free — no-op */
static void hle_free(ppu_context* ctx) { (void)ctx; }

/* HLE calloc */
static void hle_calloc(ppu_context* ctx)
{
    uint32_t n = (uint32_t)ctx->gpr[3];
    uint32_t s = (uint32_t)ctx->gpr[4];
    ctx->gpr[3] = (uint64_t)(n * s);
    hle_guest_malloc(ctx);
}

/* HLE realloc */
static void hle_realloc(ppu_context* ctx)
{
    uint32_t old_ptr = (uint32_t)ctx->gpr[3];
    uint32_t new_size = (uint32_t)ctx->gpr[4];
    ctx->gpr[3] = (uint64_t)new_size;
    hle_guest_malloc(ctx);
    uint32_t new_ptr = (uint32_t)ctx->gpr[3];
    if (new_ptr && old_ptr && vm_base) {
        uint32_t cp = new_size < 0x10000 ? new_size : 0x10000;
        memcpy(vm_base + new_ptr, vm_base + old_ptr, cp);
    }
}

/* Reset heap state — called on CRT abort redirect to discard CRT allocations */
extern "C" void hle_guest_malloc_reset(void)
{
    static int s_reset_count = 0;
    s_reset_count++;
    fprintf(stderr, "[MALLOC-RESET] #%d heap ptr=0x%08X allocs=%u\n",
            s_reset_count, g_heap_ptr, g_alloc_count);
    fflush(stderr);
    /* Zero the heap region to clear stale data from previous passes */
    if (vm_base)
        memset(vm_base + 0x00A00000, 0, g_heap_end - 0x00A00000);
    g_heap_ptr = 0x00A00000;
    g_alloc_count = 0;
}

/* Note: malloc is patched by modifying func_006B738C in ppu_recomp.cpp
 * to call hle_guest_malloc() directly. No runtime table patching needed. */
