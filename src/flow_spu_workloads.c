/* flow_spu_workloads.c — actually RUN flОw's SPURS workloads.
 *
 * The problem this solves: flОw calls cellSpursAddWorkload() three times (plus
 * two on a second SPURS instance). The HLE cellSpurs merely RECORDS each
 * workload (pm + size) and nothing ever executes it, so flОw's SPU-side
 * simulation (the fluid/velocity grid that produces the scene) never runs. The
 * render loop then blocks forever in cond_wait(250ms) waiting for a scene job
 * that never lands -> draws=0, blank screen.
 *
 * Key discovery: flОw's workload images are RAW SPU code (no ELF header) --
 * the bytes at pm start with SPU immediate loads. The project's existing 60
 * lifted SPU images were all found by ELF-magic scanning, and NONE of them sit
 * at the four workload addresses, which is why earlier SPU work never affected
 * the game. These four images were extracted directly at the pm addresses the
 * game passes:
 *
 *   pm=0x10062600 size=4800   fp=0xF55811FC3096951B
 *   pm=0x10063900 size=14992  fp=0x2A9A01FEFFCA05AA
 *   pm=0x10067400 size=4160   fp=0x92BE7AC91648AFFE
 *   pm=0x1010EC00 size=11136  fp=0x00BB5D3084E4A869
 *
 * In HLE mode there is no SPURS SPU kernel scheduling anything, so *we* are the
 * scheduler: when the game adds a workload we look its image up by fingerprint
 * and run the lifted SPU program on a detached host thread (raw image -> LS@0,
 * entry 0, workload `data` in r3 per the SPURS ABI).
 *
 * ponytail: one thread per workload, no SPU pool. These are a handful of
 * long-lived workers; add a pool only if flОw ever adds many.
 */
#include "spu_context.h"
#include "spu_workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern int  spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_begin_image(int image_id);

/* SPURS policy modules are POSITION-DEPENDENT and live at LS 0xA00 — the SPURS
 * kernel MFC-DMAs the module there and branches to it (see src/flow_spurs_kernel.c:
 * "the policy (lift_pol, LS 0xA00-0x47C0)"). Loaded at LS 0 instead, every absolute
 * address inside the module is off by 0xA00, so it reads garbage and its atomic
 * loop locks on EA 0 forever (that was the observed hang). Lift AND load at 0xA00. */
#define FLOW_SPU_POLICY_LS_BASE 0xA00u

/* The four lifted workload images (distinct symbol prefixes so they co-link). */
extern void wl0_spu_func_00000A00(spu_context*);
extern void wl1_spu_func_00000A00(spu_context*);
extern void wl2_spu_func_00000A00(spu_context*);
extern void wl3_spu_func_00000A00(spu_context*);
extern void wl0_spu_recomp_register(void);
extern void wl1_spu_recomp_register(void);
extern void wl2_spu_recomp_register(void);
extern void wl3_spu_recomp_register(void);
extern void sk_a_spu_recomp_register(void);   /* SPURS kernel (LS 0..0x834) */

/* Keyed on the workload's guest ADDRESS, not a fingerprint: the image in guest
 * RAM does NOT match the on-disk EBOOT bytes (the loader/game patches the SPU
 * module after load — runtime fp 0xFE7691B6.. vs on-disk 0xF55811FC.. for
 * pm=0x10062600), so a content hash can never match. The pm addresses are fixed
 * data-segment locations and are the stable key. */
typedef struct {
    uint32_t pm;                 /* guest EA the game passes to AddWorkload      */
    uint32_t size;               /* sizePm                                       */
    void   (*entry)(spu_context*);
    int      image_id;           /* for spu_indirect_branch resolution           */
    const char* name;
} flow_wl;

static const flow_wl s_wl[] = {
    { 0x10062600u, 4800u,  wl0_spu_func_00000A00, 30, "flow_wl_0_62600"  },
    { 0x10063900u, 14992u, wl1_spu_func_00000A00, 31, "flow_wl_1_63900"  },
    { 0x10067400u, 4160u,  wl2_spu_func_00000A00, 32, "flow_wl_2_67400"  },
    { 0x1010EC00u, 11136u, wl3_spu_func_00000A00, 33, "flow_wl_3_10EC00" },
};
#define FLOW_WL_COUNT ((int)(sizeof(s_wl)/sizeof(s_wl[0])))

/* ---- the SPU job thread -------------------------------------------------- */

typedef struct {
    const flow_wl* wl;
    uint32_t pm;        /* guest EA of the raw image */
    uint32_t size;
    uint64_t data;      /* workload arg (SPURS passes this to the module)      */
    uint32_t spurs;     /* CellSpurs instance EA — the module's whole world    */
    uint32_t tid;
} wl_run_t;

/* --- PC sampler: name the loop each module idles in (FLOW_SPU_PCS=1) -------- */
static volatile spu_context* g_pcs[FLOW_WL_COUNT];
static const char*           g_pcs_name[FLOW_WL_COUNT];
static volatile long         g_pcs_on;
static uint32_t              g_pcs_instance;   /* CellSpurs instance EA to watch */

