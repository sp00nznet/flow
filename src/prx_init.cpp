/* YDKJ real-PRX loader: brings the lifted libsre (Sony cellSpurs/cellSync) into
 * guest RAM at boot so the title's SPURS imports dispatch into REAL recompiled
 * Sony code instead of HLE stubs. Defines the weak ps3_load_prx_modules() hook
 * that the generic boot harness (runtime/ppu/tests/boot_main.cpp) calls after the
 * lifted function table is registered and vm_base is live.
 *
 * Reuses the lifted libsre from the flОw sister-project (prx/libsre_ns/*,
 * prx/libsre.linked.bin) -- firmware-version-close; covers 66/82 of YDKJ's SPURS
 * NIDs (the other 16 fall back to HLE stubs). The PRX toolchain + loader are in
 * the shared ps3recomp runtime (runtime/prx/prx_loader.*). */
#include "prx_loader.h"
#include "ppu_recomp.h"   /* ppu_context (for the import-dispatch thunk) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
/* libsre's namespaced lifted tables (prx/libsre_ns/*). The lifter's func_entry
 * is binary-compatible with prx_func_entry. */
extern const prx_func_entry libsre_function_table[];
extern const uint64_t        libsre_function_table_count;
extern const prx_export      libsre_exports[];
extern const uint32_t        libsre_export_count;

/* YDKJ's indirect-dispatch registrar (runtime/ppu/ppu_loader.cpp). extern "C"
 * matches by name; the (uint64_t, fn) ABI is identical to ppu_register_function. */
void ppu_register_function(uint64_t addr, void (*fn)(void*));

/* HLE registration + sysPrxForUser CRT shims libsre imports but the title does
 * not (so the generated NID table omits them). Registered here so cellSpurs'
 * init path (which strcpy/strncat's the SPU thread-group name and validates the
 * returned dst pointer) resolves instead of getting a 0 from the unresolved
 * fallback. */
void ps3_hle_register(unsigned int nid, const char* name, void* handler);
void ps3_hle_register_ctx(unsigned int nid, const char* name, void (*fn)(ppu_context*));
char* _sys_strcpy(char* dst, const char* src);
char* _sys_strncat(char* dst, const char* src, unsigned int size);
int   _sys_strncmp(const char* s1, const char* s2, unsigned int size);

/* Guest memory accessors (runtime/ppu/ppu_loader.cpp). */
void     vm_write32(uint64_t addr, uint32_t val);
uint64_t vm_read64(uint64_t addr);

/* sysPrxForUser thread primitives libsre imports (ctx-ABI syscall impls). */
int64_t sys_ppu_thread_create(ppu_context* ctx);
int64_t sys_ppu_thread_exit(ppu_context* ctx);
}

/* ctx-handler wrappers: the syscall impls return s64; a ctx HLE handler must
 * place the result in r3 itself. */
static void wrap_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)sys_ppu_thread_create(ctx); }
static void wrap_ppu_thread_exit (ppu_context* ctx) { (void)sys_ppu_thread_exit(ctx); }

/* ---------------------------------------------------------------------------
 * libsre lwmutex/lwcond translating wrappers.
 *
 * The libs/system/sysPrxForUser lwmutex/lwcond impls take host pointers
 * (unlike the _sys_mem and _sys_str helpers, which translate internally via
 * yz_g2h).
 * The generic HLE adapter forwards the raw guest address, so libsre's first
 * sys_lwcond_wait deref'd a guest EA as a host pointer -> AV. Register ctx
 * wrappers (checked before the generic table) that translate the pointer args
 * guest->host, then call the real impl. */
