/* ydkj_spurs_kernel.c — run the REAL lifted SPURS SPU kernel on the cellSpurs
 * SPU threads.
 *
 * libsre creates 5 CellSpursKernel SPU threads but hands them an empty
 * sys_spu_image (entry=0), so the runtime routes them to ydkj_hle_spurs_kernel
 * (lv2_register.c). That handler calls THIS runner (via g_ydkj_spurs_kernel_run)
 * which loads the SPURS kernel SPU ELF (embedded in libsre @ guest 0x30020380,
 * lifted as sk_a_) into a fresh local store and runs its entry (LS 0x818). The
 * kernel MFC-DMAs the policy module to LS 0xA00 and branches there; the policy
 * (lift_pol, LS 0xA00-0x47C0) and the cri task (LS 0x3000+) are registered under
 * the same SPU image (22), so the indirect branches resolve across all layers.
 */
#include "spu_context.h"
#include "spu_workload.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern int  spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void sk_a_spu_func_00000818(spu_context*);
extern int32_t (*g_ydkj_spurs_kernel_run)(uint32_t tid, uint32_t args_ea);
/* Early hook: called SYNCHRONOUSLY from sys_spu_thread_initialize (runtime) for
 * each cellSpurs SPU thread, BEFORE libsre's handler checks the SPU and rolls
 * the group back. We must spawn the kernel on a detached thread here (libsre
 * never calls group_start in our env), not block init. */
extern void (*g_spurs_kernel_hook)(uint32_t args_ea);

/* SPURS kernel SPU ELF: libsre.linked.bin file offset 0x020380, loaded at guest
 * base 0x30000000 -> guest EA 0x30020380. Image size 0x834 (entry LS 0x818). */
