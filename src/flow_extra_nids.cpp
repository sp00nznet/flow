/* flow_extra_nids.cpp — wire NIDs flОw imports that have real ps3recomp
 * handlers but were missing from the generated NID table (src/gen/ppu_hle_nids.cpp,
 * stale since Jun 28). Observed at runtime as `[hle] unresolved NID 0x...` in the
 * MODE_AUTO_LOAD loop -- notably cellGcmSetFlip (frame present) and cellPadGetInfo.
 *
 * The HLE dispatch (ppu_hle.cpp) calls a registered handler generically: guest
 * args arrive in gpr3.. and the return goes to r3, so the real lib signatures
 * work as-is. Registered from a host static initializer (runs before guest boot).
 */
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "ppu_recomp.h"   /* ppu_context + func decls (recomp_v2) */

/* Diagnostic: dump the .init_array walker (func_00816450) state per iteration to
 * see whether the OPD table / init-array is corrupted at runtime in the v4 relift. */
extern "C" void flow_ctorwalk_trace(unsigned int r31, unsigned int r9, unsigned int target) {
    static int n = 0;
    if (!getenv("FLOW_CTORWALK")) return;
    n++;
    /* only print the tail (near where it goes bad) + any garbage target */
    /* print the last on-array iteration (the ctor that corrupts r31) + first off-array */
    static unsigned int p_r31=0, p_tgt=0; static int p_on=1;
    int on_array = (r31 >= 0x00800000u && r31 < 0x00900000u);
    if (n <= 160 && p_on && !on_array) {
        fprintf(stderr, "[ctorwalk] CORRUPTOR at prev iter: r31=0x%08X called ctor target=0x%08X\n", p_r31, p_tgt);
        fprintf(stderr, "[ctorwalk] #%d now OFF-ARRAY r31=0x%08X\n", n, r31);
    }
    p_r31=r31; p_tgt=target; p_on=on_array;
}
extern "C" {
    void ps3_hle_register(unsigned int nid, const char* name, void* handler);
    void ps3_hle_register_ctx(unsigned int nid, const char* name, void (*fn)(ppu_context*));
    void ppu_guard_page(unsigned int guest_ea);  /* runtime page-guard watchpoint */
    void run_elf_constructors(ppu_context* ctx);       /* flow_run_ctors.cpp */
    void run_remaining_constructors(ppu_context* ctx);
    /* Real handlers live in the linked ps3recomp runtime libs (extern "C",
     * resolved by name at link; the no-arg decl here is only to take the address). */
    void cellGcmSetFlip(void);
    void cellPadGetInfo(void);
    void cellSaveDataAutoLoad(void);
    void cellAudioOutGetSoundAvailability(void);
    void cellSysmoduleInitialize(void);
    void cellAudioOutConfigure(void);
}

/* FLOW_GUARDVT=<hex>: arm the runtime page-guard on this .bss vtable slot at the
 * first cellSysmoduleInitialize (early, before m_InitEntityHierarchy) to catch the
 * raw memmove that clobbers obj@0x10187F78's vtable -> the null-vtable spin. */
/* FLOW_RUNCTORS: flОw's lifted CRT .init_array skips some static ctors (e.g. the
 * singleton at 0x10187F78 stays null-vtable -> title/localization spin). Run them
 * explicitly ONCE at the first cellSysmoduleInitialize (after CRT heap init, before
 * m_InitEntityHierarchy). Uses a SCRATCH copy of the live ctx (same guest TOC r2 +
 * stack) so the ctors don't clobber the calling thread's registers/return. This is
 * the same workaround the bypass build (run_ctors.cpp) uses. Default ON. */
static void flow_sysmod_ctor_wrap(ppu_context* ctx) {
    static int done = 0;
    if (!done) { done = 1;
        const char* g = getenv("FLOW_GUARDVT");
        if (g) ppu_guard_page((unsigned int)strtoul(g, 0, 16));
        if (!getenv("FLOW_NOCTORS")) {
            ppu_context cctx; memcpy(&cctx, ctx, sizeof(cctx));
            run_elf_constructors(&cctx);
            /* BOTH batches required — primary-only leaves 0x10187F78 null-vtable. */
            memcpy(&cctx, ctx, sizeof(cctx));
            run_remaining_constructors(&cctx);
        }
    }
    cellSysmoduleInitialize();
    ctx->gpr[3] = 0;   /* CELL_OK */
}

/* Defined in flow_spu_workloads.c — takes the GPR file so that TU doesn't need
 * the ppu_context layout. Runs the real HLE AddWorkload, then dispatches the
 * lifted SPU image for the workload the game just added. */
extern "C" void flow_spurs_add_workload_ctx(unsigned long long* gpr);
extern "C" void flow_spurs_initialize_ctx(unsigned long long* gpr);

static void flow_spurs_add_workload_wrap(ppu_context* ctx)
{
    flow_spurs_add_workload_ctx(reinterpret_cast<unsigned long long*>(ctx->gpr));
}

static void flow_spurs_initialize_wrap(ppu_context* ctx)
{
    flow_spurs_initialize_ctx(reinterpret_cast<unsigned long long*>(ctx->gpr));
}

namespace {
struct FlowExtraNids {
    FlowExtraNids() {
        ps3_hle_register(0xDC09357Eu, "cellGcmSetFlip",                 (void*)cellGcmSetFlip);
        ps3_hle_register(0x3AAAD464u, "cellPadGetInfo",                 (void*)cellPadGetInfo);
        /* cellSaveDataAutoLoad: now safe -- the runtime handler was fixed to
         * translate its guest dirName EA before use (was faulting on 0xD00EAF58). */
        ps3_hle_register(0xC22C79B5u, "cellSaveDataAutoLoad",          (void*)cellSaveDataAutoLoad);
        ps3_hle_register(0xC01B4E7Cu, "cellAudioOutGetSoundAvailability", (void*)cellAudioOutGetSoundAvailability);
        ps3_hle_register_ctx(0x63FF6FF9u, "cellSysmoduleInitialize",     flow_sysmod_ctor_wrap);
        ps3_hle_register(0x4692AB35u, "cellAudioOutConfigure",          (void*)cellAudioOutConfigure);
        /* cellSpursAddWorkload: the HLE only RECORDS workloads, so flОw's SPU
         * fluid/velocity sim never runs and the render loop waits forever on a
         * scene job that never lands (draws=0). Intercept it and actually
         * dispatch the lifted SPU image. See src/flow_spu_workloads.c. */
        ps3_hle_register_ctx(0x69726AA2u, "cellSpursAddWorkload",        flow_spurs_add_workload_wrap);
        /* cellSpursInitialize: the HLE writes a ~290-byte host stub, but the SPU
         * policy module DMAs the first 0x80 bytes of the instance and reads them
         * as the real Sony scheduling header. Overwrite with the real layout. */
        ps3_hle_register_ctx(0xACFC8DBCu, "cellSpursInitialize",         flow_spurs_initialize_wrap);
    }
} g_flow_extra_nids;
}