extern "C" uint8_t* vm_base;
extern "C" {
    int32_t sys_lwmutex_create (void* m, void* attr);
    int32_t sys_lwmutex_lock   (void* m, uint64_t timeout);
    int32_t sys_lwmutex_unlock (void* m);
    int32_t sys_lwmutex_destroy(void* m);
    int32_t sys_lwcond_create  (void* c, void* m, void* attr);
    int32_t sys_lwcond_wait    (void* c, uint64_t timeout);
    int32_t sys_lwcond_signal  (void* c);
    int32_t sys_lwcond_destroy (void* c);
    int32_t sys_prx_get_module_id_by_name(const char* name, uint64_t flags, uint32_t* id);
    int32_t sys_process_get_paramsfo(void* buf);
}
static inline void* g2h(ppu_context* ctx, int r)
{
    uint32_t ea = (uint32_t)ctx->gpr[r];
    return ea ? (void*)(vm_base + ea) : (void*)0;
}
#define SET_R3_S32(ctx, v) ((ctx)->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))
static void w_lwmutex_create (ppu_context* c){ SET_R3_S32(c, sys_lwmutex_create (g2h(c,3), g2h(c,4))); }
static void w_lwmutex_lock   (ppu_context* c){ SET_R3_S32(c, sys_lwmutex_lock   (g2h(c,3), c->gpr[4])); }
static void w_lwmutex_unlock (ppu_context* c){ SET_R3_S32(c, sys_lwmutex_unlock (g2h(c,3))); }
static void w_lwmutex_destroy(ppu_context* c){ SET_R3_S32(c, sys_lwmutex_destroy(g2h(c,3))); }
static void w_lwcond_create  (ppu_context* c){ SET_R3_S32(c, sys_lwcond_create  (g2h(c,3), g2h(c,4), g2h(c,5))); }
static void w_lwcond_wait    (ppu_context* c){ SET_R3_S32(c, sys_lwcond_wait    (g2h(c,3), c->gpr[4])); }
static void w_lwcond_signal  (ppu_context* c){ SET_R3_S32(c, sys_lwcond_signal  (g2h(c,3))); }
static void w_lwcond_destroy (ppu_context* c){ SET_R3_S32(c, sys_lwcond_destroy (g2h(c,3))); }
/* sys_prx_get_module_id_by_name: we keep no name->module-id registry, so the
 * correct answer for any query is CELL_PRX_ERROR_UNKNOWN_MODULE (0x8001112E).
 * This matters for cellSpurs: _cellSpursIsLaunchedFromTuner probes for the
 * "cellLibprof" module this way and, in the checked libsre build, asserts
 * (usertrace.c:123) on ANY result other than UNKNOWN_MODULE -- returning
 * CELL_OK/CELL_EFAULT (e.g. because the id out-ptr is NULL) trips that assert
 * and aborts PSSG init. UNKNOWN_MODULE also yields the correct "not launched
 * from a tuner" answer (0). */
#define CELL_PRX_ERROR_UNKNOWN_MODULE 0x8001112E
static void w_prx_get_module_id(ppu_context* c){ (void)sys_prx_get_module_id_by_name; SET_R3_S32(c, CELL_PRX_ERROR_UNKNOWN_MODULE); }
static void w_process_paramsfo (ppu_context* c){ SET_R3_S32(c, sys_process_get_paramsfo(g2h(c,3))); }

/* DIAGNOSTIC (FLOW_PSSGTRACE=1): hook _sys_printf to dump the guest call stack
 * when PhyreEngine prints its PSSG/Init failure, so we can locate the failing
 * function. Forwards to the real _sys_printf (generic 8-arg ABI). */
extern "C" uint32_t vm_read32(uint64_t addr);
typedef uint64_t (*gen8_fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
extern "C" int _sys_printf(const char*, ...);
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**, unsigned long*);
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char*);
static void w_sys_printf(ppu_context* c)
{
    if (getenv("FLOW_PSSGTRACE")) {
        uint32_t fmt = (uint32_t)c->gpr[3];
        const char* hs = (fmt && vm_base) ? (const char*)(vm_base + fmt) : "";
        if (strstr(hs, "PSSG") || strstr(hs, "InitApplication") || strstr(hs, "failed") || strstr(hs, "Error")) {
            fprintf(stderr, "[pssg-bt] _sys_printf fmt=\"%.60s\" caller lr=0x%08X\n", hs, (uint32_t)c->lr);
            uint32_t sp = (uint32_t)c->gpr[1];
            for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                uint32_t nsp = vm_read32(sp);
                if (nsp <= sp) break;
                uint32_t lr = vm_read32(nsp + 0x10);
                fprintf(stderr, "[pssg-bt]   #%d lr=0x%08X\n", i, lr);
                sp = nsp;
            }
        }
    }
    c->gpr[3] = ((gen8_fn)_sys_printf)(c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6],
                                       c->gpr[7], c->gpr[8], c->gpr[9], c->gpr[10]);
}