#define SPURS_KERNEL_ELF_EA  0x30020380u
#define SPURS_KERNEL_ELF_SZ  0x834u

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* --- PC sampler: find where the kernel/policy idle-loops (YDKJ_PCSAMPLE) --- */
static volatile spu_context* g_pcs_ctx[8] = {0};
static volatile uint32_t     g_pcs_tid[8] = {0};
static volatile long         g_pcs_started = 0;
#ifdef _WIN32
static DWORD WINAPI ydkj_pcsample_thread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(400);
        for (int i = 0; i < 8; i++) {
            spu_context* c = (spu_context*)g_pcs_ctx[i];
            if (c) fprintf(stderr, "[pcsample] tid=0x%X pc=0x%05X status=%d\n",
                           g_pcs_tid[i], c->pc & 0x3FFFF, c->status);
        }
        /* Also dump the live CellSpurs instance workload state: did the PPU ever
         * submit a workload (so the scheduler should dispatch it)? */
        if (vm_base) {
            uint8_t* in = vm_base + 0x10166400;
            fprintf(stderr, "[inststate] wklReadyCount[0..7]=%08X%08X wklEnabled=%08X wklInfo1[0]=%08X %08X %08X %08X\n",
                    be32(in+0x00), be32(in+0x04), be32(in+0xB0),
                    be32(in+0xB00), be32(in+0xB04), be32(in+0xB08), be32(in+0xB0C));
        }
        fflush(stderr);
    }
    return 0;
}
#endif
void ydkj_pcsample_register(uint32_t tid, spu_context* c)
{
#ifdef _WIN32
    if (!getenv("YDKJ_PCSAMPLE")) return;
    for (int i = 0; i < 8; i++) if (!g_pcs_ctx[i]) { g_pcs_tid[i] = tid; g_pcs_ctx[i] = c; break; }
    if (InterlockedExchange(&g_pcs_started, 1) == 0) {
        HANDLE h = CreateThread(NULL, 0, ydkj_pcsample_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
#else
    (void)tid; (void)c;
#endif
}

static int32_t ydkj_spurs_kernel_run(uint32_t tid, uint32_t args_ea)
{
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return -1;
    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(vm_base + SPURS_KERNEL_ELF_EA, SPURS_KERNEL_ELF_SZ, ls, &entry)) {
        fprintf(stderr, "[SPURS-KRN] tid=0x%X: failed to load kernel ELF @0x%08X\n",
                tid, SPURS_KERNEL_ELF_EA);
        free(ls);
        return -1;
    }
    fprintf(stderr, "[SPURS-KRN] tid=0x%X load OK entry=0x%X ctx=0x%08X -> running lifted kernel\n",
            tid, entry, args_ea);
    if (vm_base) {  /* instance state AT RUN TIME (group_start path = after init) */
        uint8_t* in = vm_base + 0x10166400;
        fprintf(stderr, "[SPURS-KRN] tid=0x%X instance@0x10166400:", tid);
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", be32(in + i*4));
        fprintf(stderr, "\n");
    }
    /* DIAG: dump the SPU thread args (what libsre set) to a dedicated file (stderr
     * races across the kernel/handler threads). 64 bytes around args_ea. */
    if (args_ea && vm_base) {
        const char* dp = getenv("YDKJ_KRN_ARGS");
        FILE* af = fopen(dp && *dp ? dp : "krn_args.txt", "w");
        if (af) {
            fprintf(af, "args_ea=0x%08X\n", args_ea);
            for (int i = 0; i < 16; i++)
                fprintf(af, "arg[%2d] @+0x%02X = 0x%08X\n", i, i*4, be32(vm_base + args_ea + i*4));
            fprintf(af, "kernelLS 0x1C0=%08X 0x1D0=%08X 0x870=%08X\n",
                    be32(ls+0x1C0), be32(ls+0x1D0), be32(ls+0x870));
            fclose(af);
        }
    }
    fflush(stderr);

    spu_context ctx;
    spu_context_init(&ctx, tid);
    ctx.image_id = 22;                       /* kernel+policy+task all under img 22 */
    ctx.spu_group_id = 0x1000;               /* libsre SPURS SPU thread group (INSTDUMP) */
    /* r80 = SpursKernelContext base in LS = 0x100 (RPCS3 cellSpurs.h: SpursKernelContext
     * @ LS 0x100). The policy entry reads LS[r80+0xC0] = LS[0x1C0] = the CellSpurs
     * instance pointer. The kernel never sets r80 (it's an initial SPU register libsre
     * seeds); without it the policy reads LS[0xC0] (garbage) and computes 0. */
    if (!getenv("YDKJ_NO_R80")) {
        ctx.gpr[80]._u32[0] = 0x100;   /* preferred slot = context base LS addr */
    }
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10; /* SPU stack top, 16-aligned          */
    memcpy(ctx.ls, ls, SPU_LS_SIZE);

    /* SPU thread args (4 x u64) -> r3..r6, preferred dword slot. Use the values
     * captured RACE-FREE in the early hook (args_ea is in the stack region and is
     * overwritten by the time this detached thread runs). */
    extern uint32_t g_krn_args[8];
    extern int      g_krn_args_valid;
    if (g_krn_args_valid) {
        for (int a = 0; a < 4; a++) {
            ctx.gpr[3 + a]._u32[0] = g_krn_args[a*2 + 0];   /* arg hi32 */
            ctx.gpr[3 + a]._u32[1] = g_krn_args[a*2 + 1];   /* arg lo32 */
        }
        fprintf(stderr, "[SPURS-KRN] tid=0x%X r3..r6 = %08X:%08X %08X:%08X %08X:%08X %08X:%08X\n", tid,
                g_krn_args[0],g_krn_args[1],g_krn_args[2],g_krn_args[3],
                g_krn_args[4],g_krn_args[5],g_krn_args[6],g_krn_args[7]);
        fflush(stderr);
    }
    /* EXPERIMENT (YDKJ_KRN_R16): the kernel's context-load DMA in func_000006C0
     * reads its EA from r16 (never set by the entry), so it needs the SPURS
     * instance EA there. libsre delivers it in r4 (arg1); mirror it into r16. */
    if (getenv("YDKJ_KRN_R16")) {
        ctx.gpr[16]._u32[0] = ctx.gpr[4]._u32[0];
        ctx.gpr[16]._u32[1] = ctx.gpr[4]._u32[1];
        fprintf(stderr, "[SPURS-KRN] tid=0x%X seeded r16 = r4 = %08X:%08X (instance EA)\n",
                tid, ctx.gpr[16]._u32[0], ctx.gpr[16]._u32[1]);
        fflush(stderr);
    }

    /* EXPERIMENT (YDKJ_KRN_PRELOAD): the recomp lifts the policy + task code
     * (dispatched by address), but their LS DATA isn't loaded because the kernel's
     * DMA doesn't land. Pre-load the policy module (libsre @0x30021600 -> LS 0xA00)
     * and the cri task ELF (-> LS 0x3000) so the lifted code reads valid data. */
    if (getenv("YDKJ_KRN_PRELOAD") && vm_base) {
        memcpy(ctx.ls + 0xA00, vm_base + 0x30021480, 0x2200);   /* sys-service policy module (real addr from wklInfoSysSrv) */
        uint32_t te = 0;
        spu_elf_load_to_ls(vm_base + 0x30020380, SPURS_KERNEL_ELF_SZ, ctx.ls, &te); /* re-affirm kernel */
        extern int spu_elf_load_to_ls(const uint8_t*, size_t, uint8_t*, uint32_t*);
        /* cri task ELF @ guest 0x4f5f80 -> its PH0 loads at LS 0x3000 */
        spu_elf_load_to_ls(vm_base + 0x4f5f80, 0x26cec, ctx.ls, &te);
        fprintf(stderr, "[SPURS-KRN] tid=0x%X PRELOAD: policy@0xA00=%08X task@0x3000=%08X\n",
                tid, be32(ctx.ls+0xA00), be32(ctx.ls+0x3000));
        fflush(stderr);
    }

    /* The 5 kernel host threads spawn in sys_spu_thread_group_start and may begin
     * BEFORE the PPU finishes populating the SPURS instance @0x10166400. The
     * kernel's bootstrap context DMA (func_000006C0, EA from r16) would then load
     * zeros and branch to garbage. Poll until the instance is non-zero (or time
     * out) so the kernel reads a live context. Env YDKJ_KRN_NOWAIT skips. */
    if (vm_base && !getenv("YDKJ_KRN_NOWAIT")) {
        uint8_t* inst = vm_base + 0x10166400;
        int waited = 0;
        while (waited < 4000 /*ms*/) {
            uint32_t any = 0;
            for (int i = 0; i < 16; i++) any |= be32(inst + i*4);
            if (any) break;
#ifdef _WIN32
            Sleep(5);
#endif
            waited += 5;
        }
        fprintf(stderr, "[SPURS-KRN] tid=0x%X instance-wait %dms -> @0x10166400 w0=%08X w4=%08X\n",
                tid, waited, be32(inst), be32(inst+4));
        fflush(stderr);
    }

    /* cri build (YDKJ_CRI_CHAIN): register a cri TASKSET workload in the CellSpurs
     * instance + build a CellSpursTaskset struct, so the (healthy) kernel scheduler
     * dispatches it: kernel loads the taskset policy (0x30023680) -> image-switch to
     * 23 (spu_dma.h) -> policy builds the 0x2700 task-API + dispatches the cri task.
     * Done once. Layouts from RPCS3 cellSpurs.h / cellSpursTaskset.h. */
    if (vm_base && getenv("YDKJ_CRI_CHAIN")) {
        static volatile long s_done = 0;
#ifdef _WIN32
        if (InterlockedExchange(&s_done, 1) == 0)
#else
        if (!s_done && (s_done = 1))
#endif
        {
            const uint32_t INST = 0x10166400u;     /* CellSpurs instance              */
            const uint32_t TSEA = 0x0F000000u;     /* CellSpursTaskset (committed main mem) */
            const uint32_t TPOL = 0x30023680u;     /* taskset policy module EA        */
            const uint32_t CRIELF = 0x004F5F80u;   /* cri task ELF EA                 */
            uint8_t* in = vm_base + INST;
            uint8_t* ts = vm_base + TSEA;
            #define BE32(p,v) do{uint8_t*_p=(p);uint32_t _v=(v);_p[0]=(uint8_t)(_v>>24);_p[1]=(uint8_t)(_v>>16);_p[2]=(uint8_t)(_v>>8);_p[3]=(uint8_t)_v;}while(0)
            /* --- CellSpursTaskset struct --- */
            memset(ts, 0, 0x1900);
            BE32(ts + 0x10, 0x80000000u);          /* ready: task0 (bit0 MSB)         */
            BE32(ts + 0x20, 0x80000000u);          /* pending_ready: task0            */
            BE32(ts + 0x30, 0x80000000u);          /* enabled: task0                  */
            /* NOTE: tried ready/enabled, +pending, -enabled, +context_save/ls_pattern
             * — the policy scheduler (func_00001054/10F8 clz on LS[0x2770]) still
             * loops, never picking task0. The exact SPURS task-state encoding the
             * scheduler needs isn't cracking via guessing; needs methodical RE of the
             * activation path (func_00000E60 r48!=0) or a real libsre-built taskset. */
            BE32(ts + 0x60, 0x00000000u); BE32(ts + 0x64, INST);   /* spurs back-ptr  */
            BE32(ts + 0x74, 0x00000000u);          /* wid 0                           */
            /* task_info[0] @0x80 (48B): args@0x00, elf@0x10, context_save+ls_blocks@0x18,
             * ls_pattern@0x20. The policy needs context_save + ls_pattern to ACTIVATE a
             * pending task into the runnable set (else the pending->ready transition
             * computes nothing schedulable). */
            BE32(ts + 0x80 + 0x10, 0x00000000u); BE32(ts + 0x80 + 0x14, CRIELF);   /* elf EA */
            /* context_save_storage_and_alloc_ls_blocks (u64): EA of a context-save
             * buffer in low 0x?? bits | nLsBlocks. Use a committed scratch EA + a
             * block count covering the cri task LS. */
            BE32(ts + 0x80 + 0x18, 0x00000000u); BE32(ts + 0x80 + 0x1C, 0x0F010000u | 0x7Fu);
            /* ls_pattern (128-bit): which 2KB LS blocks the task uses — set all so the
             * policy doesn't reject the task's LS footprint. */
            BE32(ts + 0x80 + 0x20, 0xFFFFFFFFu); BE32(ts + 0x80 + 0x24, 0xFFFFFFFFu);
            BE32(ts + 0x80 + 0x28, 0xFFFFFFFFu); BE32(ts + 0x80 + 0x2C, 0xFFFFFFFFu);
            BE32(ts + 0x1890, 0x00001900u);        /* size                            */
            /* --- instance: register taskset workload at wklInfo1[0] @0xB00 --- */
            uint8_t* wi = in + 0xB00;
            BE32(wi + 0x00, 0x00000000u); BE32(wi + 0x04, TPOL);   /* addr = taskset policy */
            BE32(wi + 0x08, 0x00000000u); BE32(wi + 0x0C, TSEA);   /* arg  = taskset EA     */
            BE32(wi + 0x10, 0xFF000000u);          /* size/priority-ish (cf wklInfoSysSrv) */
            in[0x00] = 1;                          /* wklReadyCount1[0] = 1 (wid0 ready)   */
            BE32(in + 0xB0, 0x0000FFFFu);          /* wklEnabled (wid0 in low bits)        */
            /* REAL workload registration (so the kernel picks wid0 + the policy activates
             * the task): wklState1[0]@0x80 = RUNNABLE(2); wklSignal1@0x70 (u16) bit for
             * wid0 = 0x8000 (MSB-first, per RPCS3 `sig |= 0x8000 >> (wid%16)`). This is
             * the notification cellSpursCreateTask/SendWorkloadSignal raises. */
            in[0x80] = 2;                          /* wklState1[0] = SPURS_WKL_STATE_RUNNABLE */
            in[0x70] = 0x80; in[0x71] = 0x00;      /* wklSignal1 = 0x8000 (wid0)           */
            /* HIJACK wklInfoSysSrv (0xD00): the kernel always dispatches this slot
             * (loads .addr -> LS 0xA00). Repoint it at the TASKSET policy + taskset
             * arg so the kernel loads 0x30023680 (-> image-switch 23) and runs the
             * taskset policy with the taskset EA as arg. Diagnostic (env YDKJ_CRI_HIJACK). */
            if (getenv("YDKJ_CRI_HIJACK")) {
                uint8_t* ws = in + 0xD00;
                BE32(ws + 0x00, 0x00000000u); BE32(ws + 0x04, TPOL);  /* addr = taskset policy */
                BE32(ws + 0x08, 0x00000000u); BE32(ws + 0x0C, TSEA);  /* arg  = taskset EA     */
                BE32(ws + 0x10, 0x00002200u);                         /* size                  */
                fprintf(stderr, "[cri-chain] HIJACK wklInfoSysSrv@0xD00 -> taskset policy 0x%08X arg=0x%08X\n", TPOL, TSEA);
            }
            #undef BE32
            fprintf(stderr, "[cri-chain] populated taskset workload@wklInfo1[0] (policy=0x%08X arg=0x%08X) + CellSpursTaskset@0x%08X (task0.elf=0x%08X)\n",
                    TPOL, TSEA, TSEA, CRIELF);
            fflush(stderr);
        }
    }

    /* PC sampler (YDKJ_PCSAMPLE): register this kernel ctx so a background thread
     * can sample ctx->pc and reveal the idle-loop function (the policy spins in a
     * pure-LS goto loop that touches no channel/atomic/branch hook). ctx is a live
     * local; valid for the whole stuck run. */
    extern void ydkj_pcsample_register(uint32_t tid, spu_context* c);
    ydkj_pcsample_register(tid, &ctx);

    /* SpursKernelContext.spurs @ LS[0x1C0] = the CellSpurs instance EA. The kernel
     * scheduler (sk_a_func_000006C0) reads r17=LS[0x1C0] and MFC-DMAs the
     * workload-ready state from there. r4 carries the instance EA; seed LS[0x1C0]
     * so the scheduler reads the real instance instead of EA 0. */
    {
        uint32_t inst = ctx.gpr[4]._u32[1] ? ctx.gpr[4]._u32[1] : 0x10166400u;
        uint8_t* p = ctx.ls + 0x1C0;
        p[0]=0;p[1]=0;p[2]=0;p[3]=0;
        p[4]=(uint8_t)(inst>>24);p[5]=(uint8_t)(inst>>16);p[6]=(uint8_t)(inst>>8);p[7]=(uint8_t)inst;
        /* broaden the diagnostic dump: instance header + workload area (0xB00) +
         * sys-service slot (0xD00), to see what cellSpursInitialize populated. */
        if (vm_base) {
            uint8_t* in = vm_base + inst;
            fprintf(stderr, "[SPURS-KRN] tid=0x%X LS[0x1C0]=inst 0x%08X; instance regions:\n", tid, inst);
            fprintf(stderr, "  +0x00: %08X %08X %08X %08X  +0x70:%08X +0x80:%08X\n",
                    be32(in),be32(in+4),be32(in+8),be32(in+0xC),be32(in+0x70),be32(in+0x80));
            fprintf(stderr, "  +0xB00(wkl1):%08X %08X %08X %08X  +0xD00(sysSrv):%08X %08X %08X %08X\n",
                    be32(in+0xB00),be32(in+0xB04),be32(in+0xB08),be32(in+0xB0C),
                    be32(in+0xD00),be32(in+0xD04),be32(in+0xD08),be32(in+0xD0C));
            fflush(stderr);
        }
    }

    spu_run_with_halt(sk_a_spu_func_00000818, &ctx);
    fprintf(stderr, "[SPURS-KRN] tid=0x%X kernel returned (status=%d)\n", tid, ctx.status);
    /* DIAG: did the kernel's DMA chain populate LS? (context@0x3FFE0, policy@0xA00,
     * task@0x3000) + is the SPURS instance @0x10166400 populated in main memory? */
    {
        uint8_t* L = ctx.ls;
        fprintf(stderr, "[SPURS-KRN] tid=0x%X post-run LS: 0x1C0=%08X 0x1D0=%08X 0x3FFE0=%08X%08X 0xA00=%08X 0x3000=%08X\n",
                tid, be32(L+0x1C0), be32(L+0x1D0), be32(L+0x3FFE0), be32(L+0x3FFE4), be32(L+0xA00), be32(L+0x3000));
        if (vm_base) {
            uint8_t* inst = vm_base + 0x10166400;
            fprintf(stderr, "[SPURS-KRN] tid=0x%X instance@0x10166400: %08X %08X %08X %08X %08X %08X %08X %08X\n", tid,
                    be32(inst+0),be32(inst+4),be32(inst+8),be32(inst+0xC),
                    be32(inst+0x10),be32(inst+0x14),be32(inst+0x18),be32(inst+0x1C));
        }
    }
    fflush(stderr);

    /* The real SPURS SPU kernel never returns -- it loops forever (poll ready
     * workload -> dispatch / idle). Our lifted kernel runs the scheduler once and
     * hits STOP. If we RETURN here, the SPU thread group "completes", and lv2 posts
     * a TERMINATION event to the SPURS handler's event queue (q=3). The debug
     * libspurs handler then asserts (handler.c:198 "unexpected value 0x0") and runs
     * a cellSpursTrace debug dump that reads a garbage workload field -> hang.
     * Keep the group alive by re-running the kernel in a loop (so it keeps polling
     * the instance and picks up workloads the PPU adds), which prevents the
     * premature termination event. Gated so behaviour is comparable. */
#ifdef _WIN32
    if (!getenv("YDKJ_KRN_NOLOOP")) {
        fprintf(stderr, "[SPURS-KRN] tid=0x%X entering persistent kernel loop (keeps SPU group alive)\n", tid);
        fflush(stderr);
        for (long i = 0; i < 5000000L; i++) {
            ctx.status = 0;
            spu_run_with_halt(sk_a_spu_func_00000818, &ctx);
            Sleep(8);   /* ~120 Hz poll; yields so the PPU/handler threads run */
        }
    }
#endif

    /* write the (possibly updated) LS back is not needed: the kernel operates via
     * MFC DMA to EA, which the runtime applies to guest memory directly. */
    free(ls);
    return 0;
}

/* Detached host thread: run the lifted kernel against the SPURS context. */
typedef struct { uint32_t tid; uint32_t args_ea; } krn_arg_t;
#ifdef _WIN32
static DWORD WINAPI ydkj_krn_thread(LPVOID p) {
    krn_arg_t* k = (krn_arg_t*)p;
    ydkj_spurs_kernel_run(k->tid, k->args_ea);
    free(k);
    return 0;
}
#endif

/* Race-free copy of the SPU thread args (4 u64 = 8 u32) captured synchronously
 * in the early hook, while args_ea's stack memory is still valid. */
uint32_t g_krn_args[8] = {0};
int      g_krn_args_valid = 0;
static uint32_t g_krn_args_ea = 0;   /* the SPU thread args EA (captured in hook) */

/* flОw: spawn the SPURS kernel AFTER cellSpursInitialize returns (so the CellSpurs
 * instance @0x10166400 is fully populated -- spawning during init crashed the
 * kernel on an empty bootstrap context). Called once from the init-return hook
 * (ppu_hle.cpp). The 5 SPU threads share one kernel; one host thread models it. */
void flow_spurs_kernel_spawn_postinit(void)
{
#ifdef _WIN32
    static volatile long s_done = 0;
    if (InterlockedExchange(&s_done, 1) != 0) return;
    if (!g_krn_args_valid) {
        fprintf(stderr, "[SPURS-KRN] post-init spawn: args not captured yet -- skip\n");
        return;
    }
    krn_arg_t* k = (krn_arg_t*)malloc(sizeof(*k));
    k->tid = 0x2000; k->args_ea = g_krn_args_ea;
    HANDLE h = CreateThread(NULL, 0, ydkj_krn_thread, k, 0, NULL);
    if (h) CloseHandle(h);
    fprintf(stderr, "[SPURS-KRN] post-init spawn: kernel thread (args_ea=0x%08X)\n", g_krn_args_ea);
    fflush(stderr);
#endif
}

/* Early hook (from sys_spu_thread_initialize). Spawn the kernel ONCE on a
 * detached thread so it runs concurrently with libsre's PPU init/handler. */
static void ydkj_spurs_kernel_hook(uint32_t args_ea)
{
    static volatile long s_spawned = 0;
#ifdef _WIN32
    /* Capture the args race-free (synchronous w.r.t. libsre setting them). */
    if (args_ea && vm_base && !g_krn_args_valid) {
        for (int i = 0; i < 8; i++) g_krn_args[i] = be32(vm_base + args_ea + i*4);
        g_krn_args_ea = args_ea;
        g_krn_args_valid = 1;
    }
    /* By DEFAULT do NOT spawn here: the early hook fires during cellSpursInitialize,
     * BEFORE libsre populates the SPURS instance @0x10166400 (verified all-zero at
     * this point), so the kernel's bootstrap DMA would load an empty context. With
     * YDKJ_KEEPGROUP, libsre now reaches group_start, which dispatches the kernel
     * LATER (instance populated) via the fallback path. Keep the early spawn only
     * under YDKJ_KRN_EARLYSPAWN for comparison. The arg capture above is the real
     * job of this hook (race-free). */
    if (InterlockedExchange(&s_spawned, 1) != 0) return;  /* once */
    if (getenv("YDKJ_KRN_EARLYSPAWN")) {
        krn_arg_t* k = (krn_arg_t*)malloc(sizeof(*k));
        k->tid = 0x2000; k->args_ea = args_ea;
        HANDLE h = CreateThread(NULL, 0, ydkj_krn_thread, k, 0, NULL);
        if (h) CloseHandle(h);
        fprintf(stderr, "[SPURS-KRN] early hook: spawned detached kernel thread ctx=0x%08X\n", args_ea);
        fflush(stderr);
    }
#else
    (void)args_ea; (void)s_spawned;
#endif
}

/* Crash tracer (YDKJ_CRASH_TRACE): on an access violation, log the faulting
 * address + host RIP (RVA from module base) + the most-recent SPU ctx->pc, so a
 * cri/policy segfault can be localized. Returns CONTINUE_SEARCH so the process
 * still dies (after logging). */
#ifdef _WIN32
static LONG WINAPI ydkj_crash_veh(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == 0xC000001Du /* illegal */) {
        char* base = (char*)GetModuleHandleA(NULL);
        uintptr_t _rip0 = (uintptr_t)ep->ContextRecord->Rip;
        fprintf(stderr, "[CRASH] code=0x%08lX rip_rva=0x%zX\n",
                (unsigned long)code, (size_t)(_rip0 - (uintptr_t)base));
        fflush(stderr);
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        char* base = (char*)GetModuleHandleA(NULL);
        uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
        uintptr_t tgt = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "[CRASH] AV at host_rip=0x%p rva=0x%zX faulting_addr=0x%p (vm_base=0x%p delta=0x%zX)\n",
                (void*)rip, (size_t)(rip - (uintptr_t)base), (void*)tgt,
                (void*)vm_base, (size_t)(tgt - (uintptr_t)vm_base));
        for (int i = 0; i < 8; i++) {
            spu_context* c = (spu_context*)g_pcs_ctx[i];
            if (c) fprintf(stderr, "[CRASH]   spu tid=0x%X pc=0x%05X\n", g_pcs_tid[i], c->pc & 0x3FFFF);
        }
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* SPU lift registration: the kernel runs sk_a_* directly, but its indirect
 * branches (e.g. 7D8 -> LS 0xA00 = the policy) resolve via spu_lookup(pc,image).
 * Nothing else registers the SPU-kernel lifts, so spu_lookup(0xA00,22) failed
 * (branch-to-0). Register kernel(sk_a) + policy(pol) under image 22 (they
 * branch into each other) and the taskset policy(tsp) under image 23.
 * (lift_b is not in the build; if sk_a branches into an sk_b address the
 * BRANCH-TO-0 trace will show it and we add lift_b then.) */
extern void spu_begin_image(int image_id);
extern void sk_a_spu_recomp_register(void);   /* SPURS kernel (entry 0x818) */
extern void pol_spu_recomp_register(void);    /* sys-service policy (LS 0xA00) */
extern void tsp_spu_recomp_register(void);    /* taskset policy */

__attribute__((constructor))
static void ydkj_spurs_kernel_install(void)
{
#ifdef _WIN32
    if (getenv("YDKJ_CRASH_TRACE")) AddVectoredExceptionHandler(1, ydkj_crash_veh);
#endif
    g_ydkj_spurs_kernel_run = ydkj_spurs_kernel_run;  /* group_start path (unused now) */
    g_spurs_kernel_hook     = ydkj_spurs_kernel_hook; /* early thread_init path        */

    /* Register the SPURS SPU kernel + policy so the kernel's indirect branch to
     * LS 0xA00 (and the policy's internal branches) resolve. */
    spu_begin_image(22);
    sk_a_spu_recomp_register();
    pol_spu_recomp_register();
    spu_begin_image(23);
    tsp_spu_recomp_register();
    spu_begin_image(0);
    fprintf(stderr, "[SPURS-KRN] registered SPU lifts: sk_a+pol(img22) tsp(img23)\n");
}