#ifdef _WIN32
static DWORD WINAPI flow_pcs_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        Sleep(1000);
        for (int i = 0; i < FLOW_WL_COUNT; i++) {
            spu_context* c = (spu_context*)g_pcs[i];
            if (!c) continue;
            /* pc = LS address of the instruction currently executing. Sampling it
             * repeatedly shows the idle loop's extent. Also dump the channel the
             * module is likely polling (r3 = its arg / queue ptr). */
            /* The modules spin in a GETLLAR/PUTLLC atomic loop: they fetch a
             * 128-byte line from EA (r11:r10 = EAH:EAL) into LS 0x1E00 and retry
             * while the lock word is non-zero. Dump the EA, the fetched line, and
             * what guest memory actually holds there — if the CellSpurs instance
             * isn't in the real Sony layout, that word is garbage and never
             * clears, which is exactly this hang. */
            uint32_t eal = c->gpr[10]._u32[0];
            const uint8_t* ls = (const uint8_t*)c->ls + 0x1E00;
            uint32_t lsw = ((uint32_t)ls[0]<<24)|((uint32_t)ls[1]<<16)|((uint32_t)ls[2]<<8)|ls[3];
            uint32_t memw = 0;
            if (vm_base && eal && eal < 0x20000000u) {
                const uint8_t* m = vm_base + eal;
                memw = ((uint32_t)m[0]<<24)|((uint32_t)m[1]<<16)|((uint32_t)m[2]<<8)|m[3];
            }
            /* Did the entry's DMA of the CellSpurs instance actually land? The
             * module does MFC_LSA=0x1D80, size=128 from EA(r4). If LS[0x1D80] is
             * zeros while guest memory at the instance is populated, the MFC never
             * executed and EVERY derived value (incl. the lock EA) is zero. */
            const uint8_t* dm = (const uint8_t*)c->ls + 0x1D80;
            uint32_t d0 = ((uint32_t)dm[0]<<24)|((uint32_t)dm[1]<<16)|((uint32_t)dm[2]<<8)|dm[3];
            uint32_t d1 = ((uint32_t)dm[4]<<24)|((uint32_t)dm[5]<<16)|((uint32_t)dm[6]<<8)|dm[7];
            /* THE kernel scheduler's input. sk_a func_324 GETLLARs (cmd 0xD0) the
             * instance's first 0x80 bytes into LS 0x100 and selects a wid from it:
             *   LS 0x100 -> instance 0x00 = wklReadyCount1  (we set 01 01 01 ...)
             *   LS 0x170 -> instance 0x70 = wklSignal1
             *   LS 0x180 -> instance 0x80 = wklState1       (RUNNABLE = 2)
             * If these are ZERO the GETLLAR never delivered and the scheduler is
             * choosing from an empty header — no amount of state encoding helps. */
            const uint8_t* kc = (const uint8_t*)c->ls;
            /* The policy-load guard in sk_a func_6C0 is:
             *   skip = gb(ceq(LS[0x1D0], LS[0x3FFE0])) > 11
             * LS[0x3FFE0] = the wklInfo it just DMA'd (addr u64 + arg u64)
             * LS[0x1D0]   = SpursKernelContext.wklCurrentId region ("already loaded")
             * Dump both: if they compare equal in words 0+1 the kernel thinks the
             * policy is already resident and never DMAs it to LS 0xA00. */
            {
                const uint8_t* wi = vm_base ? vm_base + g_pcs_instance + 0xB00 : NULL;
                fprintf(stderr, "[GUARD] LS[0x1D0]=%02X%02X%02X%02X %02X%02X%02X%02X  "
                                "LS[0x3FFE0]=%02X%02X%02X%02X %02X%02X%02X%02X  || "
                                "GUEST wklInfo1[0]@+0xB00=%02X%02X%02X%02X %02X%02X%02X%02X "
                                "size=%02X%02X%02X%02X\n",
                    kc[0x1D0],kc[0x1D1],kc[0x1D2],kc[0x1D3], kc[0x1D4],kc[0x1D5],kc[0x1D6],kc[0x1D7],
                    kc[0x3FFE0],kc[0x3FFE1],kc[0x3FFE2],kc[0x3FFE3],
                    kc[0x3FFE4],kc[0x3FFE5],kc[0x3FFE6],kc[0x3FFE7],
                    wi?wi[0]:0, wi?wi[1]:0, wi?wi[2]:0, wi?wi[3]:0,
                    wi?wi[4]:0, wi?wi[5]:0, wi?wi[6]:0, wi?wi[7]:0,
                    wi?wi[0x10]:0, wi?wi[0x11]:0, wi?wi[0x12]:0, wi?wi[0x13]:0);
            }
            /* Watch the INSTANCE state machine in guest memory — the kernel
             * GETLLAR/PUTLLCs it, so this shows what it's actually deciding. */
            const uint8_t* in = vm_base ? vm_base + g_pcs_instance : NULL;
            if (in) {
                fprintf(stderr,
                    "[FLOW-SPU-PC] %s pc=0x%05X | KRNCTX ready[0x100]=%02X%02X%02X "
                    "sig[0x170]=%02X%02X || INST ready=%02X%02X%02X state=%02X%02X%02X "
                    "status=%02X%02X%02X mskA=%02X%02X%02X%02X curCont=%02X%02X%02X "
                    "sysSrv[0xD00].addr=%02X%02X%02X%02X size=%02X%02X%02X%02X\n",
                    g_pcs_name[i], c->pc & 0x3FFFF,
                    kc[0x100], kc[0x101], kc[0x102], kc[0x170], kc[0x171],
                    in[0x00], in[0x01], in[0x02],          /* wklReadyCount1 */
                    in[0x80], in[0x81], in[0x82],          /* wklState1      */
                    in[0x90], in[0x91], in[0x92],          /* wklStatus1     */
                    in[0xB0], in[0xB1], in[0xB2], in[0xB3],/* wklMskA        */
                    in[0x40], in[0x41], in[0x42],          /* curContention  */
                    in[0xD04], in[0xD05], in[0xD06], in[0xD07],   /* sysSrv addr lo */
                    in[0xD10], in[0xD11], in[0xD12], in[0xD13]);  /* sysSrv size    */
            }
            (void)memw; (void)lsw; (void)d0; (void)d1; (void)eal;
        }
        fflush(stderr);
    }
    return 0;
}
#endif