/* DIAGNOSTIC (FLOW_GTIDTRACE=1): sys_ppu_thread_get_id is called by PhyreEngine
 * right up to the PSSG init failure; ctx->lr is the caller (a PhyreEngine code
 * addr). Logging it lets us locate the failing function (the last LR before the
 * "Error initializing PSSG" line). Replicates the stub (*id = 1). */
static void w_get_thread_id_trace(ppu_context* c)
{
    if (getenv("FLOW_GTIDTRACE")) {
        static int n = 0;
        if (n++ < 4000) {
            /* Lifted code drives control flow via the HOST C call stack (not
             * guest-stack frames), so capture the host backtrace and emit RVAs
             * (symbolize the last-before-error frames via flow.map). */
            void* bt[16]; unsigned short fr = RtlCaptureStackBackTrace(1, 16, bt, 0);
            static char* mbase = 0; if (!mbase) mbase = (char*)GetModuleHandleA(0);
            char line[400]; int p = 0;
            p += snprintf(line+p, sizeof(line)-p, "[gtid] rva:");
            for (int i = 0; i < fr; i++)
                p += snprintf(line+p, sizeof(line)-p, " %llX", (unsigned long long)((char*)bt[i] - mbase));
            fprintf(stderr, "%s\n", line);
        }
    }
    uint32_t idp = (uint32_t)c->gpr[3];
    if (idp && vm_base) { uint8_t* p = vm_base + idp; for (int i=0;i<7;i++) p[i]=0; p[7]=1; }
    c->gpr[3] = 0;
}
/* libsre's profiling/misc imports we have no behaviour for: a CELL_OK no-op is
 * correct (cellLibprof is a dev profiler; the unnamed sysPrxForUser NIDs are
 * tolerated as 0). */
static int  libsre_noop_ok(void) { return 0; }

static int ydkj_spu_image_close_stub(void) { return 0; } /* sys_spu_image_close */

/* Adapter to the prx_register_fn signature the loader expects. */
static void ydkj_prx_register(uint32_t addr, void (*host)(void*))
{
    ppu_register_function((uint64_t)addr, host);
}

/* ----------------------------------------------------------------------------
 * libsre import linking.
 *
 * libsre imports 32 functions (cellLibprof x2, sysPrxForUser x30) through PRX
 * import trampolines at 0x3001D718..0x3001DAF8. Each trampoline loads an OPD
 * pointer from libsre's import function-pointer table (slots at 0x3002E000 +
 * i*4), reads {code, toc} from that OPD, copies toc->r2 (the import NID slot in
 * a normal PS3 link is the resolved OPD), and jumps. On the real console the
 * lv2 loader fills those slots; our PRX loader does not, so every slot holds 0
 * and the trampoline jumps to garbage (0x39800000) -> cellSpursInitialize
 * fails. We resolve them here: point each slot at a synthetic OPD whose code is
 * a registered host thunk and whose toc carries the NID, so the trampoline's
 * `bctr` lands in libsre_import_thunk, which forwards to the HLE handler.
 *
 * NIDs are in trampoline/slot order (slot i == trampoline 0x1D718 + i*0x20),
 * extracted from libsre.prx's import descriptor (tools/elf_parser.py). -------*/
extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx);

static void libsre_import_thunk(ppu_context* ctx)
{
    /* The trampoline put the import NID in r2 (lwz r2,4(opd)) and saved the
     * caller's real TOC at 0x28(r1) (std r2,0x28). Recover the NID, restore the
     * TOC so the caller's post-call `ld r2,0x28(r1)` sees the right value, then
     * dispatch. */
    uint32_t nid = (uint32_t)ctx->gpr[2];
    ctx->gpr[2]  = (uint32_t)vm_read64(ctx->gpr[1] + 0x28);
    ps3_hle_call(nid, ctx);
}

