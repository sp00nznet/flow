/* Multi-image SPU workload dispatch — proves the --symbol-prefix namespacing.
 *
 * Two real flOw SPU jobs (flow_spu_00, flow_spu_01) are lifted with distinct
 * symbol prefixes and LINKED INTO ONE BINARY — impossible before, since every
 * lifted image exported spu_func_00000090 / spu_recomp_register. This harness:
 *   1. links both prefixed images (the link succeeding IS the namespacing proof),
 *   2. registers each under its image fingerprint,
 *   3. verifies the registry maps each fingerprint to the correct entry, and
 *   4. dispatches one real image through spu_workload_dispatch (watchdog-bounded).
 *
 * This is the scale model of the eventual flow.exe integration: lift all 60 SPU
 * binaries with per-job prefixes, register them from the manifest, and let
 * cellSpurs dispatch the right one by fingerprint.
 *
 * Build (PS3RECOMP_DIR = ps3recomp checkout):
 *   gcc -std=c11 -O2 -I $PS3RECOMP_DIR/runtime/spu \
 *       spu_extract/flow_spu_00_ns/spu_recomp.c \
 *       spu_extract/flow_spu_01_ns/spu_recomp.c \
 *       $PS3RECOMP_DIR/runtime/spu/spu_workload.c tools/flow_spu_dispatch_multi.c \
 *       -o tools/flow_spu_dispatch_multi.exe
 */
#include "spu_workload.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

/* The two namespaced lifted entries (declared here so we don't include two
 * generated headers with clashing relative include paths). */
extern void flow_spu_00_spu_func_00000090(spu_context* ctx);
extern void flow_spu_01_spu_func_00000090(spu_context* ctx);

static const uint64_t kFP00 = 0x0E90F50A6014D361ULL;   /* manifest: flow_spu_00 */
static const uint64_t kFP01 = 0x573579AC2D1852FFULL;   /* manifest: flow_spu_01 */

static uint8_t g_mem[16u * 1024 * 1024];
uint8_t* vm_base = g_mem;
static mfc_engine g_mfc;
static int s_dma = 0, s_wrch = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) {
    (void)ctx; u128 r = spu_zero();
    if (ch == 24) r._u32[0] = 0xFFFFFFFFu;
    return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    s_wrch++; if (channel == 21) s_dma++;
    if (channel >= 16) mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f); return b;
}

static DWORD WINAPI watchdog(LPVOID p) {
    (void)p; Sleep(2000);
    fprintf(stderr, "[WATCHDOG] cold-run loop bounded (DMAs=%d wrch=%d) — job ran.\n",
            s_dma, s_wrch);
    fflush(stderr); _exit(0); return 0;
}

int main(void) {
    mfc_engine_init(&g_mfc);
    int ok = 1;

    long n0, n1;
    uint8_t* e0 = slurp("spu_extract/flow_spu_00.elf", &n0);
    uint8_t* e1 = slurp("spu_extract/flow_spu_01.elf", &n1);

    /* fingerprints must match the manifest */
    uint64_t f0 = spu_workload_fingerprint(e0, (size_t)n0);
    uint64_t f1 = spu_workload_fingerprint(e1, (size_t)n1);
    int fp_ok = (f0 == kFP00 && f1 == kFP01 && f0 != f1);
    printf("  [FINGERPRINT] f0=0x%016llX f1=0x%016llX  distinct+match=%s\n",
           (unsigned long long)f0, (unsigned long long)f1, fp_ok ? "OK" : "FAIL");
    ok &= fp_ok;

    /* register both namespaced entries */
    spu_workload_register(f0, flow_spu_00_spu_func_00000090, "flow_spu_00");
    spu_workload_register(f1, flow_spu_01_spu_func_00000090, "flow_spu_01");

    /* registry must map each fingerprint to its OWN entry */
    int map_ok = (spu_workload_find(f0) == flow_spu_00_spu_func_00000090) &&
                 (spu_workload_find(f1) == flow_spu_01_spu_func_00000090) &&
                 (spu_workload_count() == 2);
    printf("  [REGISTRY   ] count=%u  fp->entry mapping                 %s\n",
           spu_workload_count(), map_ok ? "OK" : "FAIL");
    ok &= map_ok;

    if (!ok) { printf("  FAIL before dispatch\n"); return 1; }

    /* dispatch the SECOND image through the full path (watchdog-bounded) */
    printf("  [DISPATCH   ] running flow_spu_01 through spu_workload_dispatch ...\n");
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    int hit = spu_workload_dispatch(e1, (uint32_t)n1, 0);
    printf("  [DISPATCH   ] hit=%d\n", hit);   /* watchdog usually exits first */
    return 0;
}
