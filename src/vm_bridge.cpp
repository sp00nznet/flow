/* Bridge between lifter's memory API and ps3recomp runtime.
 *
 * Implements vm_read/vm_write with uint64_t addresses (lifter convention)
 * by truncating to uint32_t and performing big-endian byte swaps via vm_base.
 */

#include <cstdint>
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _MSC_VER
#include <stdlib.h>
static inline uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
static inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v);  }
static inline uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

/* vm_base is defined in main.cpp with C linkage and set by vm_init() */
extern "C" uint8_t* vm_base;

/* Module registry -- must be defined by the game project */
#include "ps3emu/module.h"
ps3_module_registry g_ps3_module_registry = {};

static inline uint8_t* translate(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    /* Warn about accesses to truly unmapped regions (above 4GB guest space).
     * Regions 0x00000000-0x100000000 are handled by VEH demand-paging. */
    if (a == 0) {
        static int s_warn = 0;
        if (s_warn < 5) {
            fputs("[VM] WARNING: null-pointer guest access", stderr); fputc(10, stderr);
            s_warn++;
        }
    }
    return vm_base + a;
}

/* Fast sequential write detector: when we see consecutive byte writes
 * (memset pattern), skip ahead and memset the whole region at once. */
static uint32_t s_seq_addr = 0;
static uint8_t  s_seq_val = 0;
static uint32_t s_seq_count = 0;

static void seq_flush(void) {
    if (s_seq_count > 0) {
        memset(vm_base + s_seq_addr, s_seq_val, s_seq_count);
        if (s_seq_count > 1000000) {
            fprintf(stderr, "[vm_write8] bulk memset: 0x%08X, %u bytes (%.1f MB)\n",
                    s_seq_addr, s_seq_count, s_seq_count / (1024.0*1024.0));
            fflush(stderr);
        }
        s_seq_count = 0;
    }
}

extern "C" {

uint8_t vm_read8(uint64_t addr) {
    seq_flush();
    return *translate(addr);
}

uint16_t vm_read16(uint64_t addr) {
    seq_flush();
    uint16_t raw;
    memcpy(&raw, translate(addr), 2);
    return bswap16(raw);
}

uint32_t vm_read32(uint64_t addr) {
    seq_flush();
    uint32_t raw;
    memcpy(&raw, translate(addr), 4);
    return bswap32(raw);
}

uint64_t vm_read64(uint64_t addr) {
    seq_flush();
    uint64_t raw;
    memcpy(&raw, translate(addr), 8);
    return bswap64(raw);
}

void vm_write8(uint64_t addr, uint8_t val) {
    uint32_t a = (uint32_t)addr;
    /* Detect sequential writes (memset pattern) */
    if (a == s_seq_addr + s_seq_count && val == s_seq_val && s_seq_count > 0) {
        s_seq_count++;
        return; /* defer the write */
    }
    /* Flush previous sequence */
    seq_flush();
    /* Start new potential sequence */
    s_seq_addr = a;
    s_seq_val = val;
    s_seq_count = 1;

    /* Actually write the byte */
    *(vm_base + a) = val;
}

void vm_write16(uint64_t addr, uint16_t val) {
    seq_flush();
    uint16_t raw = bswap16(val);
    memcpy(translate(addr), &raw, 2);
}

void vm_write32(uint64_t addr, uint32_t val) {
    seq_flush();
    uint32_t raw = bswap32(val);
    memcpy(translate(addr), &raw, 4);
}

void vm_write64(uint64_t addr, uint64_t val) {
    seq_flush();
    uint64_t raw = bswap64(val);
    memcpy(translate(addr), &raw, 8);
}

} /* extern "C" */

/* LV2 syscall dispatch — bridge from recompiled sc instruction to runtime.
 * The recompiled code uses ppu_recomp.h's ppu_context, while the runtime
 * uses runtime/ppu/ppu_context.h.  Both start with gpr[32] at offset 0,
 * so the runtime can safely cast the pointer. */
#include "recomp/ppu_recomp.h"

/* Import the syscall table type and dispatch function */
struct lv2_syscall_table;
extern "C" lv2_syscall_table g_lv2_syscalls;

/* The runtime's lv2_syscall_dispatch is static inline in the header.
 * We can't include that header because it pulls in the runtime's ppu_context.
 * Instead, replicate the dispatch logic here. */
typedef int64_t (*lv2_syscall_fn)(void* ctx);

extern "C" void lv2_syscall(ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];
    static int s_sc_count = 0;
    s_sc_count++;
    if (s_sc_count <= 20 || s_sc_count % 100 == 0) {
        fprintf(stderr, "[lv2] syscall %u (0x%X) r3=0x%llX r4=0x%llX r5=0x%llX SP=0x%08X\n",
                num, num,
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5],
                (uint32_t)ctx->gpr[1]);
        fflush(stderr);
    }
    /* The syscall table is an array of function pointers at offset 0 of g_lv2_syscalls */
    lv2_syscall_fn* handlers = (lv2_syscall_fn*)&g_lv2_syscalls;
    if (num >= 1024) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(-17); /* ENOSYS */
        return;
    }
    lv2_syscall_fn handler = handlers[num];
    if (handler) {
        ctx->gpr[3] = (uint64_t)handler((void*)ctx);
    } else {
        fprintf(stderr, "[lv2] Unimplemented syscall %u (0x%X)\n", num, num);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(-17);
    }
}