static void libsre_link_imports(void)
{
    static const uint32_t kImportNids[32] = {
        0x05893E7C, 0x6D045C2E, 0x04E83D2C, 0x052D29A6, 0x0618936B, 0x06574237,
        0x1573DC3F, 0x1BC200F4, 0x1C9A942C, 0x24A1EA07, 0x2A6D9D51, 0x2D36462B,
        0x2F85C0EF, 0x5FDFB2FE, 0x68B9B011, 0x6BF66EA7, 0x996F7CF8, 0x99C88692,
        0x9F04F7AF, 0x9FB6228E, 0xAFF080A4, 0xC3476D0C, 0xD3039D4D, 0xDA0EB71A,
        0xDD0C1E09, 0xE0998DBF, 0xE0DA8EFD, 0xE75C40F2, 0xEBE5F72F, 0xEF87A695,
        0xFA7F693D, 0xFB5DB080,
    };
    const uint32_t SLOT_BASE  = 0x3002E000u; /* libsre import func-ptr table   */
    const uint32_t THUNK_CODE = 0x30040000u; /* synthetic OPD code target      */
    const uint32_t OPD_BASE   = 0x30041000u; /* 32 OPDs, 8 bytes each          */

    ppu_register_function((uint64_t)THUNK_CODE, (void(*)(void*))libsre_import_thunk);
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t opd = OPD_BASE + i * 8u;
        vm_write32(opd + 0, THUNK_CODE);
        vm_write32(opd + 4, kImportNids[i]);
        vm_write32(SLOT_BASE + i * 4u, opd);
    }
    fprintf(stderr, "[init] libsre imports linked: 32 slots @0x%08X -> HLE dispatch\n", SLOT_BASE);
}

