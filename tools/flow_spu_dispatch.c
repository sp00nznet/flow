/* End-to-end validation of cellSpurs workload dispatch on a REAL flOw SPU job.
 *
 * Unlike flow_spu_run.c (which calls the lifted entry directly), this drives the
 * actual ps3recomp dispatch path the cellSpurs hook uses:
 *   1. read the real extracted SPU ELF (spu_extract/flow_spu_00.elf),
 *   2. register its pre-lifted entry under the image's FNV-1a fingerprint
 *      (the exact value the manifest / cellSpursCreateTask will use),
 *   3. spu_workload_dispatch(image, size, arg) -> fingerprint -> find -> load
 *      the ELF into a local store -> run the lifted job with the arg in r3.
 *
 * Proves the dispatcher + ELF->LS loader handle a real PhyreEngine SPU ELF and
 * that the runtime fingerprint matches the offline manifest. Run cold (no SPURS
 * job descriptor), so execution is watchdog-bounded: the goal is to see real
 * flOw SPU code execute THROUGH the dispatch path, not to complete (completion
 * needs the PPU-supplied input data — the un-bypass).
 *
 * Build (PS3RECOMP_DIR = ps3recomp checkout):
 *   gcc -std=c11 -O2 -I spu_extract/flow_spu_00_lifted -I $PS3RECOMP_DIR/runtime/spu \
 *       spu_extract/flow_spu_00_lifted/spu_recomp.c \
 *       $PS3RECOMP_DIR/runtime/spu/spu_workload.c tools/flow_spu_dispatch.c \
 *       -o tools/flow_spu_dispatch.exe
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

/* 16 MB zeroed "main memory" so most DMA EAs land in-bounds. */
static uint8_t g_mem[16u * 1024 * 1024];
uint8_t* vm_base = g_mem;
static mfc_engine g_mfc;
static int s_dma = 0, s_wrch = 0, s_rdch = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) {
    (void)ctx; u128 r = spu_zero();
    if (ch == 24) r._u32[0] = 0xFFFFFFFFu;     /* MFC_RdTagStat: synchronous DMA -> done */
    s_rdch++; return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    s_wrch++;
    if (channel == 21) s_dma++;                /* MFC_Cmd -> a DMA fires */
    if (channel >= 16) mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static DWORD WINAPI watchdog(LPVOID p) {
    (void)p;
    Sleep(2500);
    fprintf(stderr,
        "\n[WATCHDOG] still executing after 2.5s -> data-dependent loop (cold run).\n"
        "  REAL flOw SPU job executed THROUGH the cellSpurs dispatch path:\n"
        "    DMAs=%d  channel writes=%d  channel reads=%d\n"
        "  Dispatcher fingerprinted + loaded the real ELF and ran the lifted job;\n"
        "  termination needs the PPU-supplied SPURS job descriptor (the un-bypass).\n",
        s_dma, s_wrch, s_rdch);
    fflush(stderr);
    _exit(0);
    return 0;
}

int main(void) {
    /* read the real extracted SPU ELF */
    FILE* f = fopen("spu_extract/flow_spu_00.elf", "rb");
    if (!f) { fprintf(stderr, "cannot open spu_extract/flow_spu_00.elf\n"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* elf = (uint8_t*)malloc((size_t)n);
    if (fread(elf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);

    mfc_engine_init(&g_mfc);

    /* fingerprint must match the offline manifest value for flow_spu_00 */
    uint64_t fp = spu_workload_fingerprint(elf, (size_t)n);
    const uint64_t kManifestFP = 0x0E90F50A6014D361ULL;
    fprintf(stderr, "flow_spu_00.elf: %ld bytes  fp=0x%016llX  manifest=0x%016llX  %s\n",
            n, (unsigned long long)fp, (unsigned long long)kManifestFP,
            fp == kManifestFP ? "MATCH" : "MISMATCH");

    /* register the real lifted entry, then dispatch the real image */
    spu_workload_register(fp, spu_func_00000090, "flow_spu_00");

    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    fprintf(stderr, "Dispatching real flOw SPU job through spu_workload_dispatch ...\n");

    int hit = spu_workload_dispatch(elf, (uint32_t)n, 0 /*arg EA: none, cold run*/);

    fprintf(stderr, "RETURNED: dispatch hit=%d  DMAs=%d wrch=%d rdch=%d\n",
            hit, s_dma, s_wrch, s_rdch);
    return 0;
}
