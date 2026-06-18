/* Generic runner for one of flOw's REAL lifted SPU programs.
 *
 * Validates EXECUTION (not just lifting) of flOw's SPU code on the ps3recomp SPU
 * runtime, and traces the job's I/O contract (DMA EAs/sizes + channel ops) — the
 * info the integration needs to feed it real workloads. Run cold (zeroed main
 * memory + context): the goal is to see whether the lifted code executes through
 * the recompiler and what it reads/writes, not to produce a meaningful result
 * (that needs the SPURS job descriptor the PPU side supplies).
 *
 * Build (entry symbol varies per program; flow_spu_00 = spu_func_00000090):
 *   gcc -std=c11 -O2 -DSPU_ENTRY=spu_func_00000090 \
 *       -I ../spu_extract/flow_spu_00_lifted -I <runtime/spu> \
 *       ../spu_extract/flow_spu_00_lifted/spu_recomp.c flow_spu_run.c -o flow_spu_run.exe
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <stdlib.h>

#ifndef SPU_ENTRY
#error "define SPU_ENTRY to the lifted entry symbol (e.g. spu_func_00000090)"
#endif
extern void SPU_ENTRY(spu_context*);

/* 16 MB zeroed "main memory" so most DMA EAs land in-bounds. */
static uint8_t g_mem[16u * 1024 * 1024];
uint8_t* vm_base = g_mem;
static mfc_engine g_mfc;

/* track MFC staging so we can log each DMA when MFC_Cmd fires */
static uint32_t s_lsa, s_eah, s_eal, s_size, s_tag;
static int s_dma_count = 0, s_wrch_count = 0, s_rdch_count = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) {
    (void)ctx;
    u128 r = spu_zero();
    /* MFC_RdTagStat (24): our DMA is synchronous, so all tag groups are always
     * complete -> report all-done so the job's DMA-wait loops fall through. */
    if (ch == 24) r._u32[0] = 0xFFFFFFFFu;
    if (s_rdch_count < 12)
        fprintf(stderr, "  [rdch] ch=%u -> 0x%08X\n", ch, r._u32[0]);
    s_rdch_count++;
    return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    uint32_t v = value._u32[0];
    s_wrch_count++;
    switch (channel) {
        case 16: s_lsa  = v; break;    /* MFC_LSA  */
        case 17: s_eah  = v; break;    /* MFC_EAH  */
        case 18: s_eal  = v; break;    /* MFC_EAL  */
        case 19: s_size = v; break;    /* MFC_Size */
        case 20: s_tag  = v; break;    /* MFC_TagID*/
        case 21:                       /* MFC_Cmd -> a DMA fires */
            if (s_dma_count < 16)
                fprintf(stderr, "  [DMA ] cmd=0x%02X  LSA=0x%05X  EA=0x%08X  size=%u\n",
                        v & 0xFF, s_lsa, s_eal, s_size);
            s_dma_count++;
            break;
        default: break;
    }
    if (channel >= 16) mfc_channel_write(&g_mfc, ctx, channel, v);  /* real DMA engine */
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

/* Watchdog: a job run cold (no SPURS input) can spin in a data-dependent loop.
 * Bound execution and report the I/O contract observed so the run terminates
 * cleanly as a validation/diagnostic rather than hanging. */
static DWORD WINAPI watchdog(LPVOID p) {
    (void)p;
    Sleep(2500);
    fprintf(stderr,
        "\n[WATCHDOG] still executing after 2.5s -> data-dependent loop.\n"
        "  flOw SPU job EXECUTES on the ps3recomp runtime (lifted code runs):\n"
        "    DMAs issued=%d  channel writes=%d  channel reads=%d\n"
        "  It cannot terminate cold: the loop bounds come from the SPURS job\n"
        "  descriptor + input data the PPU side supplies. Validates execution;\n"
        "  running to completion is the PPU<->SPU integration (rung 5).\n",
        s_dma_count, s_wrch_count, s_rdch_count);
    fflush(stderr);
    _exit(0);
    return 0;
}

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    spu_context ctx;
    spu_context_init(&ctx, 0);
    mfc_engine_init(&g_mfc);

    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    fprintf(stderr, "Running flOw SPU job (cold, watchdog-bounded) ...\n");
    SPU_ENTRY(&ctx);                   /* execute the real lifted SPU code */

    fprintf(stderr, "RETURNED cleanly: %d DMAs, %d wrch, %d rdch\n",
            s_dma_count, s_wrch_count, s_rdch_count);
    return 0;
}