extern "C" void ps3_load_prx_modules(void)
{
    { extern void flow_heap_init(void); flow_heap_init(); }   /* CRT heap (always) */

    /* Proven path (flОw) but under active integration: libsre's lifted code still
     * has TOC/GOT-relocation gaps + unresolved mid-function jump-table targets
     * that leave SPURS construction incomplete, so it currently gets the title
     * stuck earlier than the HLE-stub path. Gate behind YDKJ_LIBSRE=1 so the
     * default build keeps the stub behaviour until the libsre execution is fixed.
     * Set YDKJ_LIBSRE=1 to dispatch cellSpurs into real recompiled Sony code. */
    { const char* e = getenv("YDKJ_LIBSRE"); if (!(e && e[0]=='1')) {
        fprintf(stderr, "[init] libsre disabled (set YDKJ_LIBSRE=1 to load real cellSpurs)\n");
        return; } }

    /* libsre.linked.bin lives in the project's prx/ (cwd is the project dir when
     * launched as ./build/ydkj_boot.exe input/EBOOT.elf). */
    const char* candidates[] = {
        "prx/libsre.linked.bin",
        "../prx/libsre.linked.bin",
        "D:/recomp/ps3games/youdontknowjack/project/prx/libsre.linked.bin",
    };
    uint8_t* img = 0; uint32_t sz = 0;
    for (unsigned i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        img = prx_image_load_file(candidates[i], &sz);
        if (img) { fprintf(stderr, "[init] libsre image: %s (%u bytes)\n", candidates[i], sz); break; }
    }
    if (!img) {
        fprintf(stderr, "[init] libsre.linked.bin not found -- cellSpurs uses HLE stubs\n");
        return;
    }

    prx_module m;
    memset(&m, 0, sizeof(m));
    m.name         = "libsre";
    m.base         = 0x30000000;   /* must match the relocate/lift base */
    m.image        = img;
    m.image_size   = sz;
    m.funcs        = libsre_function_table;
    m.func_count   = libsre_function_table_count;
    m.exports      = libsre_exports;
    m.export_count = libsre_export_count;

    prx_load_result r = prx_load_module(&m, ydkj_prx_register);
    fprintf(stderr, "[init] libsre load %s: %u funcs registered, %u exports in registry\n",
            r.ok ? "OK" : "FAILED", r.funcs_registered, prx_export_registry_count());
    free(img);

    /* Resolve libsre's 32 firmware imports (PRX trampolines -> HLE). Without this
     * cellSpursInitialize jumps to garbage (0x39800000) on its first sys_* call. */
    libsre_link_imports();

    /* DIAGNOSTIC (FLOW_GUARD_TOC=1): the cellSpursInitialize OPD's toc word reads
     * garbage at dispatch though the image is correct -> a stray runtime store
     * clobbers libsre's data. Confirm the OPD is right immediately after load,
     * then arm a page-guard on the toc word to catch the writer's host RIP
     * (map via flow.map). */
    if (getenv("FLOW_GUARD_TOC")) {
        extern uint32_t vm_read32(uint64_t);
        extern void ppu_guard_page(uint32_t);
        const uint32_t opd = 0x3003127Cu;          /* cellSpursInitialize OPD */
        fprintf(stderr, "[guard-toc] OPD@0x%08X post-load: code=0x%08X toc=0x%08X\n",
                opd, vm_read32(opd), vm_read32(opd + 4));
        ppu_guard_page(0x30031280u);               /* the toc word */
    }

    /* Supplemental sysPrxForUser CRT shims for libsre's imports. */
    ps3_hle_register(0x99C88692u, "_sys_strcpy",  (void*)_sys_strcpy);
    ps3_hle_register(0x996F7CF8u, "_sys_strncat", (void*)_sys_strncat);
    ps3_hle_register(0x04E83D2Cu, "_sys_strncmp", (void*)_sys_strncmp);
    ps3_hle_register(0xE0DA8EFDu, "sys_spu_image_close", (void*)ydkj_spu_image_close_stub);

    /* Thread primitives cellSpursInitialize uses to spawn its SPURS handler
     * thread (ctx-ABI). Registered as ctx handlers so the import thunk reaches
     * the real syscall impl. */
    ps3_hle_register_ctx(0x24A1EA07u, "sys_ppu_thread_create", wrap_ppu_thread_create);
    ps3_hle_register_ctx(0xAFF080A4u, "sys_ppu_thread_exit",   wrap_ppu_thread_exit);

    /* lwmutex/lwcond: translate guest->host pointer then call the real impl. */
    ps3_hle_register_ctx(0x2F85C0EFu, "sys_lwmutex_create",  w_lwmutex_create);
    ps3_hle_register_ctx(0x1573DC3Fu, "sys_lwmutex_lock",    w_lwmutex_lock);
    ps3_hle_register_ctx(0x1BC200F4u, "sys_lwmutex_unlock",  w_lwmutex_unlock);
    ps3_hle_register_ctx(0xC3476D0Cu, "sys_lwmutex_destroy", w_lwmutex_destroy);
    ps3_hle_register_ctx(0xDA0EB71Au, "sys_lwcond_create",   w_lwcond_create);
    ps3_hle_register_ctx(0x2A6D9D51u, "sys_lwcond_wait",     w_lwcond_wait);
    ps3_hle_register_ctx(0xEF87A695u, "sys_lwcond_signal",   w_lwcond_signal);
    ps3_hle_register_ctx(0x1C9A942Cu, "sys_lwcond_destroy",  w_lwcond_destroy);
    ps3_hle_register_ctx(0xE0998DBFu, "sys_prx_get_module_id_by_name", w_prx_get_module_id);
    ps3_hle_register_ctx(0xE75C40F2u, "sys_process_get_paramsfo",      w_process_paramsfo);
    ps3_hle_register_ctx(0x9F04F7AFu, "_sys_printf",                   w_sys_printf);
    ps3_hle_register_ctx(0x350D454Eu, "sys_ppu_thread_get_id",         w_get_thread_id_trace);

    /* libsre's profiling / unnamed sysPrxForUser imports -> CELL_OK no-op. */
    ps3_hle_register(0x05893E7Cu, "cellLibprof_a",   (void*)libsre_noop_ok);
    ps3_hle_register(0x6D045C2Eu, "cellLibprof_b",   (void*)libsre_noop_ok);
    ps3_hle_register(0x0618936Bu, "sysPrx_0618936B", (void*)libsre_noop_ok);
    ps3_hle_register(0x5FDFB2FEu, "sysPrx_5FDFB2FE", (void*)libsre_noop_ok);
    ps3_hle_register(0x9FB6228Eu, "sysPrx_9FB6228E", (void*)libsre_noop_ok);
    ps3_hle_register(0xDD0C1E09u, "sysPrx_DD0C1E09", (void*)libsre_noop_ok);
    ps3_hle_register(0xEBE5F72Fu, "sysPrx_EBE5F72F", (void*)libsre_noop_ok);
    ps3_hle_register(0xFA7F693Du, "sysPrx_FA7F693D", (void*)libsre_noop_ok);
}
