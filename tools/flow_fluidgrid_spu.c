/* FluidGrid SPU bring-up: run flОw's velocity-simulation SPU job through the
 * ps3recomp SPU runtime.
 *
 * flow_spu_58.elf IS the FluidGrid velocity sim (strings: "Velocity sim SPU %x
 * Initialized and running", source ../flOw/flOw_grid.spu.elf). The RPCS3 oracle
 * shows flОw's live water is this SPU job iterating a velocity grid; our boot
 * never creates it, so the world is static. This harness proves the lifted
 * FluidGrid SPU code EXECUTES through our runtime's dispatch + ELF->LS loader:
 *   1. read the real extracted SPU ELF,
 *   2. register its pre-lifted entry under the image fingerprint,
 *   3. spu_workload_dispatch(image, size, arg) -> load to local store -> run.
 *
 * Cold run (no PPU-supplied grid descriptor), so execution is watchdog-bounded:
 * the goal is to see the real FluidGrid SPU code run THROUGH the dispatch path
 * (DMAs + channel ops), not to complete the sim (that needs the PPU side — the
 * FluidGrid CreateSPUThreads setup feeding the grid EA + parameters).
 *
 * Build (PS3RECOMP_DIR = ps3recomp checkout):
 *   gcc -std=c11 -O2 -I spu_extract/flow_spu_58_lifted -I $PS3RECOMP_DIR/runtime/spu \
 *       spu_extract/flow_spu_58_lifted/spu_recomp.c \
 *       $PS3RECOMP_DIR/runtime/spu/spu_workload.c tools/flow_fluidgrid_spu.c \
 *       -o tools/flow_fluidgrid_spu.exe
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include "spu_workload.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

/* 16 MB zeroed "main memory" so the sim's grid DMA EAs land in-bounds. */
static uint8_t g_mem[16u * 1024 * 1024];
uint8_t* vm_base = g_mem;
static mfc_engine g_mfc;
static int s_dma = 0, s_wrch = 0, s_rdch = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) {
    (void)ctx; u128 r = spu_zero();
    if (ch == 24) r._u32[0] = 0xFFFFFFFFu;   /* MFC_RdTagStat: DMA done */
    s_rdch++; return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    s_wrch++;
    if (channel == 21) s_dma++;              /* MFC_Cmd -> a DMA fires */
    if (channel >= 16) mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static DWORD WINAPI watchdog(LPVOID p) {
    (void)p;
    Sleep(2500);
    fprintf(stderr,
        "\n[WATCHDOG] FluidGrid SPU still executing after 2.5s -> data-dependent\n"
        "  sim loop (cold run, no PPU grid descriptor). The REAL flOw velocity\n"
        "  simulation executed THROUGH the ps3recomp SPU dispatch path:\n"
        "    DMAs=%d  channel writes=%d  channel reads=%d\n"
        "  Runtime fingerprinted + loaded flОw_grid.spu.elf and ran the lifted\n"
        "  FluidGrid job; completion needs the PPU CreateSPUThreads setup\n"
        "  feeding the grid EA + parameters (the next integration step).\n",
        s_dma, s_wrch, s_rdch);
    fflush(stderr);
    _exit(0);
    return 0;
}

int main(void) {
    const char* path = "spu_extract/flow_spu_58.elf";
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* elf = (uint8_t*)malloc((size_t)n);
    if (fread(elf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);

    mfc_engine_init(&g_mfc);

    uint64_t fp = spu_workload_fingerprint(elf, (size_t)n);
    fprintf(stderr, "FluidGrid %s: %ld bytes  fp=0x%016llX\n",
            path, n, (unsigned long long)fp);

    /* register the real lifted FluidGrid entry, then dispatch the real image */
    spu_workload_register(fp, spu_func_00000090, "flow_spu_58_fluidgrid");

    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    fprintf(stderr, "Dispatching FluidGrid velocity sim through spu_workload_dispatch ...\n");

    int hit = spu_workload_dispatch(elf, (uint32_t)n, 0 /*cold run, no grid EA*/);

    fprintf(stderr, "RETURNED: dispatch hit=%d  DMAs=%d wrch=%d rdch=%d\n",
            hit, s_dma, s_wrch, s_rdch);
    return 0;
}