static void flow_pcs_register(int idx, spu_context* c, const char* name)
{
#ifdef _WIN32
    if (!getenv("FLOW_SPU_PCS")) return;
    if (idx >= 0 && idx < FLOW_WL_COUNT) { g_pcs[idx] = c; g_pcs_name[idx] = name; }
    if (InterlockedExchange(&g_pcs_on, 1) == 0) {
        HANDLE h = CreateThread(NULL, 0, flow_pcs_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
#else
    (void)idx; (void)c; (void)name;
#endif
}

static void flow_wl_run(wl_run_t* r)
{
    spu_context ctx;
    spu_context_init(&ctx, r->tid);
    ctx.image_id = r->wl->image_id;

    /* RAW image: plain SPU code, no program headers. Place it where the SPURS
     * kernel would DMA it — LS 0xA00 — because the module is position-dependent. */
    uint32_t n = r->size;
    if (n > SPU_LS_SIZE - FLOW_SPU_POLICY_LS_BASE)
        n = SPU_LS_SIZE - FLOW_SPU_POLICY_LS_BASE;
    memcpy(ctx.ls + FLOW_SPU_POLICY_LS_BASE, vm_base + r->pm, n);

    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;      /* stack top, 16-aligned      */

    /* Policy-module entry ABI (confirmed by reading the lifted entry at 0xA00:
     * it immediately stashes r3 -> LS 0x1D10 and r4 -> LS 0x1D00, i.e. it takes
     * TWO u64 args):
     *   r3 = the workload's `data` argument
     *   r4 = the CellSpurs instance EA   ("libsre delivers it in r4 (arg1)",
     *        per src/flow_spurs_kernel.c)
     * r4 was previously left uninitialised, so the module's atomic loop derived a
     * lock EA of 0 and spun forever. */
    ctx.gpr[3]._u32[0] = (uint32_t)(r->data >> 32);
    ctx.gpr[3]._u32[1] = (uint32_t)(r->data & 0xFFFFFFFFu);

    /* r4 is a 128-bit quadword carrying TWO EAs, not one u64. Traced:
     *   func_1860: MFC_EAH=r4.w0, MFC_EAL=r4.w1  -> DMA source  (words 0:1)
     *              r80 = rotqbyi(r4, 8)                          (words 2:3 -> 0:1)
     *   func_18E0: r4 = r80 -> func_1900 -> EAH/EAL              (the LOCK EA)
     * So words 0:1 = the instance to DMA in, words 2:3 = the EA the GETLLAR/PUTLLC
     * atomic locks on. Leaving words 2:3 zero is what made it lock on address 0. */
    ctx.gpr[4]._u32[0] = 0;                       /* DMA  EA hi32 */
    ctx.gpr[4]._u32[1] = r->spurs;                /* DMA  EA lo32 */
    ctx.gpr[4]._u32[2] = 0;                       /* LOCK EA hi32 */
    ctx.gpr[4]._u32[3] = r->spurs;                /* LOCK EA lo32 */

    /* THE thing that makes a policy module work: r80.
     *
     * Traced out of the lifted code — the module's lock helper is reached via
     *   func_18E0: r3 = r81 ; r4 = r80 ; call func_1900
     *   func_1900: r7 = r4 ; ... ; r11 = r7 (EAH) ; r10 = r7<<4bytes (EAL)
     * i.e. the GETLLAR/PUTLLC atomic locks on the 64-bit EA held in **r80**
     * (hi in word0, lo in word1). For a POLICY module r80 is the CellSpurs
     * instance EA — NOT the kernel's "SpursKernelContext LS base = 0x100"
     * convention in src/flow_spurs_kernel.c (that's for the kernel image).
     * Seeding r80=0x100 the kernel's way yields EA {hi=0x100, lo=0} -> lock on
     * address 0 -> spin forever, which is exactly what we observed.
     *
     * r81 carries the value the atomic writes (left as the module finds it). */
    ctx.gpr[80]._u32[0] = 0;              /* instance EA hi32 */
    ctx.gpr[80]._u32[1] = r->spurs;       /* instance EA lo32 */

    /* Also leave the instance EA in the SpursKernelContext slot the kernel would
     * have DMA'd (LS[0x1C0], u64 BE) — some paths read it from there. */
    {
        uint32_t inst = r->spurs;
        uint8_t* p = ctx.ls + 0x1C0;
        p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;                 /* EA hi32 = 0 */
        p[4] = (uint8_t)(inst >> 24); p[5] = (uint8_t)(inst >> 16);
        p[6] = (uint8_t)(inst >> 8);  p[7] = (uint8_t)inst;     /* EA lo32     */
    }

    fprintf(stderr, "[FLOW-SPU] %s: running (pm=0x%08X size=%u arg=0x%016llX spurs=0x%08X img=%d)\n",
            r->wl->name, r->pm, r->size, (unsigned long long)r->data,
            r->spurs, r->wl->image_id);
    fflush(stderr);

    flow_pcs_register(r->tid - 0x3000, &ctx, r->wl->name);
    spu_run_with_halt(r->wl->entry, &ctx);

    fprintf(stderr, "[FLOW-SPU] %s: returned (status=%d pc=0x%05X)\n",
            r->wl->name, ctx.status, ctx.pc & 0x3FFFF);
    fflush(stderr);
}

#ifdef _WIN32
static DWORD WINAPI flow_wl_thread(LPVOID p)
{
    wl_run_t* r = (wl_run_t*)p;
    /* SPURS workloads are persistent workers: the module runs, processes the
     * queue, and halts. Re-run it so it keeps servicing work the PPU posts,
     * rather than dying after one pass. */
    if (getenv("FLOW_SPU_ONCE")) {
        flow_wl_run(r);
    } else {
        for (long i = 0; i < 2000000L; i++) {
            flow_wl_run(r);
            Sleep(4);      /* ~250 Hz; yields to the PPU/render threads */
        }
    }
    free(r);
    return 0;
}
#endif

/* Dispatch the workload image the game just added, if we have a lift for it. */
static void flow_wl_dispatch(uint32_t pm, uint32_t size, uint64_t data, uint32_t spurs)
{
    if (!vm_base || !pm || !size) return;
    /* OFF by default: bare-starting a policy module is the wrong model (see the
     * kernel section above) — the SPURS kernel dispatches them now. Kept behind
     * FLOW_SPU_BARE for A/B comparison. */
    if (!getenv("FLOW_SPU_BARE")) return;
    if (getenv("FLOW_NO_SPU_WL")) return;

    for (int i = 0; i < FLOW_WL_COUNT; i++) {
        if (s_wl[i].pm != pm) continue;
        /* Sanity: the live image should still be the code we lifted. Dump the
         * first words so a mismatch (game patched the module) is visible rather
         * than silently running a lift of different code. */
        const uint8_t* p = vm_base + pm;
        fprintf(stderr, "[FLOW-SPU] match %s pm=0x%08X size=%u (game said %u) head=%02X%02X%02X%02X %02X%02X%02X%02X\n",
                s_wl[i].name, pm, s_wl[i].size, size,
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7]);
#ifdef _WIN32
        wl_run_t* r = (wl_run_t*)calloc(1, sizeof(*r));
        r->wl = &s_wl[i]; r->pm = pm;
        r->size = size ? size : s_wl[i].size;
        r->data = data;
        r->spurs = spurs;
        r->tid = 0x3000 + i;
        HANDLE h = CreateThread(NULL, 0, flow_wl_thread, r, 0, NULL);
        if (h) CloseHandle(h);
#endif
        return;
    }
    fprintf(stderr, "[FLOW-SPU] no lift for workload pm=0x%08X size=%u\n", pm, size);
    fflush(stderr);
}

/* ---- REAL CellSpurs instance layout -------------------------------------- *
 *
 * The HLE CellSpurs (libs/spurs/cellSpurs.h) is a ~290-byte host convenience
 * struct { initialized; nSpus; flags; prefix[16]; _internal; _padding[256] }.
 * But the SPU policy module DMAs the FIRST 0x80 BYTES of the instance straight
 * out of guest memory (entry -> func_1860: MFC_Size=128, EA from r4) and derives
 * its workload state and lock EAs from those fields. Against the stub it reads
 * `initialized/nSpus/flags/prefix` as if they were the workload queue -> garbage
 * -> it locks on EA 0 and spins forever. No register seeding can fix that; the
 * shared-memory CONTRACT has to be right.
 *
 * AUTHORITATIVE LAYOUT — from RPCS3's cellSpurs.h (the open-source ground truth
 * for this firmware struct), NOT hand-inferred. Several offsets I had guessed
 * earlier were WRONG and broke the scheduler:
 *   - wklCurrentContention is 0x20, NOT 0x40 (I'd put maxContention at 0x20)
 *   - wklMaxContention is 0x50, NOT 0x20
 *   - 0xB0 is wklEnabled (u32), mskB at 0xB4
 *   - CellSpursWorkloadInfo is { data u64 @0x00; priority u64 @0x08;
 *     policyModule u32 @0x10; sizePolicyModule u32 @0x14 } — 0x30 stride.
 *     I had been writing the policy address at wklInfo+0x00/0x04 (as "addr"),
 *     but the kernel's dispatch DMA (sk_a func_6C0) reads policyModule/size from
 *     +0x10/+0x14 — THAT is why the fetched wklInfo (LS 0x3FFE0) was garbage.
 *
 *   0x000  wklReadyCount1[16]       u8 per wid   (non-zero = work pending)
 *   0x010  wklIdleSpuCountOrReadyCount2[16]
 *   0x020  wklCurrentContention[16] u8 per wid
 *   0x030  wklPendingContention[16]
 *   0x040  wklMinContention[16]
 *   0x050  wklMaxContention[16]
 *   0x060  wklFlag[16]
 *   0x070  wklSignal1               u16 BE, bit = 0x8000 >> (wid & 15)
 *   0x076  nSpus                    u8
 *   0x080  wklState1[16]            u8 per wid   (2 = RUNNABLE)
 *   0x090  wklStatus1[16]
 *   0x0A0  wklEvent1[16]
 *   0x0B0  wklEnabled               u32 BE, bit = 0x80000000 >> wid
 *   0x0B4  wklMskB                  u32
 *   0xB00  wklInfo1[16]             CellSpursWorkloadInfo, 0x30 stride
 *   0xD00  wklInfoSysSrv            CellSpursWorkloadInfo
 *   total size 0x2000
 *
 * The instance lives in the GAME's memory (it passes the pointer), so we don't
 * allocate — we just write the correct layout into it.
 */
#define SPURS_INSTANCE_SIZE   0x2000u
#define SPURS_WKL_MAX         16u
#define SPURS_OFF_READYCOUNT1 0x000u
#define SPURS_OFF_READYCOUNT2 0x010u
#define SPURS_OFF_CURCONT     0x020u
#define SPURS_OFF_PENDCONT    0x030u
#define SPURS_OFF_MINCONT     0x040u
#define SPURS_OFF_MAXCONT     0x050u
#define SPURS_OFF_SIGNAL1     0x070u
#define SPURS_OFF_NSPUS       0x076u
#define SPURS_OFF_STATE1      0x080u
#define SPURS_OFF_STATUS1     0x090u
#define SPURS_OFF_ENABLED     0x0B0u
#define SPURS_OFF_WKLINFO1    0xB00u
/* WorkloadInfo layout — read from THIS firmware's kernel (sk_a func_6C0), which
 * is authoritative over RPCS3 master (a different SDK rev). The kernel does:
 *   fetch 0x20 bytes of wklInfo[wid] (stride 0x20, NOT the 0x30 in RPCS3 master)
 *   gpr[4]=LS[wi+0x00]; MFC_EAH=gpr[4]; MFC_EAL=gpr[4]<<4bytes  => addr @ 0x00 (u64)
 *   MFC_Size = LS[wi+0x10]                                      => size @ 0x10
 * i.e. { addr u64 @0x00; ... ; size u32 @0x10 } with a 0x20 stride. */
#define SPURS_WKLINFO_STRIDE  0x20u
#define WKLINFO_ADDR          0x00u   /* u64 — policy module EA          */
#define WKLINFO_ARG           0x08u   /* u64 — workload data arg         */
#define WKLINFO_SIZE          0x10u   /* u32 — policy module size        */
#define SPURS_WKL_STATE_RUNNABLE 2u

static void be32w(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t be32r(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Lay down a clean real-layout instance. Called on cellSpursInitialize, AFTER
 * the HLE has written its stub (we overwrite it). */
static void flow_spurs_build_instance(uint32_t ea, uint32_t nSpus)
{
    if (!vm_base || !ea) return;
    uint8_t* in = vm_base + ea;
    memset(in, 0, SPURS_INSTANCE_SIZE);
    /* nSpus @0x76 — the scheduler reads it to know how many SPUs it schedules
     * across. RPCS3 cellSpurs.h places it here; libsre's kernel uses it. */
    if (nSpus == 0 || nSpus > 6) nSpus = 1;
    in[SPURS_OFF_NSPUS] = (uint8_t)nSpus;
    fprintf(stderr, "[FLOW-SPURS] real-layout instance @0x%08X (%u bytes, nSpus=%u)\n",
            ea, SPURS_INSTANCE_SIZE, nSpus);
    fflush(stderr);
}

/* Publish a workload into the instance so the policy module can find and run it. */
static void flow_spurs_publish_wkl(uint32_t ea, uint32_t wid, uint32_t pm,
                                   uint32_t sizePm, uint64_t data,
                                   uint32_t minC, uint32_t maxC)
{
    if (!vm_base || !ea || wid >= SPURS_WKL_MAX) return;
    uint8_t* in = vm_base + ea;

    /* WorkloadInfo for THIS firmware: addr(u64)@0x00, arg(u64)@0x08, size@0x10. */
    uint8_t* wi = in + SPURS_OFF_WKLINFO1 + wid * SPURS_WKLINFO_STRIDE;
    be32w(wi + WKLINFO_ADDR + 0, 0);                       /* policy EA hi32 */
    be32w(wi + WKLINFO_ADDR + 4, pm);                      /* policy EA lo32 */
    be32w(wi + WKLINFO_ARG  + 0, (uint32_t)(data >> 32));  /* data arg hi32  */
    be32w(wi + WKLINFO_ARG  + 4, (uint32_t)data);          /* data arg lo32  */
    be32w(wi + WKLINFO_SIZE,     sizePm);                  /* module size    */

    in[SPURS_OFF_READYCOUNT1 + wid] = 1;                       /* work pending  */
    in[SPURS_OFF_STATE1      + wid] = SPURS_WKL_STATE_RUNNABLE;
    in[SPURS_OFF_MINCONT     + wid] = (uint8_t)(minC ? minC : 1);
    in[SPURS_OFF_MAXCONT     + wid] = (uint8_t)(maxC ? maxC : 1);
    in[SPURS_OFF_CURCONT     + wid] = 0;                       /* nobody running it yet */

    /* wklSignal1: u16 BE, MSB-first per wid */
    uint32_t sig = ((uint32_t)in[SPURS_OFF_SIGNAL1] << 8) | in[SPURS_OFF_SIGNAL1 + 1];
    sig |= (0x8000u >> (wid & 15));
    in[SPURS_OFF_SIGNAL1]     = (uint8_t)(sig >> 8);
    in[SPURS_OFF_SIGNAL1 + 1] = (uint8_t)sig;

    /* wklEnabled: u32 BE, MSB-first per wid */
    uint32_t msk = be32r(in + SPURS_OFF_ENABLED);
    msk |= (0x80000000u >> wid);
    be32w(in + SPURS_OFF_ENABLED, msk);

    fprintf(stderr, "[FLOW-SPURS] wkl%u published: policyModule=0x%08X size=%u data=0x%08X "
                    "ready=1 state=RUNNABLE enabled=0x%08X sig=0x%04X minC=%u maxC=%u\n",
            wid, pm, sizePm, (uint32_t)data, msk, sig,
            (unsigned)in[SPURS_OFF_MINCONT + wid], (unsigned)in[SPURS_OFF_MAXCONT + wid]);

    /* --- wklInfoSysSrv hijack (FLOW_SPURS_HIJACK) --------------------------
     * RE result: the kernel's selector currently returns wid=16, which its own
     * `wid > 15` path (func_6C0: clgti(r3,15)) maps to the FIXED wklInfoSysSrv
     * slot at instance+0xD00 — i.e. "no user workload chosen, run the system
     * service". Confirmed from the trace: it GETs ea=instance+0xD00 exactly
     * (0xD00-0xB00 = 0x200 = 16*32), never instance+0xB00+wid*32 for wid 0..2,
     * even though ready/state/mskA/signal are all set for them.
     *
     * The kernel ALWAYS dispatches that slot. We have no SPURS system service
     * (that's libsre's), so pointing it at one of flОw's policies exercises the
     * real dispatch path: kernel DMAs the policy to LS 0xA00 -> image switch ->
     * policy runs WITH the kernel-built SpursKernelContext (which is precisely
     * what bare-starting could never provide). Proves the chain end-to-end while
     * the selector predicate is still being reversed. */
    if (wid == 0 && getenv("FLOW_SPURS_HIJACK")) {
        uint8_t* ws = in + 0xD00;   /* wklInfoSysSrv, same WorkloadInfo layout */
        be32w(ws + WKLINFO_ADDR + 4, pm);
        be32w(ws + WKLINFO_ARG  + 0, (uint32_t)(data >> 32));
        be32w(ws + WKLINFO_ARG  + 4, (uint32_t)data);
        be32w(ws + WKLINFO_SIZE,     sizePm);
        fprintf(stderr, "[FLOW-SPURS] HIJACK wklInfoSysSrv@0xD00 -> wkl0 policy "
                        "(pm=0x%08X size=%u) — kernel always dispatches this slot\n",
                pm, sizePm);
    }
    fflush(stderr);
}

/* ---- SPURS policy-module image registry (declared in spu_workload.h) ------
 * The MFC path (spu_dma.h, inlined into spu_channels.c) calls the lookup when a
 * GET lands at LS 0xA00, so the SPU's image_id follows the policy module the
 * kernel just DMA'd in and its indirect branches resolve in the right lift.
 * Implemented here rather than in the runtime lib: spu_workload.c doesn't compile
 * cleanly in this TU's include context. */
#define SPU_POLICY_MAX 16
static struct { uint32_t ea; int image_id; } s_policy[SPU_POLICY_MAX];
static unsigned s_policy_count;

void spu_policy_image_register(uint32_t ea, int image_id)
{
    if (!ea || image_id <= 0) return;
    for (unsigned i = 0; i < s_policy_count; i++)
        if (s_policy[i].ea == ea) { s_policy[i].image_id = image_id; return; }
    if (s_policy_count < SPU_POLICY_MAX) {
        s_policy[s_policy_count].ea = ea;
        s_policy[s_policy_count].image_id = image_id;
        s_policy_count++;
    }
}

int spu_policy_image_lookup(uint32_t ea)
{
    for (unsigned i = 0; i < s_policy_count; i++)
        if (s_policy[i].ea == ea) return s_policy[i].image_id;
    return 0;
}

/* ---- run the REAL SPURS SPU kernel, and let IT dispatch the policies ------ *
 *
 * Bare-starting a policy module does not work and cannot be made to work: the
 * policy expects the SpursKernelContext (LS 0x100) that the SPURS *kernel* builds
 * before branching in, so every kernel-derived value (r80, the atomic lock EA)
 * stays 0. The giveaway: the policy's own DMA target LS 0x1D80 lands INSIDE its
 * own code for the larger workloads (wl1 = 14992 B at LS 0xA00 spans 0xA00-0x4470)
 * — a policy would never self-corrupt, so the LS layout is the kernel's to choose.
 *
 * So run the real kernel (lifted as sk_a, img 22 — already registered by
 * src/flow_spurs_kernel.c). It polls the CellSpurs instance for ready workloads
 * (which we now publish in the correct layout), DMAs the winning policy to LS
 * 0xA00 and branches there; spu_policy_image_register() makes the MFC path switch
 * image_id so the branch resolves into OUR lift of that policy.
 *
 * The kernel's SPU ELF lives inside libsre (guest 0x30020380 = libsre base
 * 0x30000000 + file offset 0x020380, 0x834 bytes). We do NOT want libsre's PPU
 * cellSpurs (it spins in lwmutex init and never loads content), so we just copy
 * those bytes out of prx/libsre.linked.bin into guest RAM. */
#define SPURS_KERNEL_EA        0x30020380u
#define SPURS_KERNEL_FILE_OFF  0x020380u
#define SPURS_KERNEL_SZ        0x834u

extern void sk_a_spu_func_00000818(spu_context*);   /* SPURS kernel entry (LS 0x818) */

static int flow_load_spurs_kernel_image(void)
{
    if (!vm_base) return 0;
    FILE* f = fopen("prx/libsre.linked.bin", "rb");
    if (!f) { fprintf(stderr, "[SPURS-KRN] cannot open prx/libsre.linked.bin\n"); return 0; }
    if (fseek(f, (long)SPURS_KERNEL_FILE_OFF, SEEK_SET) != 0) { fclose(f); return 0; }
    size_t n = fread(vm_base + SPURS_KERNEL_EA, 1, SPURS_KERNEL_SZ, f);
    fclose(f);
    if (n != SPURS_KERNEL_SZ) {
        fprintf(stderr, "[SPURS-KRN] short read of kernel ELF (%zu/%u)\n", n, SPURS_KERNEL_SZ);
        return 0;
    }
    fprintf(stderr, "[SPURS-KRN] kernel SPU ELF loaded to guest 0x%08X (%u bytes, no libsre PPU)\n",
            SPURS_KERNEL_EA, SPURS_KERNEL_SZ);
    return 1;
}

typedef struct { uint32_t spurs; uint32_t tid; } krn_t;

static void flow_spurs_kernel_run(krn_t* k)
{
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return;
    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(vm_base + SPURS_KERNEL_EA, SPURS_KERNEL_SZ, ls, &entry)) {
        fprintf(stderr, "[SPURS-KRN] kernel ELF failed to load into LS\n");
        free(ls);
        return;
    }

    spu_context ctx;
    spu_context_init(&ctx, k->tid);
    ctx.image_id = 22;                        /* sk_a + pol live under image 22   */
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;  /* stack top                        */
    memcpy(ctx.ls, ls, SPU_LS_SIZE);

    /* Kernel entry ABI — decoded from func_818, which BUILDS LS[0x1C0] from the
     * entry registers (my earlier hand-seed of LS[0x1C0] was overwritten):
     *   gpr[7] = shufb(r4, LS[0x1C0], cdd(SP,0))   // insert r4's DWORD at 0x1C0+0
     *   gpr[2] = shufb(r3, gpr[7],   cwd(SP,8))    // insert r3's WORD  at 0x1C0+8
     *   LS[0x1C0] = gpr[2]
     * func_6C0 then reads LS[0x1C0] and uses WORD 0 as the instance EA. So the
     * instance must be r4's PREFERRED-SLOT high word (r4._u32[0]). Previously I put
     * it in r4._u32[1] -> it landed at LS[0x1C0].w1, leaving w0 = 0 -> the selector
     * saw instance 0 and fell through to the sysSrv slot (wid 16). */
    ctx.gpr[80]._u32[0] = 0x100;   /* SpursKernelContext LS base (kernel convention) */
    ctx.gpr[4]._u32[0]  = k->spurs;   /* -> LS[0x1C0].w0 = instance EA (what 6C0 reads) */
    ctx.gpr[4]._u32[1]  = k->spurs;
    ctx.gpr[3]._u32[0]  = k->spurs;   /* -> LS[0x1C0].w8 (secondary context field)      */
    ctx.gpr[16]._u32[0] = k->spurs;   /* context-load DMA EA (func_6C0 preferred slot)  */
    ctx.gpr[16]._u32[1] = k->spurs;

    fprintf(stderr, "[SPURS-KRN] running kernel (entry=0x%X instance=0x%08X img=22)\n",
            entry, k->spurs);
    fflush(stderr);
    g_pcs_instance = k->spurs;
    flow_pcs_register(3, &ctx, "spurs_kernel");   /* FLOW_SPU_PCS=1 to watch it */

    /* The real kernel loops forever (poll ready workload -> dispatch -> idle).
     * Our lifted one runs the scheduler and hits STOP, so re-enter it to keep it
     * polling for the workloads the PPU adds. */
    for (long i = 0; i < 5000000L && !getenv("FLOW_KRN_ONCE"); i++) {
        ctx.status = 0;
        spu_run_with_halt(sk_a_spu_func_00000818, &ctx);
#ifdef _WIN32
        Sleep(4);
#endif
    }
    free(ls);
}

#ifdef _WIN32
static DWORD WINAPI flow_krn_thread(LPVOID p)
{
    krn_t* k = (krn_t*)p;
    flow_spurs_kernel_run(k);
    free(k);
    return 0;
}
#endif

static void flow_spurs_kernel_start(uint32_t spurs)
{
#ifdef _WIN32
    static volatile long s_done = 0;
    if (getenv("FLOW_NO_SPU_KRN")) return;
    if (InterlockedExchange(&s_done, 1) != 0) return;    /* one kernel */
    if (!flow_load_spurs_kernel_image()) return;

    /* Tell the MFC path which lift to switch to when the kernel DMAs each policy
     * module to LS 0xA00. */
    for (int i = 0; i < FLOW_WL_COUNT; i++)
        spu_policy_image_register(s_wl[i].pm, s_wl[i].image_id);

    krn_t* k = (krn_t*)calloc(1, sizeof(*k));
    k->spurs = spurs; k->tid = 0x2000;
    HANDLE h = CreateThread(NULL, 0, flow_krn_thread, k, 0, NULL);
    if (h) CloseHandle(h);
#else
    (void)spurs;
#endif
}

/* ---- cellSpursInitialize interception ------------------------------------ */

/* Host-side wid bookkeeping (see the AddWorkload note below for why the HLE's
 * in-guest bookkeeping can't be used alongside the real layout). */
#define FLOW_SPURS_MAX_INST 4
static struct { uint32_t ea; uint32_t next_wid; } s_inst[FLOW_SPURS_MAX_INST];

/* Don't call the HLE cellSpursInitialize either — it would write its stub fields
 * over the very bytes the SPU reads as the scheduling header. We own the instance. */
void flow_spurs_initialize_ctx(uint64_t* gpr)
{
    uint32_t spurs = (uint32_t)gpr[3];
    int32_t  nSpus = (int32_t)gpr[4];
    if (!spurs || !vm_base) { gpr[3] = (uint64_t)(int64_t)-1; return; }

    for (int i = 0; i < FLOW_SPURS_MAX_INST; i++)
        if (s_inst[i].ea == spurs) { s_inst[i].ea = 0; s_inst[i].next_wid = 0; }

    printf("[cellSpurs] Initialize(nSpus=%d) [flow]\n", nSpus);
    flow_spurs_build_instance(spurs, (uint32_t)nSpus);
    /* NOTE: the kernel is deliberately NOT started here. See flow_spurs_publish_wkl():
     * starting it before any workload exists makes its first scheduler pass fetch an
     * all-zero wklInfo, which its "already loaded" guard mistakes for a cache hit —
     * and it then never re-fetches. Start it once we actually have work. */
    gpr[3] = 0;   /* CELL_OK */
}

/* ---- cellSpursAddWorkload interception ----------------------------------- */

/* We do NOT delegate to the HLE cellSpursAddWorkload. Its bookkeeping lives in
 * the guest instance's first bytes (`initialized` @0x00, `nSpus` @0x04...), which
 * in the REAL layout are wklReadyCount1[] — the two representations occupy the
 * same memory and cannot coexist. (Calling the HLE after we lay down the real
 * block made it see initialized==0 and reject every AddWorkload.) The real layout
 * is what the SPU actually reads, so it wins; we keep the trivial wid bookkeeping
 * host-side instead (s_inst, declared above). */
static uint32_t flow_spurs_next_wid(uint32_t ea)
{
    for (int i = 0; i < FLOW_SPURS_MAX_INST; i++)
        if (s_inst[i].ea == ea) return s_inst[i].next_wid++;
    for (int i = 0; i < FLOW_SPURS_MAX_INST; i++)
        if (!s_inst[i].ea) { s_inst[i].ea = ea; s_inst[i].next_wid = 1; return 0; }
    return 0;
}

/* ppu_context is opaque here; we only need the GPRs. */
void flow_spurs_add_workload_ctx(uint64_t* gpr)
{
    uint32_t spurs  = (uint32_t)gpr[3];
    uint32_t wid_ea = (uint32_t)gpr[4];
    uint32_t pm     = (uint32_t)gpr[5];
    uint32_t sizePm = (uint32_t)gpr[6];
    uint64_t data   =            gpr[7];
    uint32_t minC   = (uint32_t)gpr[9];
    uint32_t maxC   = (uint32_t)gpr[10];   /* was being dropped — see publish_wkl */

    if (!spurs || !wid_ea || !vm_base) {
        gpr[3] = (uint64_t)(int64_t)-1;
        return;
    }

    uint32_t wid = flow_spurs_next_wid(spurs);
    if (wid >= SPURS_WKL_MAX) { gpr[3] = (uint64_t)(int64_t)-1; return; }

    be32w(vm_base + wid_ea, wid);          /* *wid out-param (guest BE) */
    gpr[3] = 0;                            /* CELL_OK                   */

    fprintf(stderr, "[cellSpurs] AddWorkload(wid=%u, pm=0x%08X, size=%u minC=%u maxC=%u) [flow]\n",
            wid, pm, sizePm, minC, maxC);

    /* Publish into the REAL instance layout so the kernel's scheduler finds a
     * runnable workload... */
    flow_spurs_publish_wkl(spurs, wid, pm, sizePm, data, minC, maxC);

    /* ...and only THEN start the kernel. Ordering is load-bearing: the kernel's
     * policy-load guard (sk_a func_6C0) skips the DMA when the wklInfo it fetched
     * equals LS[0x1D0] — and an ALL-ZERO wklInfo equals the zeroed LS[0x1D0], so a
     * kernel started before any workload exists latches "already loaded" on empty
     * and never dispatches. Start it once there is real work to find. */
    flow_spurs_kernel_start(spurs);

    flow_wl_dispatch(pm, sizePm, data, spurs);   /* bare path: off unless FLOW_SPU_BARE */
}

/* ---- registration -------------------------------------------------------- */

__attribute__((constructor))
static void flow_spu_workloads_install(void)
{
    /* Each policy gets its own image so spu_indirect_branch resolves within it.
     *
     * The SPURS KERNEL's lift must be registered under each policy image TOO: the
     * kernel occupies LS 0..0x834 and the policy sits at 0xA00, and the policy
     * CALLS BACK into kernel service routines (observed: "BRANCH-TO-0 unresolved
     * pc=0x00808 image=30" — LS 0x808 is kernel code, and after the MFC image
     * switch we're in image 30 where it isn't registered). Registering sk_a under
     * 30..33 as well makes those cross-calls resolve. Same trick flow_spurs_kernel.c
     * already uses ("the cri task is registered under both 22 and 23"). */
    spu_begin_image(30); wl0_spu_recomp_register(); sk_a_spu_recomp_register();
    spu_begin_image(31); wl1_spu_recomp_register(); sk_a_spu_recomp_register();
    spu_begin_image(32); wl2_spu_recomp_register(); sk_a_spu_recomp_register();
    spu_begin_image(33); wl3_spu_recomp_register(); sk_a_spu_recomp_register();
    spu_begin_image(0);

    /* No spu_workload_register_img() here: that registry is keyed by image
     * fingerprint, and the live image doesn't hash to the on-disk bytes (the
     * module is patched after load). We dispatch by pm address instead, so all
     * we need from the SPU runtime is the per-image branch tables above. */
    fprintf(stderr, "[FLOW-SPU] registered %d flOw SPU workload lifts (img 30-33)\n",
            FLOW_WL_COUNT);
}

/* One-shot getenv for the in-kernel 6C0 guard trace (patched into lift_a). */
int getenv_int_flowdma(void) { static int v=-1; if(v<0) v=getenv("FLOW_KRN_DMA")?1:0; return v; }

/* In-kernel guard trace (called from lift_a func_6C0 at the LS[0x3FFE0] read). */
void flow_6c0_guard_trace(const void* ls_v)
{
    if (!getenv("FLOW_KRN_DMA")) return;
    const unsigned char* ls = (const unsigned char*)ls_v;
    const unsigned char* p = ls + 0x3FFE0;   /* fetched wklInfo */
    const unsigned char* q = ls + 0x1D0;     /* compared-against ("already loaded") */
    fprintf(stderr, "[6C0-GUARD] wklInfo@LS0x3FFE0=%02X%02X%02X%02X %02X%02X%02X%02X  "
                    "cmp@LS0x1D0=%02X%02X%02X%02X %02X%02X%02X%02X\n",
            p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
            q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7]);
    fflush(stderr);
}

/* In-kernel trace of func_6C0's wklInfo GET params (EAH/EAL/size/cmd + instance). */
void flow_6c0_dma_trace(uint32_t eah, uint32_t eal, uint32_t sz, uint32_t cmd, uint32_t inst)
{
    if (!getenv("FLOW_KRN_DMA")) return;
    fprintf(stderr, "[6C0-DMA] GET eah=0x%08X eal=0x%08X size=%u cmd=0x%X inst(LS0x1C0)=0x%08X\n",
            eah, eal, sz, cmd, inst);
    fflush(stderr);
}
