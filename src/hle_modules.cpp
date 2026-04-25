/*
 * flOw HLE module registration — real ps3recomp bridges
 *
 * Bridges recompiled code's NID-dispatched calls (ppu_context*) to
 * ps3recomp's real HLE implementations (explicit C parameters).
 *
 * PS3 PPC64 ABI:
 *   r3-r10  = integer / pointer arguments
 *   f1-f13  = floating-point arguments
 *   r3      = integer return value
 *   f1      = floating-point return value
 *
 * Guest memory is big-endian. When HLE functions write structs to guest
 * memory, each field must be byte-swapped. For scalar returns, we just
 * store in ctx->gpr[3].
 */

#include "config.h"

#include <ps3emu/module.h>
#include <ps3emu/nid.h>
#include <ps3emu/error_codes.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

/* Runtime headers */
#include "runtime/ppu/ppu_context.h"
#include "runtime/memory/vm.h"

/* Access the syscall table for direct syscall dispatch */
extern "C" {
#include "runtime/syscalls/lv2_syscall_table.h"
}
extern "C" lv2_syscall_table g_lv2_syscalls;

/* ps3recomp HLE headers — only include headers for modules we actually bridge.
 * Headers using C11 _Atomic (cellSync.h) are incompatible with C++ compilation.
 * Stub-only modules (cellSpurs, cellSync, sceNp, sys_net) don't
 * need their headers. */
extern "C" {
#include "libs/system/cellSysutil.h"
#include "libs/system/cellSysmodule.h"
#include "libs/system/sysPrxForUser.h"
#include "libs/video/cellGcmSys.h"
#include "libs/video/cellVideoOut.h"
#include "libs/audio/cellAudio.h"
#include "libs/input/cellPad.h"
#include "libs/filesystem/cellFs.h"
#include "libs/network/cellNetCtl.h"
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Endian helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

extern "C" uint8_t* vm_base;

extern "C" void dispatch_register_external(uint32_t addr, void (*func)(void*));
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*);
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" int rsx_null_backend_pump_messages(void);

/* Snapshot refs for restoring zeroed ELF data (defined in vm_bridge.cpp) */
struct elf_snap_ref_ext { uint8_t* data; uint32_t start; uint32_t end; };
extern "C" elf_snap_ref_ext g_snap_refs[2];

extern "C" void vm_write8(uint64_t addr, uint8_t val);
extern "C" void vm_write16(uint64_t addr, uint16_t val);
extern "C" void vm_write32(uint64_t addr, uint32_t val);
extern "C" void vm_write64(uint64_t addr, uint64_t val);
extern "C" uint8_t  vm_read8(uint64_t addr);
extern "C" uint16_t vm_read16(uint64_t addr);
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" uint64_t vm_read64(uint64_t addr);

/* Translate guest address to host pointer */
static inline void* guest_ptr(uint64_t addr) {
    return vm_base + (uint32_t)addr;
}

/* Read a guest C string (ASCII/UTF-8 is byte-order neutral) */
static inline const char* guest_str(uint64_t addr) {
    if ((uint32_t)addr == 0) return nullptr;
    return (const char*)(vm_base + (uint32_t)addr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper: register function NID from name
 * ═══════════════════════════════════════════════════════════════════════════ */

static void reg_func(ps3_module* m, const char* name, void* handler)
{
    uint32_t nid = ps3_compute_nid(name);
    ps3_nid_table_add(&m->func_table, nid, name, handler);
}

/* Stub handler for not-yet-bridged functions — returns CELL_OK (0). */
static int64_t hle_stub(ppu_context* ctx) {
    ctx->gpr[3] = 0;  /* CELL_OK in guest r3 */
    return 0;
}

/* Verbose stub that logs the call (used for functions we want to track) */
static int64_t hle_stub_verbose(ppu_context* ctx) {
    ctx->gpr[3] = 0;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * sysPrxForUser — core PRX runtime (REAL bridges)
 *
 * Threads, mutexes, TLS, process management. These are critical for CRT
 * startup and must be wired to real implementations.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* sys_initialize_tls(tls_addr, tls_filesize, tls_memsize, tls_align) */
static int64_t bridge_sys_initialize_tls(ppu_context* ctx)
{
    uint32_t tls_addr  = (uint32_t)ctx->gpr[3];
    uint32_t tls_filesz = (uint32_t)ctx->gpr[4];
    uint32_t tls_memsz  = (uint32_t)ctx->gpr[5];
    uint32_t tls_align  = (uint32_t)ctx->gpr[6];

    fprintf(stderr, "[HLE] sys_initialize_tls(addr=0x%x, filesz=0x%x, memsz=0x%x, align=%u)\n",
            tls_addr, tls_filesz, tls_memsz, tls_align);

    /* Allocate TLS block in guest memory */
    static uint32_t tls_alloc_ptr = 0x0F000000;
    uint32_t tls_base = tls_alloc_ptr;
    if (tls_align > 0)
        tls_base = (tls_base + tls_align - 1) & ~(tls_align - 1);

    /* Copy TLS template data */
    if (tls_filesz > 0 && tls_addr != 0)
        memcpy(vm_base + tls_base, vm_base + tls_addr, tls_filesz);

    /* Zero BSS portion */
    if (tls_memsz > tls_filesz)
        memset(vm_base + tls_base + tls_filesz, 0, tls_memsz - tls_filesz);

    tls_alloc_ptr = tls_base + tls_memsz + 0x1000;

    /* Set r13 to TLS base + 0x7000 (PPC64 TLS ABI convention) */
    ctx->gpr[13] = tls_base + 0x7000;

    fprintf(stderr, "[HLE] TLS block at 0x%x, r13 = 0x%llx\n",
            tls_base, (unsigned long long)ctx->gpr[13]);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_process_exit(status) */
/* sys_process_exit redirect: set flag for main() to redirect to game main */
#include <setjmp.h>
extern "C" jmp_buf g_abort_jmp;
extern "C" int g_abort_redirect;
extern "C" const char* failbit_resolve_rip(void* rip, uint32_t* out_guest);
/* Second longjmp slot: when the game main reaches an assertion, jump out
 * of game main entirely and hand control back to main.cpp, which then
 * invokes the game-loop injection (func_000CBF4C → func_000C858C)
 * directly. Set by main.cpp via setjmp before calling func_000CB9CC. */
extern "C" jmp_buf g_loop_jmp;
extern "C" int g_loop_jmp_set;

static int64_t bridge_sys_process_exit(ppu_context* ctx)
{
    int32_t status = (int32_t)ctx->gpr[3];
    fprintf(stderr, "[HLE] sys_process_exit(%d) LR=0x%llX SP=0x%llX r3=0x%llX r4=0x%llX r5=0x%llX\n",
            status, (unsigned long long)ctx->lr, (unsigned long long)ctx->gpr[1],
            (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
            (unsigned long long)ctx->gpr[5]);
    /* Dump stack frame for debugging */
    uint32_t sp = (uint32_t)ctx->gpr[1];
    fprintf(stderr, "[HLE]   Stack dump at SP=0x%08X:\n", sp);
    for (int i = 0; i < 16; i++) {
        uint32_t val = vm_read32(sp + i * 4);
        fprintf(stderr, "    SP+0x%02X: 0x%08X\n", i * 4, val);
    }
    /* Host backtrace — same trick we used at the failbit-throw site.
     * Resolve each frame to a guest function so we can walk back to the
     * actual abort source. Limit to the first few exit calls so the log
     * doesn't blow up on retry loops. */
    {
        static int s_bt = 0;
        if (s_bt < 4) {
            s_bt++;
            void* frames[24];
            USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
            uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            fprintf(stderr, "[ABORT-BT] HOST stack (%u frames):\n", n);
            for (USHORT i = 0; i < n && i < 18; i++) {
                uintptr_t rip = (uintptr_t)frames[i];
                uint32_t guest = 0;
                const char* nm = failbit_resolve_rip(frames[i], &guest);
                fprintf(stderr, "[ABORT-BT]   #%2u exe+0x%llX  guest=%s (0x%08X)\n",
                        (unsigned)i, (unsigned long long)(rip - base),
                        nm ? nm : "?", guest);
            }
        }
    }
    fflush(stderr);

    /* During CRT or game main, intercept exit and redirect */
    if (g_abort_redirect == 0) {
        /* CRT phase: redirect to game main */
        g_abort_redirect = 1;
        fprintf(stderr, "[HLE] CRT abort intercepted — longjmp to main\n");
        fflush(stderr);
        longjmp(g_abort_jmp, 1);
    }

    /* Game main phase: the game's assertion handler calls exit(1) on an
     * init failure (SPU thread group join throw). Returning 0 just puts
     * the game back in the same retry loop. Instead, longjmp out of game
     * main entirely — main.cpp set g_loop_jmp before calling func_000CB9CC
     * and will invoke the game-loop injection (func_000CBF4C) on the
     * catch side, bypassing the broken init. */
    if (g_abort_redirect >= 2) {
        if (g_loop_jmp_set && status != 0) {
            fprintf(stderr, "[HLE] Game main assertion — longjmp to game loop injection\n");
            fflush(stderr);
            g_loop_jmp_set = 0;
            longjmp(g_loop_jmp, 1);
        }
        static int s_exit_count = 0;
        s_exit_count++;
        if (s_exit_count <= 200 && status != 0) {
            if (s_exit_count <= 5 || (s_exit_count % 20) == 0) {
                fprintf(stderr, "[HLE] Game assertion exit(%d) #%d — continuing past assertion\n",
                        status, s_exit_count);
                fflush(stderr);
            }
            ctx->gpr[3] = 0;
            return 0;
        }
        fprintf(stderr, "[HLE] Too many assertion exits (%d), stopping\n", s_exit_count);
        fflush(stderr);
    }

    exit(status);
    return 0;
}

/* sys_time_get_system_time() → r3 = microseconds */
static int64_t bridge_sys_time_get_system_time(ppu_context* ctx)
{
    static uint64_t fake_time = 1000000;
    static int call_count = 0;
    fake_time += 16667; /* ~60fps frame time */
    call_count++;
    if (call_count <= 10 || call_count % 100 == 0)
        fprintf(stderr, "[HLE] sys_time_get_system_time (call #%d)\n", call_count);
    ctx->gpr[3] = fake_time;
    return 0;
}

/* sys_ppu_thread_get_id(sys_ppu_thread_t* thread_id) */
extern "C" jmp_buf g_abort_jmp;
extern "C" int g_abort_redirect;
static int64_t bridge_sys_ppu_thread_get_id(ppu_context* ctx)
{
    static int s_call_count = 0;
    s_call_count++;

    uint32_t ptr = (uint32_t)ctx->gpr[3];
    if (ptr && vm_base)
        vm_write64(ptr, 0x10000); /* fake thread ID matching PS3 main thread */
    ctx->gpr[3] = 0;

    /* Force-fix LR if it's been corrupted to a stack address.
     * The CRT's trampoline chain can corrupt LR between fragments. */
    if ((ctx->lr >> 28) == 0xD) { /* stack region 0xD0000000+ */
        ctx->lr = 0x008175FC; /* restore to import stub return */
    }

    /* Log first few calls, every 200th, and spin detection */
    if (s_call_count <= 5 || s_call_count % 200 == 0) {
        fprintf(stderr, "[HLE] sys_ppu_thread_get_id(ptr=0x%X) call #%d SP=0x%08X LR=0x%08X CIA=0x%08X CTR=0x%08X\n",
                ptr, s_call_count, (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr,
                (uint32_t)ctx->cia, (uint32_t)ctx->ctr);
        fflush(stderr);
    }

    /* Detect true infinite spin: same SP+LR for consecutive calls.
     * The CRT spins waiting for a system thread variable that never gets set. */
    {
        static uint32_t s_last_sp = 0, s_last_lr = 0;
        static int s_same_count = 0;
        uint32_t sp = (uint32_t)ctx->gpr[1];
        uint32_t lr = (uint32_t)ctx->lr;
        if (sp == s_last_sp && lr == s_last_lr) {
            s_same_count++;
            if (s_same_count > 2000 && g_abort_redirect == 0) {
                fprintf(stderr, "[HLE] thread_get_id spin detected (%d identical calls SP=0x%08X LR=0x%08X)\n",
                        s_same_count, sp, lr);
                fflush(stderr);
                g_abort_redirect = 1;
                longjmp(g_abort_jmp, 1);
            }
        } else {
            s_last_sp = sp;
            s_last_lr = lr;
            s_same_count = 0;
        }
    }

    return 0;
}

/* sys_lwmutex_create(lwmutex_ptr, attr_ptr) — REAL */
static int64_t bridge_sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t mutex_addr = (uint32_t)ctx->gpr[3];
    uint32_t attr_addr  = (uint32_t)ctx->gpr[4];

    fprintf(stderr, "[HLE] sys_lwmutex_create(mutex=0x%x, attr=0x%x)\n",
            mutex_addr, attr_addr);

    if (!mutex_addr || mutex_addr > 0x20000000) {
        fprintf(stderr, "[HLE]   ERROR: invalid mutex address 0x%08X\n", mutex_addr);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; /* CELL_EFAULT */
        return 0;
    }

    /* Read attribute from guest memory, or use defaults if attr is NULL */
    sys_lwmutex_attribute_t attr;
    if (attr_addr && attr_addr < 0x20000000) {
        attr.protocol  = vm_read32(attr_addr);
        attr.recursive = vm_read32(attr_addr + 4);
        memcpy(attr.name, guest_ptr(attr_addr + 8), 8);
    } else {
        /* Default attributes: priority protocol, recursive */
        attr.protocol  = 2; /* SYS_SYNC_PRIORITY */
        attr.recursive = 0x10; /* SYS_SYNC_RECURSIVE */
        memset(attr.name, 0, 8);
        fprintf(stderr, "[HLE]   Using default attributes (attr ptr=0x%X)\n", attr_addr);
    }

    fprintf(stderr, "[HLE]   proto=%u, recur=%u, name=%.8s\n",
            attr.protocol, attr.recursive, attr.name);

    /* Create on a host temp, then store ID back */
    sys_lwmutex_t_hle mutex_hle;
    memset(&mutex_hle, 0, sizeof(mutex_hle));
    s32 rc = sys_lwmutex_create(&mutex_hle, &attr);

    fprintf(stderr, "[HLE]   rc=0x%x, lock_var=0x%llx\n", rc,
            (unsigned long long)mutex_hle.lock_var);

    /* Write the lwmutex struct back to guest in big-endian */
    vm_write64(mutex_addr,      mutex_hle.lock_var);
    vm_write32(mutex_addr + 8,  mutex_hle.attribute);
    vm_write32(mutex_addr + 12, mutex_hle.recursive_count);
    vm_write32(mutex_addr + 16, mutex_hle.sleep_queue);
    vm_write32(mutex_addr + 20, mutex_hle.pad);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* sys_lwmutex_lock(lwmutex_ptr, timeout) — REAL */
static int64_t bridge_sys_lwmutex_lock(ppu_context* ctx)
{
    static int s_lock_count = 0;
    static uint32_t s_last_mutex = 0;
    static int s_repeat = 0;
    s_lock_count++;

    uint32_t mutex_addr = (uint32_t)ctx->gpr[3];
    uint64_t timeout    = ctx->gpr[4];

    /* Spin detection: same mutex locked repeatedly without other HLE calls */
    if (mutex_addr == s_last_mutex) {
        s_repeat++;
    } else {
        s_repeat = 0;
        s_last_mutex = mutex_addr;
    }

    /* Force-succeed locks on corrupted mutex addresses.
     * The CRT's FILE* structures have 0x74 fill patterns. Printf tries to
     * lock the stdout mutex at 0x74747484 (corrupted pointer). Without this
     * fix, the lock fails and printf loops forever. */
    if (mutex_addr > 0x20000000 && mutex_addr < 0xD0000000) {
        /* Address is in unmapped/garbage region — force succeed */
        ctx->gpr[3] = 0;
        return 0;
    }

    /* Read the lock_var to identify which host mutex to use */
    sys_lwmutex_t_hle mutex_hle;
    mutex_hle.lock_var       = vm_read64(mutex_addr);
    mutex_hle.attribute      = vm_read32(mutex_addr + 8);
    mutex_hle.recursive_count = vm_read32(mutex_addr + 12);
    mutex_hle.sleep_queue    = vm_read32(mutex_addr + 16);
    mutex_hle.pad            = vm_read32(mutex_addr + 20);

    s32 rc = sys_lwmutex_lock(&mutex_hle, timeout);

    /* Write back updated state.
     * Set owner thread ID (1) in upper 32 bits of lock_var so the
     * Dinkumware CRT's __Mtx_lock sees the correct owner and doesn't spin. */
    if (rc == 0) {
        mutex_hle.lock_var = ((uint64_t)0x10000 << 32) | (mutex_hle.lock_var & 0xFFFFFFFF);
    }
    vm_write64(mutex_addr,      mutex_hle.lock_var);
    vm_write32(mutex_addr + 12, mutex_hle.recursive_count);

    /* Debug: log first 10 and every 200th, plus spin detection */
    if (s_lock_count <= 10 || s_lock_count % 200 == 0 || s_repeat == 50) {
        fprintf(stderr, "[HLE] lwmutex_lock(0x%08X) #%d rc=%d lock_var=0x%llX recur=%u SP=0x%08X LR=0x%08X repeat=%d\n",
                mutex_addr, s_lock_count, rc,
                (unsigned long long)mutex_hle.lock_var,
                mutex_hle.recursive_count,
                (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr, s_repeat);
        fflush(stderr);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* sys_lwmutex_trylock(lwmutex_ptr) — REAL */
static int64_t bridge_sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t mutex_addr = (uint32_t)ctx->gpr[3];

    sys_lwmutex_t_hle mutex_hle;
    mutex_hle.lock_var       = vm_read64(mutex_addr);
    mutex_hle.attribute      = vm_read32(mutex_addr + 8);
    mutex_hle.recursive_count = vm_read32(mutex_addr + 12);
    mutex_hle.sleep_queue    = vm_read32(mutex_addr + 16);
    mutex_hle.pad            = vm_read32(mutex_addr + 20);

    s32 rc = sys_lwmutex_trylock(&mutex_hle);

    vm_write64(mutex_addr,      mutex_hle.lock_var);
    vm_write32(mutex_addr + 12, mutex_hle.recursive_count);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* sys_lwmutex_unlock(lwmutex_ptr) — REAL */
static int64_t bridge_sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t mutex_addr = (uint32_t)ctx->gpr[3];

    sys_lwmutex_t_hle mutex_hle;
    mutex_hle.lock_var       = vm_read64(mutex_addr);
    mutex_hle.attribute      = vm_read32(mutex_addr + 8);
    mutex_hle.recursive_count = vm_read32(mutex_addr + 12);
    mutex_hle.sleep_queue    = vm_read32(mutex_addr + 16);
    mutex_hle.pad            = vm_read32(mutex_addr + 20);

    s32 rc = sys_lwmutex_unlock(&mutex_hle);

    /* Clear owner on unlock (set upper 32 bits to 0xFFFFFFFF = no owner) */
    if (rc == 0 && mutex_hle.recursive_count == 0) {
        mutex_hle.lock_var = (0xFFFFFFFFULL << 32) | (mutex_hle.lock_var & 0xFFFFFFFF);
    }
    vm_write64(mutex_addr,      mutex_hle.lock_var);
    vm_write32(mutex_addr + 12, mutex_hle.recursive_count);

    /* Debug: log state after unlock */
    static int s_unlock_count = 0;
    s_unlock_count++;
    if (s_unlock_count <= 10 || s_unlock_count % 200 == 0) {
        fprintf(stderr, "[HLE] lwmutex_unlock(0x%08X) #%d rc=%d lock_var=0x%llX recur=%u\n",
                mutex_addr, s_unlock_count, (int)rc,
                (unsigned long long)mutex_hle.lock_var,
                mutex_hle.recursive_count);
        fflush(stderr);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* sys_lwmutex_destroy(lwmutex_ptr) — REAL */
static int64_t bridge_sys_lwmutex_destroy(ppu_context* ctx)
{
    uint32_t mutex_addr = (uint32_t)ctx->gpr[3];

    sys_lwmutex_t_hle mutex_hle;
    mutex_hle.lock_var       = vm_read64(mutex_addr);
    mutex_hle.attribute      = vm_read32(mutex_addr + 8);
    mutex_hle.recursive_count = vm_read32(mutex_addr + 12);
    mutex_hle.sleep_queue    = vm_read32(mutex_addr + 16);
    mutex_hle.pad            = vm_read32(mutex_addr + 20);

    s32 rc = sys_lwmutex_destroy(&mutex_hle);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* sys_ppu_thread_create — delegate to runtime's real implementation.
 * The runtime (sys_ppu_thread.c) uses ppu_context* calling convention. */
extern "C" int64_t sys_ppu_thread_create_syscall(ppu_context* ctx);
extern "C" int64_t sys_ppu_thread_exit_syscall(ppu_context* ctx);

static int64_t bridge_sys_ppu_thread_create(ppu_context* ctx)
{
    fprintf(stderr, "[HLE] sys_ppu_thread_create(entry=0x%llx, arg=0x%llx, prio=%d, stack=%u)\n",
            (unsigned long long)ctx->gpr[4], (unsigned long long)ctx->gpr[5],
            (int32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7]);

    /* Call the runtime's real thread creation (uses ppu_context* ABI).
     * This is registered in lv2_syscall_table as syscall 41. */
    extern lv2_syscall_table g_lv2_syscalls;
    if (g_lv2_syscalls.handlers[41]) {
        int64_t rc = g_lv2_syscalls.handlers[41](ctx);
        return rc;
    }
    /* Fallback: fake thread ID */
    static uint64_t next_tid = 2;
    uint32_t tid_addr = (uint32_t)ctx->gpr[3];
    if (tid_addr)
        vm_write64(tid_addr, next_tid++);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_ppu_thread_exit — delegate to runtime */
static int64_t bridge_sys_ppu_thread_exit(ppu_context* ctx)
{
    fprintf(stderr, "[HLE] sys_ppu_thread_exit(0x%llx)\n",
            (unsigned long long)ctx->gpr[3]);
    extern lv2_syscall_table g_lv2_syscalls;
    if (g_lv2_syscalls.handlers[42]) {
        return g_lv2_syscalls.handlers[42](ctx);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellSysutil — system callbacks and parameters (REAL bridges)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Registered sysutil callback info */
static uint32_t g_sysutil_callback_func = 0;
static uint32_t g_sysutil_callback_userdata = 0;

/* cellSysutilRegisterCallback(slot, func, userdata) */
static int64_t bridge_cellSysutilRegisterCallback(ppu_context* ctx)
{
    s32 slot         = (s32)ctx->gpr[3];
    /* func and userdata are guest addresses; store as-is for callback dispatch */
    uint32_t func    = (uint32_t)ctx->gpr[4];
    uint32_t userdata = (uint32_t)ctx->gpr[5];

    fprintf(stderr, "[HLE] cellSysutilRegisterCallback(slot=%d, func=0x%x, TOC=0x%llX)\n",
            slot, func, (unsigned long long)ctx->gpr[2]);

    /* Save callback info for dispatch */
    if (slot == 0) {
        g_sysutil_callback_func = func;
        g_sysutil_callback_userdata = userdata;
    }

    s32 rc = cellSysutilRegisterCallback(slot, (CellSysutilCallback)(uintptr_t)func,
                                          (void*)(uintptr_t)userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellSysutilUnregisterCallback(slot) */
static int64_t bridge_cellSysutilUnregisterCallback(ppu_context* ctx)
{
    s32 rc = cellSysutilUnregisterCallback((s32)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellSysutilCheckCallback() */
static int64_t bridge_cellSysutilCheckCallback(ppu_context* ctx)
{
    /* Dispatch the game's registered sysutil callback on first call.
     * The game loops on cellSysutilCheckCallback waiting for system events.
     * We fire the callback once to simulate normal system startup. */
    static int s_check_count = 0;
    s_check_count++;
    /* The game polls this in a tight loop. We need to pump the RSX null
     * backend's Win32 message loop to keep the window responsive, and
     * also add a small sleep to prevent burning CPU. */
    {
        rsx_null_backend_pump_messages();
        if (s_check_count > 5) {
#ifdef _WIN32
            Sleep(1); /* prevent 100% CPU usage in the polling loop */
#endif
        }
    }

    /* Restore ELF snapshots on every CheckCallback call.
     * The game's init code continuously zeroes GOT entries via fast inline
     * vm_write8/vm_write32 (which bypass our hooks). Restoring the snapshot
     * here ensures GOT/OPD data is correct for subsequent code. */
    {
        /* g_snap_refs declared at file scope */
        for (int _i = 0; _i < 2; _i++) {
            if (g_snap_refs[_i].data)
                memcpy(vm_base + g_snap_refs[_i].start, g_snap_refs[_i].data,
                       g_snap_refs[_i].end - g_snap_refs[_i].start);
        }
    }
    /* Fix: restore the PhyreEngine vtable if it was zeroed by malloc memset.
     * The engine at 0xA000C0 should have vtable 0x1006E508 at offset 0
     * and sub-object vtable 0x1006E548 at offset 0x14. These get zeroed by
     * hle_guest_malloc's HOST memset during the init chain's allocations. */
    {
        uint32_t vt = vm_read32(0xA000C0);
        if (vt == 0) {
            uint32_t main_vt = vm_read32(0x891300); /* TOC-0x56A8 */
            uint32_t sub_vt  = vm_read32(0x891318); /* TOC-0x5690 */
            if (main_vt != 0) {
                vm_write32(0xA000C0, main_vt);
                vm_write32(0xA000D4, sub_vt);
                /* Also set the engine "initialized" flag at +0x5.
                 * The engine run function checks this: if 0, skip game loop. */
                vm_write8(0xA000C5, 1);
                fprintf(stderr, "[VTABLE-FIX] Restored engine vtables + init flag\n");
                fflush(stderr);
            }
        }
    }
    s32 rc = cellSysutilCheckCallback();
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellSysutilGetSystemParamInt(id, value_ptr) */
static int64_t bridge_cellSysutilGetSystemParamInt(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    uint32_t value_addr = (uint32_t)ctx->gpr[4];

    s32 host_value = 0;
    s32 rc = cellSysutilGetSystemParamInt(id, &host_value);

    if (rc == CELL_OK && value_addr)
        vm_write32(value_addr, (uint32_t)host_value);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellSysutilGetSystemParamString(id, buf_ptr, bufsize) */
static int64_t bridge_cellSysutilGetSystemParamString(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    uint32_t buf_addr = (uint32_t)ctx->gpr[4];
    uint32_t bufsize  = (uint32_t)ctx->gpr[5];

    /* Strings are byte-order neutral; write directly to guest memory */
    char* buf = (char*)guest_ptr(buf_addr);
    s32 rc = cellSysutilGetSystemParamString(id, buf, bufsize);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellVideoOut — video output config (REAL bridges)
 * These are part of cellSysutil library on PS3 but implemented in
 * ps3recomp's cellVideoOut module.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* cellVideoOutGetState(videoOut, deviceIndex, state_ptr) */
static int64_t bridge_cellVideoOutGetState(ppu_context* ctx)
{
    uint32_t videoOut    = (uint32_t)ctx->gpr[3];
    uint32_t deviceIndex = (uint32_t)ctx->gpr[4];
    uint32_t state_addr  = (uint32_t)ctx->gpr[5];

    CellVideoOutState host_state;
    s32 rc = cellVideoOutGetState(videoOut, deviceIndex, &host_state);

    fprintf(stderr, "[HLE] cellVideoOutGetState(out=%u, dev=%u) rc=0x%X state=%u colorSpace=%u displayMode=0x%X\n",
            videoOut, deviceIndex, rc, host_state.state, host_state.colorSpace, host_state.displayMode);

    if (rc == CELL_OK && state_addr) {
        /* Write struct to guest in big-endian */
        vm_write8(state_addr,     host_state.state);
        vm_write8(state_addr + 1, host_state.colorSpace);
        /* reserved[6] at offset 2 */
        memset((uint8_t*)guest_ptr(state_addr) + 2, 0, 6);
        vm_write32(state_addr + 8, host_state.displayMode);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellHddGameCheck(version, dirName, errDialog, funcStat, container)
 * On PS3, this checks HDD game data and calls funcStat callback.
 * The callback sets cbResult->result = CELL_HDDGAME_CBRESULT_OK.
 * We call the callback immediately with success status. */
extern "C" void ps3_indirect_call(ppu_context* ctx);
static int64_t bridge_cellHddGameCheck(ppu_context* ctx)
{
    uint32_t version  = (uint32_t)ctx->gpr[3];
    uint32_t dirName  = (uint32_t)ctx->gpr[4]; /* guest string ptr */
    uint32_t errDlg   = (uint32_t)ctx->gpr[5];
    uint32_t funcStat = (uint32_t)ctx->gpr[6]; /* guest OPD for callback */
    uint32_t container = (uint32_t)ctx->gpr[7];

    fprintf(stderr, "[HLE] cellHddGameCheck(ver=%u, dir=0x%08X, err=%u, func=0x%08X, cont=0x%08X)\n",
            version, dirName, errDlg, funcStat, container);
    fflush(stderr);

    if (funcStat != 0) {
        /* Allocate temporary CellHddGameCBResult on guest stack */
        uint32_t sp = (uint32_t)ctx->gpr[1];
        uint32_t cbResult_addr = sp - 0x100;  /* below current stack frame */
        uint32_t statGet_addr  = sp - 0x200;
        uint32_t statSet_addr  = sp - 0x300;

        /* Zero the structs */
        memset(guest_ptr(cbResult_addr), 0, 0x100);
        memset(guest_ptr(statGet_addr), 0, 0x100);
        memset(guest_ptr(statSet_addr), 0, 0x100);

        /* CellHddGameCBResult: result at offset 0, errNeedSizeKB at 4, etc.
         * Set result = CELL_HDDGAME_CBRESULT_OK (0) — already 0 from memset
         * Set isNewData = 1 (new game, no existing data) */
        vm_write32(statGet_addr + 0, 1);  /* isNewData = 1 */

        /* Write game dir path into statGet hddDir field (offset 0x18 typically) */
        const char* game_path = "/dev_hdd0/game/NPUA80001";
        for (int i = 0; game_path[i] && i < 127; i++)
            vm_write8(statGet_addr + 0x18 + i, game_path[i]);

        /* Call funcStat(cbResult, statGet, statSet) through dispatch */
        fprintf(stderr, "[HLE] Calling HddGameCheck callback at OPD 0x%08X\n", funcStat);
        fflush(stderr);

        /* Save context and call the callback */
        ppu_context saved = *ctx;
        ctx->gpr[3] = cbResult_addr;
        ctx->gpr[4] = statGet_addr;
        ctx->gpr[5] = statSet_addr;

        /* Read function entry from OPD */
        uint32_t func_entry = vm_read32(funcStat);
        uint32_t func_toc   = vm_read32(funcStat + 4);
        if (func_entry != 0) {
            ctx->ctr = func_entry;
            ctx->gpr[2] = func_toc ? func_toc : 0x008969A8;
            ps3_indirect_call(ctx);
            /* Drain trampolines */
            while (g_trampoline_fn) {
                void(*tf)(void*) = g_trampoline_fn;
                g_trampoline_fn = nullptr;
                tf((void*)ctx);
            }
        }

        /* Check cbResult->result */
        uint32_t result = vm_read32(cbResult_addr);
        fprintf(stderr, "[HLE] HddGameCheck callback returned, cbResult=0x%08X\n", result);
        fflush(stderr);

        /* Restore context (keep r3 for return) */
        uint64_t cb_r3 = ctx->gpr[3];
        *ctx = saved;
    }

    ctx->gpr[3] = 0; /* CELL_OK */
    return 0;
}

/* cellVideoOutGetResolution(resolutionId, resolution_ptr) */
static int64_t bridge_cellVideoOutGetResolution(ppu_context* ctx)
{
    uint32_t resId    = (uint32_t)ctx->gpr[3];
    uint32_t res_addr = (uint32_t)ctx->gpr[4];

    CellVideoOutResolution host_res;
    s32 rc = cellVideoOutGetResolution(resId, &host_res);

    if (rc == CELL_OK && res_addr) {
        vm_write16(res_addr,     host_res.width);
        vm_write16(res_addr + 2, host_res.height);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellVideoOutConfigure(videoOut, config_ptr, option, waitForEvent) */
static int64_t bridge_cellVideoOutConfigure(ppu_context* ctx)
{
    uint32_t videoOut   = (uint32_t)ctx->gpr[3];
    uint32_t config_addr = (uint32_t)ctx->gpr[4];

    /* Read config from guest */
    CellVideoOutConfiguration host_config;
    memset(&host_config, 0, sizeof(host_config));
    if (config_addr) {
        host_config.resolutionId = vm_read8(config_addr);
        host_config.format       = vm_read8(config_addr + 1);
        host_config.aspect       = vm_read8(config_addr + 2);
        host_config.pitch        = vm_read32(config_addr + 12);
    }

    s32 rc = cellVideoOutConfigure(videoOut, &host_config, nullptr, 0);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellSysmodule — module loading (REAL bridges)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* PS3 module IDs for logging (must match cellSysmodule.h) */
static const char* get_module_name(uint32_t id) {
    switch (id) {
    case 0x0000: return "NET";
    case 0x0001: return "HTTP";
    case 0x0002: return "HTTP_UTIL";
    case 0x0003: return "SSL";
    case 0x0004: return "HTTPS";
    case 0x0005: return "VDEC";
    case 0x0006: return "ADEC";
    case 0x0007: return "DMUX";
    case 0x0008: return "VPOST";
    case 0x0009: return "RTC";
    case 0x000A: return "SPURS";
    case 0x000B: return "OVIS";
    case 0x000C: return "SHEAP";
    case 0x000D: return "SYNC";
    case 0x000E: return "SYNC2";
    case 0x000F: return "FS";
    case 0x0010: return "JPGDEC";
    case 0x0011: return "GCM_SYS";
    case 0x0012: return "AUDIO";
    case 0x0013: return "PAMF";
    case 0x0014: return "ATRAC3PLUS";
    case 0x0015: return "NETCTL";
    case 0x0016: return "SYSUTIL";
    case 0x0017: return "SYSUTIL_NP";
    case 0x0018: return "IO";
    case 0x0019: return "PNGDEC";
    case 0x001A: return "FONT";
    case 0x001B: return "FREETYPE";
    case 0x001C: return "USBD";
    case 0x001D: return "SAIL";
    case 0x001E: return "L10N";
    case 0x001F: return "RESC";
    case 0x0020: return "DAISY";
    case 0x0021: return "KEY2CHAR";
    case 0x0022: return "MIC";
    case 0x0023: return "AVCONF_EXT";
    case 0x0024: return "USERINFO";
    case 0x0025: return "SAVEDATA";
    case 0x0026: return "GAME";
    case 0x0027: return "SUBDISPLAY";
    case 0x0028: return "GAME_EXEC";
    case 0x0029: return "NP_TROPHY";
    default: return "UNKNOWN";
    }
}

static int64_t bridge_cellSysmoduleLoadModule(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[HLE] cellSysmoduleLoadModule(0x%X = %s) TOC=0x%llX\n", id, get_module_name(id),
            (unsigned long long)ctx->gpr[2]);
    s32 rc = cellSysmoduleLoadModule(id);
    fprintf(stderr, "[cellSysmodule] LoadModule(id=0x%04X '%s') -> 0x%X\n",
            id, get_module_name(id), (uint32_t)rc);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellSysmoduleUnloadModule(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    s32 rc = cellSysmoduleUnloadModule(id);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellGcmSys — RSX graphics system (REAL bridges)
 *
 * Critical for game init. Even without rendering, the game needs GCM
 * initialized to configure display buffers and memory mapping.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* _cellGcmInitBody(context_ptr_addr, cmdSize, ioSize, ioAddress)
 *
 * Real PS3 signature: s32 _cellGcmInitBody(CellGcmContextData** context,
 *                                          u32 cmdSize, u32 ioSize, u32 ioAddr)
 * r3 = guest address of the gCellGcmCurrentContext pointer variable
 * r4 = command buffer size (0 = default)
 * r5 = IO mapping size
 * r6 = IO mapping address
 *
 * After initializing RSX state, we must:
 *   1. Allocate a CellGcmContextData struct in guest memory
 *   2. Allocate a command buffer in guest memory
 *   3. Fill in context->begin/end/current/callback
 *   4. Write the context guest address to *context_ptr_addr
 * The game then writes RSX commands through gCellGcmCurrentContext->current.
 */
/* Forward declarations for RSX command processor */
extern "C" {
    void rsx_state_init(void*);       /* from rsx_commands.c */
    extern void* rsx_get_backend(void); /* from rsx_commands.c */
    void hle_guest_malloc(ppu_context* ctx); /* from malloc_override.cpp */
}

/* Guest command buffer state — tracked for flushing.
 * The cmdbuf_begin/size are also written by hle_gcm_set_cmdbuf() so the
 * SPU-bypass path can install a synthesized GCM context without going
 * through cellGcmInitBody. */
extern "C" uint32_t g_gcm_context_guest = 0;  /* guest addr of CellGcmContextData */
extern "C" uint32_t g_gcm_cmdbuf_begin  = 0;  /* guest addr of command buffer start */
extern "C" uint32_t g_gcm_cmdbuf_size   = 0;  /* command buffer size in bytes */
static uint32_t g_gcm_callback_opd  = 0;  /* guest addr of callback OPD entry */

extern "C" void hle_gcm_install_cmdbuf(uint32_t ctx_addr, uint32_t buf_addr, uint32_t buf_size)
{
    g_gcm_context_guest = ctx_addr;
    g_gcm_cmdbuf_begin  = buf_addr;
    g_gcm_cmdbuf_size   = buf_size;
}

/* Guest address for the GCM control register mirror */
extern "C" uint32_t g_gcm_control_guest = 0;

/* Forward declarations */
static void gcm_flush_guest_cmdbuf(void);
static void gcm_flush_guest_cmdbuf_noreset(void);

/* RSX FIFO watchdog thread.
 * Monitors ctrl->put in guest memory. When put changes (game submitted commands),
 * processes the command buffer and updates ctrl->get and ctrl->ref.
 * This breaks cellGcmFinish spin loops without needing HLE bridge calls. */
static volatile int s_fifo_watchdog_active = 0;
static DWORD WINAPI fifo_watchdog_thread(LPVOID param)
{
    (void)param;
    uint32_t last_put = 0;
    while (s_fifo_watchdog_active) {
        if (!g_gcm_control_guest || !g_gcm_cmdbuf_begin) {
            Sleep(10);
            continue;
        }
        /* Read put from guest memory (big-endian via vm_read32) */
        uint32_t put = vm_read32(g_gcm_control_guest + 0);
        uint32_t get = vm_read32(g_gcm_control_guest + 4);
        if (put != get) {
            /* Game submitted commands — process and advance get.
             * Use the actual command buffer begin address, not IO base.
             * g_gcm_cmdbuf_begin is the guest address of the buffer start. */
            uint32_t buf_start = g_gcm_cmdbuf_begin;
            uint32_t io_base = buf_start & ~0xFFFFF; /* align down to IO map base */
            uint32_t buf_end_addr = io_base + put;
            if (buf_end_addr > buf_start && buf_end_addr < 0x02000000) {
                uint32_t scan_size = buf_end_addr - buf_start;
                const uint32_t* cmdbuf = (const uint32_t*)(vm_base + buf_start);
                uint32_t num_dwords = scan_size / 4;
                static int s_wd_log = 0;
                if (s_wd_log < 3) {
                    fprintf(stderr, "[FIFO-WD] put=0x%X scan %u bytes from 0x%X, first 8 raw:",
                            put, scan_size, buf_start);
                    for (uint32_t d = 0; d < 8 && d < num_dwords; d++)
                        fprintf(stderr, " %08X", _byteswap_ulong(cmdbuf[d]));
                    fprintf(stderr, "\n"); fflush(stderr);
                    s_wd_log++;
                }
                /* Scan for SET_REFERENCE and WRITE_BACK_END_LABEL */
                int found_ref = 0;
                for (uint32_t i = 0; i < num_dwords; ) {
                    uint32_t header = _byteswap_ulong(cmdbuf[i++]);
                    uint32_t type = (header >> 29) & 0x7;
                    if (type == 0 || type == 2) {
                        uint32_t method = ((header >> 2) & 0x7FF) << 2;
                        uint32_t count = (header >> 18) & 0x7FF;
                        for (uint32_t j = 0; j < count && i < num_dwords; j++, i++) {
                            uint32_t m = (type == 0) ? (method + j * 4) : method;
                            uint32_t data = _byteswap_ulong(cmdbuf[i]);
                            if (m == 0x0050) { /* NV406E_SET_REFERENCE */
                                vm_write32(g_gcm_control_guest + 8, data);
                                found_ref = 1;
                                fprintf(stderr, "[FIFO-WD] SET_REFERENCE ref=%u\n", data);
                            }
                            if (m == 0x1D6C) { /* WRITE_BACK_END_LABEL */
                                fprintf(stderr, "[FIFO-WD] WRITE_BACK_LABEL idx=%u\n", data);
                            }
                        }
                    } else {
                        break;
                    }
                }
                if (!found_ref && s_wd_log <= 3) {
                    fprintf(stderr, "[FIFO-WD] No SET_REFERENCE found in %u dwords\n", num_dwords);
                    fflush(stderr);
                }
            }
            /* Set get = put (commands processed) */
            vm_write32(g_gcm_control_guest + 4, put);
            last_put = put;
        }
        Sleep(0); /* yield — tight poll for low latency */
    }
    return 0;
}

static void start_fifo_watchdog(void)
{
    if (!s_fifo_watchdog_active) {
        s_fifo_watchdog_active = 1;
        CreateThread(NULL, 0, fifo_watchdog_thread, NULL, 0, NULL);
        fprintf(stderr, "[RSX] FIFO watchdog thread started\n");
        fflush(stderr);
    }
}

/* GCM command buffer callback — called when current >= end.
 * Flushes pending commands and resets the write pointer. */
static void gcm_callback_handler(void* vctx)
{
    ppu_context* ctx = (ppu_context*)vctx;
    (void)ctx;

    if (!g_gcm_context_guest || !g_gcm_cmdbuf_begin) return;

    /* 1. Flush pending RSX commands before resetting the buffer */
    gcm_flush_guest_cmdbuf();

    /* 2. Update control register: set put to current write position,
     *    then set get = put (instant RSX processing) */
    if (g_gcm_control_guest) {
        uint32_t current = vm_read32(g_gcm_context_guest + 12);
        uint32_t begin   = vm_read32(g_gcm_context_guest + 4);
        uint32_t put_offset = current - begin; /* RSX offset */
        /* HOST-ENDIAN writes — game reads via lwbrx */
        uint32_t* ctrl_cb = (uint32_t*)(vm_base + g_gcm_control_guest);
        ctrl_cb[0] = put_offset; /* put */
        ctrl_cb[1] = put_offset; /* get = put */
    }

    /* 3. Reset the write pointer to beginning of command buffer */
    vm_write32(g_gcm_context_guest + 12, g_gcm_cmdbuf_begin);  /* current = begin */

    static int s_cb_count = 0;
    s_cb_count++;
    if (s_cb_count <= 10 || s_cb_count % 100 == 0) {
        fprintf(stderr, "[GCM-CALLBACK] Buffer overflow reset #%d\n", s_cb_count);
    }

    /* Return CELL_OK */
    ctx->gpr[3] = 0;
}

static int64_t bridge_cellGcmInitBody(ppu_context* ctx)
{
    uint32_t contextPtrAddr = (uint32_t)ctx->gpr[3];
    uint32_t cmdSize        = (uint32_t)ctx->gpr[4];
    uint32_t ioSize         = (uint32_t)ctx->gpr[5];
    uint32_t ioAddress      = (uint32_t)ctx->gpr[6];

    fprintf(stderr, "[HLE] _cellGcmInitBody(ctx_ptr=0x%08X, cmdSize=0x%x, ioSize=0x%x, ioAddr=0x%x)\n",
            contextPtrAddr, cmdSize, ioSize, ioAddress);

    /* The wrapper reads &gCellGcmCurrentContext from TOC-0x2A50 (0x00893F58).
     * ELF analysis: GOT at 0x00893F58 should contain 0x101ED198 (BSS addr of the pointer).
     * The game's init code zeroes this GOT entry before calling cellGcmInit,
     * so r3 arrives as 0. Fix: use the known address AND restore the GOT entry
     * so inline GCM functions (compiled into the game) can also find the context. */
    if (contextPtrAddr == 0) {
        contextPtrAddr = 0x101ED198;
        /* Restore the GOT entry so future TOC-relative reads work */
        vm_write32(0x00893F58, 0x101ED198);
        fprintf(stderr, "[HLE]   Fixed GOT entry at 0x00893F58 -> 0x101ED198\n");
        fprintf(stderr, "[HLE]   Using gCellGcmCurrentContext addr: 0x%08X\n", contextPtrAddr);
    }

    s32 rc = cellGcmInit(cmdSize, ioSize, ioAddress);

    if (rc == CELL_OK) {
        fprintf(stderr, "[HLE] cellGcmInit SUCCESS — RSX initialized\n");

        /* Log the configuration */
        CellGcmConfig config;
        cellGcmGetConfiguration(&config);
        fprintf(stderr, "[HLE]   localAddr=0x%08X localSize=%u MB\n",
                config.localAddress, config.localSize / (1024*1024));
        fprintf(stderr, "[HLE]   ioAddr=0x%08X ioSize=%u MB\n",
                config.ioAddress, config.ioSize / (1024*1024));

        /* Check if a graphics backend is registered */
        if (rsx_get_backend()) {
            fprintf(stderr, "[HLE]   Graphics backend: active\n");
        } else {
            fprintf(stderr, "[HLE]   Graphics backend: none (null rendering)\n");
        }

        /* --- Set up guest command buffer and CellGcmContextData --- */

        /* Command buffer: use the game's requested size (0xF0000 = 960KB).
         * The callback handler (at 0xDEAD00) resets the buffer when full.
         * We previously used 16MB which consumed heap space needed for
         * game function pointers. */
        uint32_t buf_size = cmdSize ? cmdSize : (1024 * 1024);

        /* Allocate command buffer in guest memory */
        ppu_context tmp = *ctx;
        tmp.gpr[3] = buf_size;
        hle_guest_malloc(&tmp);
        uint32_t buf_addr = (uint32_t)tmp.gpr[3];

        /* Allocate CellGcmContextData (16 bytes) in guest memory */
        tmp.gpr[3] = 32; /* 32 bytes, aligned */
        hle_guest_malloc(&tmp);
        uint32_t ctx_addr = (uint32_t)tmp.gpr[3];

        if (buf_addr && ctx_addr) {
            g_gcm_context_guest = ctx_addr;
            g_gcm_cmdbuf_begin  = buf_addr;
            g_gcm_cmdbuf_size   = buf_size;

            /* Allocate an OPD entry for the command buffer callback.
             * PPC64 ELF v1 calls functions through OPDs: [func_addr, toc].
             * We allocate a small OPD in guest memory and register a host
             * handler at the function address in the dispatch table. */
            {
                ppu_context opd_tmp = *ctx;
                opd_tmp.gpr[3] = 16; /* OPD: 4 bytes func + 4 bytes TOC */
                hle_guest_malloc(&opd_tmp);
                g_gcm_callback_opd = (uint32_t)opd_tmp.gpr[3];

                if (g_gcm_callback_opd) {
                    /* Use a recognizable guest address for the callback function */
                    uint32_t cb_func_addr = 0x00DEAD00;
                    vm_write32(g_gcm_callback_opd + 0, cb_func_addr); /* function entry */
                    vm_write32(g_gcm_callback_opd + 4, 0x008969A8);   /* TOC */

                    /* Register the handler in the dispatch table */
                    dispatch_register_external(cb_func_addr, gcm_callback_handler);

                    fprintf(stderr, "[HLE]   GCM callback OPD at 0x%08X -> func 0x%08X\n",
                            g_gcm_callback_opd, cb_func_addr);
                }
            }

            /* Write CellGcmContextData fields to guest memory (big-endian).
             * The game's compiled SDK inline functions expect the callback
             * at offset 0 (confirmed by the bctrl targeting our begin value
             * when we put begin at offset 0). Layout:
             *   offset 0:  callback (OPD pointer for buffer overflow)
             *   offset 4:  begin (start of command buffer)
             *   offset 8:  end (end of command buffer)
             *   offset 12: current (current write position) */
            vm_write32(ctx_addr + 0,  g_gcm_callback_opd);    /* callback OPD */
            vm_write32(ctx_addr + 4,  buf_addr);              /* begin */
            vm_write32(ctx_addr + 8,  buf_addr + buf_size);   /* end */
            vm_write32(ctx_addr + 12, buf_addr);              /* current = begin */

            fprintf(stderr, "[HLE]   GCM context at guest 0x%08X\n", ctx_addr);
            fprintf(stderr, "[HLE]   Command buffer: 0x%08X - 0x%08X (%u KB)\n",
                    buf_addr, buf_addr + buf_size, buf_size / 1024);

            /* Write the context pointer to gCellGcmCurrentContext */
            vm_write32(contextPtrAddr, ctx_addr);
            fprintf(stderr, "[HLE]   Wrote context ptr to guest 0x%08X\n", contextPtrAddr);

            /* Map the command buffer region so cellGcmAddressToOffset works.
             * The buffer is at buf_addr which may not be 1MB-aligned. Map the
             * surrounding 1MB-aligned region. */
            {
                uint32_t map_ea = buf_addr & ~0xFFFFF; /* align down to 1MB */
                uint32_t map_end = (buf_addr + buf_size + 0xFFFFF) & ~0xFFFFF;
                uint32_t map_size = map_end - map_ea;
                uint32_t map_offset = 0;
                s32 map_rc = cellGcmMapMainMemory(map_ea, map_size, &map_offset);
                fprintf(stderr, "[HLE]   Mapped cmd buffer region: EA=0x%08X size=0x%X -> offset=0x%X rc=%d\n",
                        map_ea, map_size, map_offset, map_rc);
            }
        } else {
            fprintf(stderr, "[HLE]   ERROR: Failed to allocate guest command buffer!\n");
        }
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmGetConfiguration(config_ptr) */
static int64_t bridge_cellGcmGetConfiguration(ppu_context* ctx)
{
    uint32_t config_addr = (uint32_t)ctx->gpr[3];

    CellGcmConfig host_config;
    s32 rc = cellGcmGetConfiguration(&host_config);

    if (rc == CELL_OK && config_addr) {
        vm_write32(config_addr,      host_config.localAddress);
        vm_write32(config_addr + 4,  host_config.ioAddress);
        vm_write32(config_addr + 8,  host_config.localSize);
        vm_write32(config_addr + 12, host_config.ioSize);
        vm_write32(config_addr + 16, host_config.memoryFrequency);
        vm_write32(config_addr + 20, host_config.coreFrequency);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmGetControlRegister() → r3 = pointer to CellGcmControl */
static int64_t bridge_cellGcmGetControlRegister(ppu_context* ctx)
{
    /* The game expects a guest address pointing to a CellGcmControl struct
     * (put, get, ref — 3 u32 values = 12 bytes). We allocate this in guest
     * memory so vm_read/vm_write can access it. */
    if (g_gcm_control_guest == 0) {
        /* Allocate in our heap region */
        /* hle_guest_malloc declared at top of file */
        ppu_context tmp = *ctx;
        tmp.gpr[3] = 64; /* allocate 64 bytes for control struct */
        hle_guest_malloc(&tmp);
        g_gcm_control_guest = (uint32_t)tmp.gpr[3];

        if (g_gcm_control_guest) {
            /* Initialize: put=0, get=0, ref=0 in HOST-ENDIAN.
             * Game reads via lwbrx (host-endian on recompiled x86). */
            uint32_t* ctrl_init = (uint32_t*)(vm_base + g_gcm_control_guest);
            ctrl_init[0] = 0; /* put */
            ctrl_init[1] = 0; /* get */
            ctrl_init[2] = 0; /* ref */
            fprintf(stderr, "[HLE] cellGcmGetControlRegister -> 0x%08X\n",
                    g_gcm_control_guest);
            /* Start FIFO watchdog to process commands automatically */
            start_fifo_watchdog();
        }
    }

    /* Emulate instant RSX command processing.
     * CRITICAL: The control register is RSX MMIO (little-endian).
     * The game uses lwbrx (byte-reversed load) to read it, which in
     * recompiled code becomes a HOST-ENDIAN read (no byte-swap).
     * So we must write in HOST-ENDIAN (direct memory, not vm_write32). */
    if (g_gcm_control_guest) {
        gcm_flush_guest_cmdbuf_noreset();
        /* Read/write control register in HOST-ENDIAN (direct memory access).
         * The game wrote put via lwbrx-style store (host-endian). */
        uint32_t* ctrl = (uint32_t*)(vm_base + g_gcm_control_guest);
        uint32_t put = ctrl[0]; /* read put (host-endian) */
        ctrl[1] = put;          /* get = put (host-endian) */
        /* Advance ref when put changes */
        static uint32_t s_last_put = 0;
        if (put != s_last_put) {
            ctrl[2] = ctrl[2] + 1; /* ref++ (host-endian) */
            s_last_put = put;
        }
        /* Spin detection: if GetControlRegister is called >5000 times
         * rapidly, the game is stuck in a FIFO sync loop. Use longjmp
         * to break out, similar to CRT abort redirect. */
        static int s_ctrl_total = 0;
        s_ctrl_total++;
        /* On iteration 50, dump the PPU state to understand the loop condition */
        if (s_ctrl_total == 50) {
            fprintf(stderr, "[CTRL-SPIN] Iteration 50 — dumping loop state:\n");
            fprintf(stderr, "  r3=0x%llX r4=0x%llX r9=0x%llX r11=0x%llX r31=0x%llX\n",
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                    (unsigned long long)ctx->gpr[9], (unsigned long long)ctx->gpr[11],
                    (unsigned long long)ctx->gpr[31]);
            /* Dump memory at r31 (the data structure being checked) */
            uint32_t r31 = (uint32_t)ctx->gpr[31];
            if (r31 > 0x100000 && r31 < 0x10000000) {
                fprintf(stderr, "  *(r31): ");
                for (int d = 0; d < 8; d++)
                    fprintf(stderr, "%08X ", vm_read32(r31 + d * 4));
                fprintf(stderr, "\n");
            }
            /* Also dump what r3 points to (control register area) */
            uint32_t r3 = (uint32_t)ctx->gpr[3];
            if (r3 > 0x100000 && r3 < 0x10000000) {
                fprintf(stderr, "  *(r3):  ");
                for (int d = 0; d < 4; d++)
                    fprintf(stderr, "%08X ", vm_read32(r3 + d * 4));
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
        /* Clear PhyreEngine FIFO pending flags on ALL structures.
         * func_000D5054 sets +0x18=1 on r27, r28, r30, r26.
         * Clear all of them so the caller doesn't spin. */
        {
            uint32_t regs[] = {
                (uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[27],
                (uint32_t)ctx->gpr[28], (uint32_t)ctx->gpr[30],
                (uint32_t)ctx->gpr[26], (uint32_t)ctx->gpr[29]
            };
            for (int ri = 0; ri < 6; ri++) {
                uint32_t r = regs[ri];
                if (r > 0x100000 && r < 0x10000000) {
                    if (vm_read32(r + 0x18) != 0) vm_write32(r + 0x18, 0);
                }
            }
            /* Patch NULL OPDs in heap objects the game is trying to call.
             * When the game reads an OPD with func=0, it spins forever.
             * Write a NOP function (func_000CBF40) to make it proceed. */
            uint32_t ctr = (uint32_t)ctx->ctr;
            if (ctr == 0) {
                /* CTR=0 means the game is about to call a NULL function.
                 * The OPD was read from some object. Patch it. */
                uint32_t r9 = (uint32_t)ctx->gpr[9];
                if (r9 > 0x100000 && r9 < 0x10000000 && vm_read32(r9) == 0) {
                    vm_write32(r9, 0x000CBF40); /* NOP function */
                    vm_write32(r9 + 4, 0x008969A8); /* TOC */
                    ctx->ctr = 0x000CBF40;
                    static int s_nop_patches = 0;
                    if (s_nop_patches < 10) {
                        fprintf(stderr, "[CTRL-SPIN] Patched NULL OPD at 0x%08X\n", r9);
                        fflush(stderr);
                        s_nop_patches++;
                    }
                }
            }
        }
        if (s_ctrl_total == 5000) {
            fprintf(stderr, "[CTRL-SPIN] 100000 calls, forcing longjmp!\n");
            fflush(stderr);
            s_ctrl_total = 0;
            longjmp(g_abort_jmp, 42);
        }
    }

    ctx->gpr[3] = g_gcm_control_guest;
    return 0;
}

/* cellGcmSetFlipMode(mode) */
static int64_t bridge_cellGcmSetFlipMode(ppu_context* ctx)
{
    cellGcmSetFlipMode((uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = 0;
    return 0;
}

/* cellGcmGetFlipStatus() → r3 = status */
static int64_t bridge_cellGcmGetFlipStatus(ppu_context* ctx)
{
    ctx->gpr[3] = cellGcmGetFlipStatus();
    return 0;
}

/* cellGcmSetWaitFlip() */
static int64_t bridge_cellGcmSetWaitFlip(ppu_context* ctx)
{
    cellGcmSetWaitFlip();
    ctx->gpr[3] = 0;
    return 0;
}

/* cellGcmResetFlipStatus() */
static int64_t bridge_cellGcmResetFlipStatus(ppu_context* ctx)
{
    cellGcmResetFlipStatus();
    ctx->gpr[3] = 0;
    return 0;
}

/* cellGcmSetDisplayBuffer(bufferId, offset, pitch, width, height) */
static int64_t bridge_cellGcmSetDisplayBuffer(ppu_context* ctx)
{
    s32 rc = cellGcmSetDisplayBuffer(
        (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
        (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
        (uint32_t)ctx->gpr[7]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmAddressToOffset(address, offset_ptr) */
static int64_t bridge_cellGcmAddressToOffset(ppu_context* ctx)
{
    uint32_t address     = (uint32_t)ctx->gpr[3];
    uint32_t offset_addr = (uint32_t)ctx->gpr[4];

    uint32_t host_offset = 0;
    s32 rc = cellGcmAddressToOffset(address, &host_offset);

    /* Fallback: if the address isn't mapped, synthesize an offset.
     * Addresses in the command buffer / label / heap region that weren't
     * explicitly mapped with cellGcmMapMainMemory still need offsets. */
    if (rc != 0 && address >= 0x00A00000 && address < 0x02000000) {
        host_offset = address - 0x00A00000;
        rc = 0;
        static int s_fallback_count = 0;
        if (s_fallback_count < 5) {
            fprintf(stderr, "[HLE] cellGcmAddressToOffset: fallback 0x%08X -> offset 0x%08X\n",
                    address, host_offset);
            s_fallback_count++;
        }
    }

    if (rc == CELL_OK && offset_addr)
        vm_write32(offset_addr, host_offset);

    /* Debug: log address, offset, and control register */
    {
        static int s_a2o_log = 0;
        if (s_a2o_log < 3 && g_gcm_control_guest) {
            uint32_t put = vm_read32(g_gcm_control_guest + 0);
            uint32_t get = vm_read32(g_gcm_control_guest + 4);
            fprintf(stderr, "[A2O] addr=0x%08X offset=0x%08X rc=%d put=0x%X get=0x%X\n",
                    address, host_offset, rc, put, get);
            fflush(stderr);
            s_a2o_log++;
        }
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmMapMainMemory(ea, size, offset_ptr) */
static int64_t bridge_cellGcmMapMainMemory(ppu_context* ctx)
{
    uint32_t ea          = (uint32_t)ctx->gpr[3];
    uint32_t size        = (uint32_t)ctx->gpr[4];
    uint32_t offset_addr = (uint32_t)ctx->gpr[5];

    /* Fix: EA must be 1MB aligned for cellGcmMapMainMemory.
     * Our bump allocator doesn't align, so round up the EA. */
    uint32_t aligned_ea = (ea + 0xFFFFF) & ~0xFFFFF;
    if (aligned_ea != ea) {
        fprintf(stderr, "[HLE] cellGcmMapMainMemory: aligning EA 0x%08X -> 0x%08X\n", ea, aligned_ea);
        ea = aligned_ea;
    }

    uint32_t host_offset = 0;
    s32 rc = cellGcmMapMainMemory(ea, size, &host_offset);

    fprintf(stderr, "[HLE] cellGcmMapMainMemory(ea=0x%08X, size=0x%X) -> offset=0x%08X rc=%d\n",
            ea, size, host_offset, rc); fflush(stderr);

    if (rc == CELL_OK && offset_addr)
        vm_write32(offset_addr, host_offset);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmSetVBlankHandler(handler) */
static int64_t bridge_cellGcmSetVBlankHandler(ppu_context* ctx)
{
    /* Store the guest function pointer; real VBlank won't fire yet */
    cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)(uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = 0;
    return 0;
}

/* cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank) */
static int64_t bridge_cellGcmSetTile(ppu_context* ctx)
{
    s32 rc = cellGcmSetTile(
        (uint8_t)ctx->gpr[3], (uint8_t)ctx->gpr[4],
        (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
        (uint32_t)ctx->gpr[7], (uint8_t)ctx->gpr[8],
        (uint16_t)ctx->gpr[9], (uint8_t)ctx->gpr[10]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmSetZcull(index, offset, width, height, cullStart, zFormat, aaFormat,
 *                  zcullDir, zcullFormat, sFunc, sRef, sMask) */
static int64_t bridge_cellGcmSetZcull(ppu_context* ctx)
{
    /* First 8 args in r3-r10, remaining on stack */
    s32 rc = cellGcmSetZcull(
        (uint8_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
        (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
        (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8],
        (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[10],
        /* Remaining args are on stack. Read from guest stack frame. */
        vm_read32((uint32_t)ctx->gpr[1] + 0x70),  /* zcullFormat */
        vm_read32((uint32_t)ctx->gpr[1] + 0x78),  /* sFunc */
        vm_read32((uint32_t)ctx->gpr[1] + 0x80),  /* sRef */
        vm_read32((uint32_t)ctx->gpr[1] + 0x88)); /* sMask */
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmSetInvalidateTile — stub (internal function) */
static int64_t bridge_cellGcmSetInvalidateTile(ppu_context* ctx)
{
    (void)ctx;
    ctx->gpr[3] = 0;
    return 0;
}

/* Guest address for label array mirror */
static uint32_t g_gcm_labels_guest = 0;

/* cellGcmGetLabelAddress(index) → r3 = pointer */
static int64_t bridge_cellGcmGetLabelAddress(ppu_context* ctx)
{
    uint8_t index = (uint8_t)ctx->gpr[3];

    /* Allocate label array in guest memory on first call (256 labels × 4 bytes) */
    if (g_gcm_labels_guest == 0) {
        /* hle_guest_malloc declared at top of file */
        ppu_context tmp = *ctx;
        tmp.gpr[3] = 256 * 4 + 64; /* 256 labels + padding */
        hle_guest_malloc(&tmp);
        g_gcm_labels_guest = (uint32_t)tmp.gpr[3];
        fprintf(stderr, "[HLE] cellGcmGetLabelAddress: allocated label array at 0x%08X\n",
                g_gcm_labels_guest);
    }

    if (g_gcm_labels_guest && index < 256)
        ctx->gpr[3] = g_gcm_labels_guest + index * 4;
    else
        ctx->gpr[3] = 0;
    return 0;
}

/* cellGcmGetTiledPitchSize(size, pitch_ptr) */
static int64_t bridge_cellGcmGetTiledPitchSize(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t pitch_addr = (uint32_t)ctx->gpr[4];

    uint32_t host_pitch = 0;
    s32 rc = cellGcmGetTiledPitchSize(size, &host_pitch);

    if (rc == CELL_OK && pitch_addr)
        vm_write32(pitch_addr, host_pitch);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* Flush the guest command buffer — process RSX commands and reset write pointer.
 * Called on flip and other synchronization points. */
extern "C" int rsx_process_command_buffer(void* state, const uint32_t* buf, uint32_t size);
extern "C" void rsx_state_init(void* state);

/* RSX state — initialized once, updated by command buffer processing */
static uint8_t s_rsx_state_buf[4096]; /* rsx_state is large, use static buffer */
static int s_rsx_state_inited = 0;

static void gcm_flush_guest_cmdbuf_impl(int reset_current);
static void gcm_flush_guest_cmdbuf(void) { gcm_flush_guest_cmdbuf_impl(1); }
static void gcm_flush_guest_cmdbuf_noreset(void) { gcm_flush_guest_cmdbuf_impl(0); }

static void gcm_flush_guest_cmdbuf_impl(int reset_current)
{
    if (!g_gcm_context_guest || !g_gcm_cmdbuf_begin) return;

    /* Initialize RSX state on first flush */
    if (!s_rsx_state_inited) {
        rsx_state_init(s_rsx_state_buf);
        s_rsx_state_inited = 1;
        fprintf(stderr, "[GCM-FLUSH] RSX state initialized\n");
    }

    /* Read current write position from guest context (offset 12 in our layout) */
    uint32_t current = vm_read32(g_gcm_context_guest + 12);
    uint32_t begin   = g_gcm_cmdbuf_begin;

    if (current <= begin) return; /* nothing to process */

    uint32_t used_bytes = current - begin;

    static int s_flush_count = 0;
    s_flush_count++;
    if (s_flush_count <= 10 || s_flush_count % 100 == 0) {
        fprintf(stderr, "[GCM-FLUSH] Processing %u bytes of RSX commands (flush #%d)\n",
                used_bytes, s_flush_count);
    }

    /* Process the command buffer through the RSX command processor. */
    const uint32_t* cmdbuf = (const uint32_t*)(vm_base + begin);
    int methods = rsx_process_command_buffer(s_rsx_state_buf, cmdbuf, used_bytes);
    if (s_flush_count <= 10) {
        fprintf(stderr, "[GCM-FLUSH] Processed %d RSX methods\n", methods);
    }

    /* Scan for NV406E_SET_REFERENCE (method 0x0050) and update ctrl->ref.
     * Commands are BIG-ENDIAN (PS3 format), byte-swap before parsing. */
    if (g_gcm_control_guest) {
        uint32_t num_dwords = used_bytes / 4;
        /* Debug: dump first few dwords */
        if (s_flush_count <= 3) {
            fprintf(stderr, "[GCM-SCAN] %u dwords, first 8:", num_dwords);
            for (uint32_t d = 0; d < 8 && d < num_dwords; d++)
                fprintf(stderr, " %08X(%08X)", cmdbuf[d], _byteswap_ulong(cmdbuf[d]));
            fprintf(stderr, "\n"); fflush(stderr);
        }
        for (uint32_t i = 0; i < num_dwords; ) {
            uint32_t header = _byteswap_ulong(cmdbuf[i++]);
            uint32_t type = (header >> 29) & 0x7;
            if (type == 0 || type == 2) {
                uint32_t method = ((header >> 2) & 0x7FF) << 2;
                uint32_t count = (header >> 18) & 0x7FF;
                for (uint32_t j = 0; j < count && i < num_dwords; j++, i++) {
                    uint32_t m = (type == 0) ? (method + j * 4) : method;
                    uint32_t data = _byteswap_ulong(cmdbuf[i]);
                    if (m == 0x0050) { /* NV406E_SET_REFERENCE */
                        vm_write32(g_gcm_control_guest + 8, data);
                        static int s_ref_log = 0;
                        if (s_ref_log < 5) {
                            fprintf(stderr, "[GCM-FLUSH] SET_REFERENCE ref=%u\n", data);
                            s_ref_log++;
                        }
                    }
                    if (m == 0x1D6C && (j + 1) < count && (i + 1) < num_dwords) {
                        /* NV4097_SET_WRITE_BACK_END_LABEL: index=data, value=next */
                        uint32_t label_index = data;
                        uint32_t label_value = _byteswap_ulong(cmdbuf[i + 1]);
                        if (g_gcm_labels_guest && label_index < 256) {
                            vm_write32(g_gcm_labels_guest + label_index * 4, label_value);
                            static int s_lbl_log = 0;
                            if (s_lbl_log < 5) {
                                fprintf(stderr, "[GCM-FLUSH] WRITE_BACK_LABEL[%u]=%u\n",
                                        label_index, label_value);
                                s_lbl_log++;
                            }
                        }
                    }
                }
            } else {
                break; /* jump or unknown */
            }
        }
    }

    /* Reset the write pointer to the beginning so the game can reuse the buffer.
     * Only reset when explicitly requested (flip/callback), not on auto-flush
     * from GetControlRegister (the game may still be writing to the buffer). */
    if (reset_current) {
        vm_write32(g_gcm_context_guest + 12, begin);  /* current = begin */
    }
}

/* cellGcmSetFlip(bufferId) */
static int64_t bridge_cellGcmSetFlip(ppu_context* ctx)
{
    /* Flush pending RSX commands before flip */
    gcm_flush_guest_cmdbuf();

    uint32_t buf_id = (uint32_t)ctx->gpr[3];
    s32 rc = cellGcmSetFlipCommand(buf_id);
    if (rc != 0) {
        static int s_flip_err_count = 0;
        if (s_flip_err_count < 5) {
            fprintf(stderr, "[HLE] cellGcmSetFlip(buf=%u) FAILED rc=0x%X\n", buf_id, (unsigned)rc);
            s_flip_err_count++;
        }
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* _cellGcmFunc15 — internal helper, safe to stub */
static int64_t bridge_cellGcmFunc15(ppu_context* ctx)
{
    (void)ctx;
    ctx->gpr[3] = 0;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellAudio — audio output (REAL bridges)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int64_t bridge_cellAudioInit(ppu_context* ctx)
{
    fprintf(stderr, "[HLE] cellAudioInit()\n");
    s32 rc = cellAudioInit();
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellAudioQuit(ppu_context* ctx)
{
    s32 rc = cellAudioQuit();
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellAudioPortOpen(ppu_context* ctx)
{
    uint32_t param_addr   = (uint32_t)ctx->gpr[3];
    uint32_t portnum_addr = (uint32_t)ctx->gpr[4];

    /* Read CellAudioPortParam from guest memory */
    CellAudioPortParam host_param;
    host_param.nChannel = vm_read64(param_addr);
    host_param.nBlock   = vm_read64(param_addr + 8);
    host_param.attr     = vm_read64(param_addr + 16);
    /* float is 4 bytes; read as u32 and reinterpret */
    uint32_t level_bits = vm_read32(param_addr + 24);
    memcpy(&host_param.level, &level_bits, 4);

    uint32_t host_portnum = 0;
    s32 rc = cellAudioPortOpen(&host_param, &host_portnum);

    if (rc == CELL_OK && portnum_addr)
        vm_write32(portnum_addr, host_portnum);

    fprintf(stderr, "[HLE] cellAudioPortOpen(ch=%llu, blk=%llu) → port %u, rc=0x%x\n",
            (unsigned long long)host_param.nChannel, (unsigned long long)host_param.nBlock,
            host_portnum, rc);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellAudioPortClose(ppu_context* ctx)
{
    s32 rc = cellAudioPortClose((uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellAudioPortStart(ppu_context* ctx)
{
    s32 rc = cellAudioPortStart((uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellAudioPortStop(ppu_context* ctx)
{
    s32 rc = cellAudioPortStop((uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellAudioGetPortConfig(ppu_context* ctx)
{
    uint32_t portNum     = (uint32_t)ctx->gpr[3];
    uint32_t config_addr = (uint32_t)ctx->gpr[4];

    CellAudioPortConfig host_config;
    memset(&host_config, 0, sizeof(host_config));
    s32 rc = cellAudioGetPortConfig(portNum, &host_config);

    if (rc == CELL_OK && config_addr) {
        vm_write64(config_addr,      host_config.readIndexAddr);
        vm_write32(config_addr + 8,  host_config.status);
        vm_write64(config_addr + 16, host_config.nChannel);
        vm_write64(config_addr + 24, host_config.nBlock);
        vm_write32(config_addr + 32, host_config.portSize);
        vm_write64(config_addr + 40, host_config.portAddr);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellFs / sys_fs — filesystem (REAL bridges)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int64_t bridge_cellFsOpen(ppu_context* ctx)
{
    const char* path  = guest_str(ctx->gpr[3]);
    s32 flags         = (s32)ctx->gpr[4];
    uint32_t fd_addr  = (uint32_t)ctx->gpr[5];

    fprintf(stderr, "[HLE] cellFsOpen(\"%s\", flags=0x%x)\n", path ? path : "(null)", flags);

    CellFsFd host_fd = -1;
    s32 rc = cellFsOpen(path, flags, &host_fd, nullptr, 0);

    if (rc == CELL_OK && fd_addr)
        vm_write32(fd_addr, (uint32_t)host_fd);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsClose(ppu_context* ctx)
{
    s32 rc = cellFsClose((CellFsFd)(s32)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsRead(ppu_context* ctx)
{
    CellFsFd fd       = (CellFsFd)(s32)ctx->gpr[3];
    uint32_t buf_addr = (uint32_t)ctx->gpr[4];
    uint64_t nbytes   = ctx->gpr[5];
    uint32_t nread_addr = (uint32_t)ctx->gpr[6];

    /* Read directly into guest memory (data bytes don't need endian swap) */
    void* buf = guest_ptr(buf_addr);
    uint64_t host_nread = 0;
    s32 rc = cellFsRead(fd, buf, nbytes, &host_nread);

    if (nread_addr)
        vm_write64(nread_addr, host_nread);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsWrite(ppu_context* ctx)
{
    CellFsFd fd       = (CellFsFd)(s32)ctx->gpr[3];
    uint32_t buf_addr = (uint32_t)ctx->gpr[4];
    uint64_t nbytes   = ctx->gpr[5];
    uint32_t nwrite_addr = (uint32_t)ctx->gpr[6];

    const void* buf = guest_ptr(buf_addr);
    uint64_t host_nwrite = 0;
    s32 rc = cellFsWrite(fd, buf, nbytes, &host_nwrite);

    if (nwrite_addr)
        vm_write64(nwrite_addr, host_nwrite);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsLseek(ppu_context* ctx)
{
    CellFsFd fd       = (CellFsFd)(s32)ctx->gpr[3];
    s64 offset        = (s64)ctx->gpr[4];
    s32 whence        = (s32)ctx->gpr[5];
    uint32_t pos_addr = (uint32_t)ctx->gpr[6];

    uint64_t host_pos = 0;
    s32 rc = cellFsLseek(fd, offset, whence, &host_pos);

    if (pos_addr)
        vm_write64(pos_addr, host_pos);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsFstat(ppu_context* ctx)
{
    CellFsFd fd        = (CellFsFd)(s32)ctx->gpr[3];
    uint32_t stat_addr = (uint32_t)ctx->gpr[4];

    CellFsStat host_stat;
    memset(&host_stat, 0, sizeof(host_stat));
    s32 rc = cellFsFstat(fd, &host_stat);

    if (rc == CELL_OK && stat_addr) {
        vm_write32(stat_addr,      (uint32_t)host_stat.st_mode);
        vm_write32(stat_addr + 4,  (uint32_t)host_stat.st_uid);
        vm_write32(stat_addr + 8,  (uint32_t)host_stat.st_gid);
        vm_write64(stat_addr + 16, (uint64_t)host_stat.st_atime);
        vm_write64(stat_addr + 24, (uint64_t)host_stat.st_mtime);
        vm_write64(stat_addr + 32, (uint64_t)host_stat.st_ctime);
        vm_write64(stat_addr + 40, host_stat.st_size);
        vm_write64(stat_addr + 48, host_stat.st_blksize);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsOpendir(ppu_context* ctx)
{
    const char* path = guest_str(ctx->gpr[3]);
    uint32_t fd_addr = (uint32_t)ctx->gpr[4];

    CellFsDir host_fd = -1;
    s32 rc = cellFsOpendir(path, &host_fd);

    if (rc == CELL_OK && fd_addr)
        vm_write32(fd_addr, (uint32_t)host_fd);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsReaddir(ppu_context* ctx)
{
    CellFsDir fd        = (CellFsDir)(s32)ctx->gpr[3];
    uint32_t entry_addr = (uint32_t)ctx->gpr[4];
    uint32_t nread_addr = (uint32_t)ctx->gpr[5];

    CellFsDirectoryEntry host_entry;
    memset(&host_entry, 0, sizeof(host_entry));
    uint64_t host_nread = 0;
    s32 rc = cellFsReaddir(fd, &host_entry, &host_nread);

    if (rc == CELL_OK && entry_addr) {
        /* Write CellFsStat portion */
        vm_write32(entry_addr,      (uint32_t)host_entry.attribute.st_mode);
        vm_write32(entry_addr + 4,  (uint32_t)host_entry.attribute.st_uid);
        vm_write32(entry_addr + 8,  (uint32_t)host_entry.attribute.st_gid);
        vm_write64(entry_addr + 16, (uint64_t)host_entry.attribute.st_atime);
        vm_write64(entry_addr + 24, (uint64_t)host_entry.attribute.st_mtime);
        vm_write64(entry_addr + 32, (uint64_t)host_entry.attribute.st_ctime);
        vm_write64(entry_addr + 40, host_entry.attribute.st_size);
        vm_write64(entry_addr + 48, host_entry.attribute.st_blksize);
        /* entry_name (byte-neutral string) */
        memcpy(guest_ptr(entry_addr + 56), host_entry.entry_name,
               CELL_FS_MAX_FS_FILE_NAME_LENGTH);
    }

    if (nread_addr)
        vm_write64(nread_addr, host_nread);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsClosedir(ppu_context* ctx)
{
    s32 rc = cellFsClosedir((CellFsDir)(s32)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsUnlink(ppu_context* ctx)
{
    const char* path = guest_str(ctx->gpr[3]);
    s32 rc = cellFsUnlink(path);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellFsGetFreeSize(ppu_context* ctx)
{
    const char* path       = guest_str(ctx->gpr[3]);
    uint32_t blksize_addr  = (uint32_t)ctx->gpr[4];
    uint32_t freeblk_addr  = (uint32_t)ctx->gpr[5];

    uint32_t host_blksize = 0;
    uint64_t host_freeblk = 0;
    s32 rc = cellFsGetFreeSize(path, &host_blksize, &host_freeblk);

    if (blksize_addr) vm_write32(blksize_addr, host_blksize);
    if (freeblk_addr) vm_write64(freeblk_addr, host_freeblk);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cellPad / sys_io — input (REAL bridges)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int64_t bridge_cellPadInit(ppu_context* ctx)
{
    uint32_t max_connect = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[HLE] cellPadInit(max=%u)\n", max_connect);
    s32 rc = cellPadInit(max_connect);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellPadEnd(ppu_context* ctx)
{
    s32 rc = cellPadEnd();
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

static int64_t bridge_cellPadGetData(ppu_context* ctx)
{
    uint32_t port_no  = (uint32_t)ctx->gpr[3];
    uint32_t data_addr = (uint32_t)ctx->gpr[4];

    CellPadData host_data;
    memset(&host_data, 0, sizeof(host_data));
    s32 rc = cellPadGetData(port_no, &host_data);

    if (rc == CELL_OK && data_addr) {
        vm_write16(data_addr, (uint16_t)host_data.len);
        for (int i = 0; i < CELL_PAD_MAX_CODES && i < host_data.len; i++)
            vm_write16(data_addr + 2 + i * 2, host_data.button[i]);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellPadGetInfo — old API. Bridge to cellPadGetInfo2 internally.
 * The old CellPadInfo struct is: { max_connect, now_connect, system_info, status[7] }
 * which is the same layout as CellPadInfo2 minus port_setting[]. */
static int64_t bridge_cellPadGetInfo(ppu_context* ctx)
{
    uint32_t info_addr = (uint32_t)ctx->gpr[3];

    CellPadInfo2 host_info;
    memset(&host_info, 0, sizeof(host_info));
    s32 rc = cellPadGetInfo2(&host_info);

    if (rc == CELL_OK && info_addr) {
        vm_write32(info_addr,     host_info.max_connect);
        vm_write32(info_addr + 4, host_info.now_connect);
        vm_write32(info_addr + 8, host_info.system_info);
        for (int i = 0; i < CELL_PAD_MAX_PORT_NUM; i++)
            vm_write32(info_addr + 12 + i * 4, host_info.port_status[i]);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Module definitions and registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static ps3_module mod_cellSysutil;
static ps3_module mod_cellGcmSys;
static ps3_module mod_cellSysmodule;
static ps3_module mod_cellSpurs;
static ps3_module mod_cellAudio;
static ps3_module mod_cellSync;
static ps3_module mod_cellNetCtl;
static ps3_module mod_sceNp;
static ps3_module mod_sys_net;
static ps3_module mod_sys_io;
static ps3_module mod_sys_fs;
static ps3_module mod_sysPrxForUser;

/* ---------------------------------------------------------------------------
 * cellSysutil
 * -----------------------------------------------------------------------*/
static void register_cellSysutil(void)
{
    ps3_module_init(&mod_cellSysutil, "cellSysutil");

    /* Real bridges */
    reg_func(&mod_cellSysutil, "cellSysutilRegisterCallback",   (void*)bridge_cellSysutilRegisterCallback);
    reg_func(&mod_cellSysutil, "cellSysutilUnregisterCallback", (void*)bridge_cellSysutilUnregisterCallback);
    reg_func(&mod_cellSysutil, "cellSysutilCheckCallback",      (void*)bridge_cellSysutilCheckCallback);
    reg_func(&mod_cellSysutil, "cellSysutilGetSystemParamInt",  (void*)bridge_cellSysutilGetSystemParamInt);
    reg_func(&mod_cellSysutil, "cellSysutilGetSystemParamString", (void*)bridge_cellSysutilGetSystemParamString);
    reg_func(&mod_cellSysutil, "cellVideoOutGetState",          (void*)bridge_cellVideoOutGetState);
    reg_func(&mod_cellSysutil, "cellVideoOutGetResolution",     (void*)bridge_cellVideoOutGetResolution);
    reg_func(&mod_cellSysutil, "cellVideoOutConfigure",         (void*)bridge_cellVideoOutConfigure);

    /* Stubs (non-critical for boot) */
    reg_func(&mod_cellSysutil, "cellAudioOutConfigure",            (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellAudioOutGetSoundAvailability", (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellMsgDialogOpen",                (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellMsgDialogOpen2",               (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellMsgDialogAbort",               (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellSaveDataAutoSave",             (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellSaveDataAutoLoad",             (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellSaveDataDelete",               (void*)hle_stub);
    reg_func(&mod_cellSysutil, "cellHddGameCheck",                 (void*)bridge_cellHddGameCheck);

    mod_cellSysutil.loaded = true;
    ps3_register_module(&mod_cellSysutil);
}

/* ---------------------------------------------------------------------------
 * cellGcmSys (REAL bridges for all functions)
 * -----------------------------------------------------------------------*/
static void register_cellGcmSys(void)
{
    ps3_module_init(&mod_cellGcmSys, "cellGcmSys");

    reg_func(&mod_cellGcmSys, "_cellGcmInitBody",          (void*)bridge_cellGcmInitBody);
    reg_func(&mod_cellGcmSys, "cellGcmGetConfiguration",   (void*)bridge_cellGcmGetConfiguration);
    reg_func(&mod_cellGcmSys, "cellGcmGetControlRegister",  (void*)bridge_cellGcmGetControlRegister);
    reg_func(&mod_cellGcmSys, "cellGcmSetFlipMode",         (void*)bridge_cellGcmSetFlipMode);
    reg_func(&mod_cellGcmSys, "cellGcmGetFlipStatus",       (void*)bridge_cellGcmGetFlipStatus);
    reg_func(&mod_cellGcmSys, "cellGcmSetWaitFlip",         (void*)bridge_cellGcmSetWaitFlip);
    reg_func(&mod_cellGcmSys, "cellGcmResetFlipStatus",     (void*)bridge_cellGcmResetFlipStatus);
    reg_func(&mod_cellGcmSys, "cellGcmSetDisplayBuffer",    (void*)bridge_cellGcmSetDisplayBuffer);
    reg_func(&mod_cellGcmSys, "cellGcmAddressToOffset",     (void*)bridge_cellGcmAddressToOffset);
    reg_func(&mod_cellGcmSys, "cellGcmMapMainMemory",       (void*)bridge_cellGcmMapMainMemory);
    reg_func(&mod_cellGcmSys, "cellGcmSetVBlankHandler",    (void*)bridge_cellGcmSetVBlankHandler);
    reg_func(&mod_cellGcmSys, "cellGcmSetTile",             (void*)bridge_cellGcmSetTile);
    reg_func(&mod_cellGcmSys, "cellGcmSetZcull",            (void*)bridge_cellGcmSetZcull);
    reg_func(&mod_cellGcmSys, "cellGcmSetInvalidateTile",   (void*)bridge_cellGcmSetInvalidateTile);
    reg_func(&mod_cellGcmSys, "cellGcmGetLabelAddress",     (void*)bridge_cellGcmGetLabelAddress);
    reg_func(&mod_cellGcmSys, "cellGcmGetTiledPitchSize",   (void*)bridge_cellGcmGetTiledPitchSize);
    reg_func(&mod_cellGcmSys, "cellGcmSetFlip",             (void*)bridge_cellGcmSetFlip);
    reg_func(&mod_cellGcmSys, "_cellGcmFunc15",             (void*)bridge_cellGcmFunc15);

    mod_cellGcmSys.loaded = true;
    ps3_register_module(&mod_cellGcmSys);
}

/* ---------------------------------------------------------------------------
 * cellSysmodule (REAL bridges)
 * -----------------------------------------------------------------------*/
static void register_cellSysmodule(void)
{
    ps3_module_init(&mod_cellSysmodule, "cellSysmodule");
    reg_func(&mod_cellSysmodule, "cellSysmoduleLoadModule",   (void*)bridge_cellSysmoduleLoadModule);
    reg_func(&mod_cellSysmodule, "cellSysmoduleUnloadModule", (void*)bridge_cellSysmoduleUnloadModule);
    reg_func(&mod_cellSysmodule, "cellSysmoduleInitialize",   (void*)hle_stub);
    reg_func(&mod_cellSysmodule, "cellSysmoduleFinalize",     (void*)hle_stub);
    mod_cellSysmodule.loaded = true;
    ps3_register_module(&mod_cellSysmodule);
}

/* ---------------------------------------------------------------------------
 * cellSpurs — SPU task management (stubs for now; SPU execution is N/A)
 * -----------------------------------------------------------------------*/
/* cellSpursInitialize(CellSpurs *spurs, int nSpus, int spuPriority,
 *                     int ppuPriority, bool exitIfNoWork)
 * r3 = pointer to CellSpurs object (2048 bytes, 128-aligned)
 * r4 = nSpus, r5 = spuPriority, r6 = ppuPriority, r7 = exitIfNoWork */
static int64_t hle_cellSpursInitialize(ppu_context* ctx) {
    uint32_t spurs_addr = (uint32_t)ctx->gpr[3];
    uint32_t nSpus = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[cellSpurs] Initialize(spurs=0x%08X, nSpus=%u, spuPrio=%u, ppuPrio=%u)\n",
            spurs_addr, nSpus, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    /* Zero the SPURS structure (2048 bytes = 0x800) so the game
     * doesn't read garbage when checking SPURS state. */
    if (spurs_addr && spurs_addr < 0xF0000000) {
        extern uint8_t* vm_base;
        memset(vm_base + spurs_addr, 0, 0x800);
        /* Mark as initialized (offset 0x0 = magic/state) */
        vm_write32(spurs_addr, 1);
    }
    ctx->gpr[3] = 0;  /* CELL_OK */
    return 0;
}

/* cellSpursAddWorkload — r3=spurs, r4=workloadId_ptr, ... */
static int64_t hle_cellSpursAddWorkload(ppu_context* ctx) {
    uint32_t id_ptr = (uint32_t)ctx->gpr[4];
    static uint32_t s_next_wl = 1;
    fprintf(stderr, "[cellSpurs] AddWorkload(spurs=0x%08X, id_ptr=0x%08X) -> wl=%u\n",
            (uint32_t)ctx->gpr[3], id_ptr, s_next_wl);
    if (id_ptr && id_ptr < 0xF0000000)
        vm_write32(id_ptr, s_next_wl++);
    ctx->gpr[3] = 0;
    return 0;
}

static void register_cellSpurs(void)
{
    ps3_module_init(&mod_cellSpurs, "cellSpurs");
    const char* funcs[] = {
        "cellSpursDetachLv2EventQueue", "cellSpursRemoveWorkload",
        "cellSpursWaitForWorkloadShutdown",
        "cellSpursWakeUp", "cellSpursShutdownWorkload",
        "cellSpursAttachLv2EventQueue",
        "cellSpursFinalize", "cellSpursReadyCountStore",
        "cellSpursRequestIdleSpu", "cellSpursGetInfo",
        "cellSpursSetPriorities", "cellSpursSetExceptionEventHandler",
    };
    for (auto name : funcs)
        reg_func(&mod_cellSpurs, name, (void*)hle_stub);
    reg_func(&mod_cellSpurs, "cellSpursInitialize", (void*)hle_cellSpursInitialize);
    reg_func(&mod_cellSpurs, "cellSpursAddWorkload", (void*)hle_cellSpursAddWorkload);
    mod_cellSpurs.loaded = true;
    ps3_register_module(&mod_cellSpurs);
}

/* ---------------------------------------------------------------------------
 * cellAudio (REAL bridges)
 * -----------------------------------------------------------------------*/
static void register_cellAudio(void)
{
    ps3_module_init(&mod_cellAudio, "cellAudio");
    reg_func(&mod_cellAudio, "cellAudioInit",          (void*)bridge_cellAudioInit);
    reg_func(&mod_cellAudio, "cellAudioQuit",          (void*)bridge_cellAudioQuit);
    reg_func(&mod_cellAudio, "cellAudioPortOpen",      (void*)bridge_cellAudioPortOpen);
    reg_func(&mod_cellAudio, "cellAudioPortClose",     (void*)bridge_cellAudioPortClose);
    reg_func(&mod_cellAudio, "cellAudioPortStart",     (void*)bridge_cellAudioPortStart);
    reg_func(&mod_cellAudio, "cellAudioPortStop",      (void*)bridge_cellAudioPortStop);
    reg_func(&mod_cellAudio, "cellAudioGetPortConfig", (void*)bridge_cellAudioGetPortConfig);
    mod_cellAudio.loaded = true;
    ps3_register_module(&mod_cellAudio);
}

/* ---------------------------------------------------------------------------
 * cellSync (stubs — safe for now, real implementations exist in runtime)
 * -----------------------------------------------------------------------*/
static void register_cellSync(void)
{
    ps3_module_init(&mod_cellSync, "cellSync");
    const char* funcs[] = {
        "cellSyncBarrierInitialize", "cellSyncBarrierWait",
        "cellSyncBarrierTryWait",
    };
    for (auto name : funcs)
        reg_func(&mod_cellSync, name, (void*)hle_stub);
    mod_cellSync.loaded = true;
    ps3_register_module(&mod_cellSync);
}

/* ---------------------------------------------------------------------------
 * cellNetCtl — real bridges to ps3recomp implementation
 * -----------------------------------------------------------------------*/

/* cellNetCtlInit() — no args */
static int64_t bridge_cellNetCtlInit(ppu_context* ctx)
{
    (void)ctx;
    int32_t rc = cellNetCtlInit();
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* cellNetCtlTerm() — no args */
static int64_t bridge_cellNetCtlTerm(ppu_context* ctx)
{
    (void)ctx;
    int32_t rc = cellNetCtlTerm();
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* cellNetCtlGetState(s32* state) — r3 = guest ptr to s32 */
static int64_t bridge_cellNetCtlGetState(ppu_context* ctx)
{
    uint32_t state_addr = (uint32_t)ctx->gpr[3];
    int32_t state = 0;
    int32_t rc = cellNetCtlGetState(&state);
    if (rc == 0 && state_addr) {
        /* Write state as big-endian s32 to guest memory */
        vm_write32(state_addr, (uint32_t)state);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* cellNetCtlGetInfo(s32 code, CellNetCtlInfo* info) — r3=code, r4=guest ptr */
static int64_t bridge_cellNetCtlGetInfo(ppu_context* ctx)
{
    int32_t code = (int32_t)ctx->gpr[3];
    uint32_t info_addr = (uint32_t)ctx->gpr[4];
    CellNetCtlInfo info;
    int32_t rc = cellNetCtlGetInfo(code, &info);
    if (rc == 0 && info_addr) {
        /* Write the info union to guest memory.
         * For u32 fields, byte-swap.  For string/byte fields, copy raw. */
        switch (code) {
        case CELL_NET_CTL_INFO_DEVICE:
        case CELL_NET_CTL_INFO_MTU:
        case CELL_NET_CTL_INFO_LINK:
        case CELL_NET_CTL_INFO_LINK_TYPE:
        case CELL_NET_CTL_INFO_WLAN_SECURITY:
        case CELL_NET_CTL_INFO_8021X_TYPE:
        case CELL_NET_CTL_INFO_IP_CONFIG:
        case CELL_NET_CTL_INFO_HTTP_PROXY_CONFIG:
        case CELL_NET_CTL_INFO_UPNP_CONFIG:
            vm_write32(info_addr, info.device);
            break;
        case CELL_NET_CTL_INFO_HTTP_PROXY_PORT:
            vm_write16(info_addr, info.http_proxy_port);
            break;
        case CELL_NET_CTL_INFO_ETHER_ADDR:
        case CELL_NET_CTL_INFO_BSSID:
            /* 6 bytes MAC + 2 padding — raw copy */
            for (int i = 0; i < 8; i++)
                vm_write8(info_addr + i, info.ether_addr.data[i]);
            break;
        default:
            /* String/byte fields: raw memcpy (already in byte order) */
            for (size_t i = 0; i < sizeof(CellNetCtlInfo); i++)
                vm_write8(info_addr + i, ((uint8_t*)&info)[i]);
            break;
        }
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* cellNetCtlGetNatInfo(CellNetCtlNatInfo* natInfo) — r3=guest ptr */
static int64_t bridge_cellNetCtlGetNatInfo(ppu_context* ctx)
{
    uint32_t nat_addr = (uint32_t)ctx->gpr[3];
    CellNetCtlNatInfo natInfo;
    int32_t rc = cellNetCtlGetNatInfo(&natInfo);
    if (rc == 0 && nat_addr) {
        vm_write32(nat_addr + 0, natInfo.size);
        vm_write32(nat_addr + 4, natInfo.nat_type);
        vm_write32(nat_addr + 8, natInfo.stun_status);
        vm_write32(nat_addr + 12, natInfo.upnp_status);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* cellNetCtlAddHandler(handler, arg, s32* hid) — r3=func, r4=arg, r5=guest ptr */
static int64_t bridge_cellNetCtlAddHandler(ppu_context* ctx)
{
    /* We don't actually call back into guest code, just register and succeed */
    uint32_t hid_addr = (uint32_t)ctx->gpr[5];
    int32_t hid = 0;
    int32_t rc = cellNetCtlAddHandler(NULL, NULL, &hid);
    if (rc == 0 && hid_addr)
        vm_write32(hid_addr, (uint32_t)hid);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

/* cellNetCtlDelHandler(s32 hid) — r3=hid */
static int64_t bridge_cellNetCtlDelHandler(ppu_context* ctx)
{
    int32_t hid = (int32_t)ctx->gpr[3];
    int32_t rc = cellNetCtlDelHandler(hid);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return 0;
}

static void register_cellNetCtl(void)
{
    ps3_module_init(&mod_cellNetCtl, "cellNetCtl");
    reg_func(&mod_cellNetCtl, "cellNetCtlInit",       (void*)bridge_cellNetCtlInit);
    reg_func(&mod_cellNetCtl, "cellNetCtlTerm",       (void*)bridge_cellNetCtlTerm);
    reg_func(&mod_cellNetCtl, "cellNetCtlGetState",   (void*)bridge_cellNetCtlGetState);
    reg_func(&mod_cellNetCtl, "cellNetCtlGetInfo",    (void*)bridge_cellNetCtlGetInfo);
    reg_func(&mod_cellNetCtl, "cellNetCtlGetNatInfo", (void*)bridge_cellNetCtlGetNatInfo);
    reg_func(&mod_cellNetCtl, "cellNetCtlAddHandler", (void*)bridge_cellNetCtlAddHandler);
    reg_func(&mod_cellNetCtl, "cellNetCtlDelHandler", (void*)bridge_cellNetCtlDelHandler);
    reg_func(&mod_cellNetCtl, "cellNetCtlNetStartDialogLoadAsync",   (void*)hle_stub);
    reg_func(&mod_cellNetCtl, "cellNetCtlNetStartDialogUnloadAsync", (void*)hle_stub);
    mod_cellNetCtl.loaded = true;
    ps3_register_module(&mod_cellNetCtl);
}

/* ---------------------------------------------------------------------------
 * sceNp (stubs — NP/PSN not critical)
 * -----------------------------------------------------------------------*/
static void register_sceNp(void)
{
    ps3_module_init(&mod_sceNp, "sceNp");
    const char* funcs[] = {
        "sceNpManagerGetTicket", "sceNpTerm",
        "sceNpManagerRequestTicket", "sceNpManagerGetStatus",
        "sceNpBasicRegisterHandler", "sceNpInit",
        "sceNpUtilCmpNpId", "sceNpBasicGetEvent",
        "sceNpManagerGetNpId", "sceNpManagerRegisterCallback",
    };
    for (auto name : funcs)
        reg_func(&mod_sceNp, name, (void*)hle_stub);
    mod_sceNp.loaded = true;
    ps3_register_module(&mod_sceNp);
}

/* ---------------------------------------------------------------------------
 * sys_net (stubs — networking not critical for boot)
 * -----------------------------------------------------------------------*/
static void register_sys_net(void)
{
    ps3_module_init(&mod_sys_net, "sys_net");
    const char* funcs[] = {
        "socketpoll", "sys_net_initialize_network_ex", "getsockname",
        "recvfrom", "listen", "socketselect", "getsockopt",
        "_sys_net_errno_loc",
        "connect", "socketclose", "gethostbyname",
        "setsockopt", "sendto", "socket", "shutdown",
        "inet_aton", "bind", "sys_net_finalize_network",
        "accept", "send", "recv",
    };
    for (auto name : funcs)
        reg_func(&mod_sys_net, name, (void*)hle_stub);
    mod_sys_net.loaded = true;
    ps3_register_module(&mod_sys_net);
}

/* ---------------------------------------------------------------------------
 * sys_io — input devices (REAL bridges for cellPad, stubs for kb/mouse)
 * -----------------------------------------------------------------------*/
static void register_sys_io(void)
{
    ps3_module_init(&mod_sys_io, "sys_io");

    /* Real cellPad bridges */
    reg_func(&mod_sys_io, "cellPadInit",     (void*)bridge_cellPadInit);
    reg_func(&mod_sys_io, "cellPadEnd",      (void*)bridge_cellPadEnd);
    reg_func(&mod_sys_io, "cellPadGetData",  (void*)bridge_cellPadGetData);
    reg_func(&mod_sys_io, "cellPadGetInfo",  (void*)bridge_cellPadGetInfo);
    reg_func(&mod_sys_io, "cellPadGetRawData",     (void*)hle_stub);
    reg_func(&mod_sys_io, "cellPadInfoSensorMode", (void*)hle_stub);
    reg_func(&mod_sys_io, "cellPadSetSensorMode",  (void*)hle_stub);

    /* Keyboard/Mouse stubs */
    const char* kb_mouse[] = {
        "cellKbClearBuf", "cellKbGetInfo", "cellKbInit", "cellKbEnd",
        "cellKbSetCodeType", "cellKbRead",
        "cellMouseGetData", "cellMouseGetInfo", "cellMouseInit", "cellMouseEnd",
    };
    for (auto name : kb_mouse)
        reg_func(&mod_sys_io, name, (void*)hle_stub);

    mod_sys_io.loaded = true;
    ps3_register_module(&mod_sys_io);
}

/* ---------------------------------------------------------------------------
 * sys_fs — filesystem (REAL bridges)
 * -----------------------------------------------------------------------*/
static void register_sys_fs(void)
{
    ps3_module_init(&mod_sys_fs, "sys_fs");

    reg_func(&mod_sys_fs, "cellFsOpen",        (void*)bridge_cellFsOpen);
    reg_func(&mod_sys_fs, "cellFsClose",       (void*)bridge_cellFsClose);
    reg_func(&mod_sys_fs, "cellFsRead",        (void*)bridge_cellFsRead);
    reg_func(&mod_sys_fs, "cellFsWrite",       (void*)bridge_cellFsWrite);
    reg_func(&mod_sys_fs, "cellFsLseek",       (void*)bridge_cellFsLseek);
    reg_func(&mod_sys_fs, "cellFsFstat",       (void*)bridge_cellFsFstat);
    reg_func(&mod_sys_fs, "cellFsOpendir",     (void*)bridge_cellFsOpendir);
    reg_func(&mod_sys_fs, "cellFsReaddir",     (void*)bridge_cellFsReaddir);
    reg_func(&mod_sys_fs, "cellFsClosedir",    (void*)bridge_cellFsClosedir);
    reg_func(&mod_sys_fs, "cellFsUnlink",      (void*)bridge_cellFsUnlink);
    reg_func(&mod_sys_fs, "cellFsGetFreeSize", (void*)bridge_cellFsGetFreeSize);

    mod_sys_fs.loaded = true;
    ps3_register_module(&mod_sys_fs);
}

/* exitspawn: PS3 replaces the process with a new one.
 * In recomp, we treat this as "restart game main". */
extern jmp_buf g_abort_jmp;
extern int g_abort_redirect;

static int s_exitspawn_count = 0;

static int64_t bridge_exitspawn(ppu_context* ctx)
{
    s_exitspawn_count++;
    uint32_t path_addr = (uint32_t)ctx->gpr[3];
    char path[256] = {};
    if (path_addr) {
        for (int i = 0; i < 255; i++) {
            uint8_t c = vm_read8(path_addr + i);
            path[i] = (char)c;
            if (!c) break;
        }
    }
    fprintf(stderr, "\n[HLE] sys_prx_exitspawn_with_level (call #%d)\n", s_exitspawn_count);
    fprintf(stderr, "[HLE]   path='%s'\n", path);

    /* Read argv[0] if available */
    uint32_t argv_addr = (uint32_t)ctx->gpr[4];
    if (argv_addr) {
        uint32_t arg0_ptr = vm_read32(argv_addr);
        if (arg0_ptr) {
            char arg0[128] = {};
            for (int i = 0; i < 127; i++) {
                uint8_t c = vm_read8(arg0_ptr + i);
                arg0[i] = (char)c;
                if (!c) break;
            }
            fprintf(stderr, "[HLE]   argv[0]='%s'\n", arg0);
        }
    }

    fprintf(stderr, "[HLE]   Ignoring exitspawn — returning to caller\n\n");
    fflush(stderr);

    /* Don't restart: just return CELL_OK so the caller continues.
     * The game may have fallback code after the exitspawn call. */
    ctx->gpr[3] = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * sysPrxForUser — core PRX runtime (REAL bridges for critical functions)
 * -----------------------------------------------------------------------*/
static void register_sysPrxForUser(void)
{
    ps3_module_init(&mod_sysPrxForUser, "sysPrxForUser");

    /* Real implementations */
    reg_func(&mod_sysPrxForUser, "sys_initialize_tls",      (void*)bridge_sys_initialize_tls);
    reg_func(&mod_sysPrxForUser, "sys_process_exit",         (void*)bridge_sys_process_exit);
    reg_func(&mod_sysPrxForUser, "sys_time_get_system_time", (void*)bridge_sys_time_get_system_time);
    reg_func(&mod_sysPrxForUser, "sys_ppu_thread_get_id",    (void*)bridge_sys_ppu_thread_get_id);
    reg_func(&mod_sysPrxForUser, "sys_ppu_thread_create",    (void*)bridge_sys_ppu_thread_create);
    reg_func(&mod_sysPrxForUser, "sys_ppu_thread_exit",      (void*)bridge_sys_ppu_thread_exit);

    /* Real lightweight mutex bridges (critical for CRT startup) */
    reg_func(&mod_sysPrxForUser, "sys_lwmutex_create",  (void*)bridge_sys_lwmutex_create);
    reg_func(&mod_sysPrxForUser, "sys_lwmutex_lock",    (void*)bridge_sys_lwmutex_lock);
    reg_func(&mod_sysPrxForUser, "sys_lwmutex_trylock", (void*)bridge_sys_lwmutex_trylock);
    reg_func(&mod_sysPrxForUser, "sys_lwmutex_unlock",  (void*)bridge_sys_lwmutex_unlock);
    reg_func(&mod_sysPrxForUser, "sys_lwmutex_destroy", (void*)bridge_sys_lwmutex_destroy);

    /* Remaining stubs */
    reg_func(&mod_sysPrxForUser, "sys_process_is_stack",              (void*)hle_stub); /* returns 0 = not stack */
    reg_func(&mod_sysPrxForUser, "sys_prx_exitspawn_with_level",     (void*)bridge_exitspawn);
    reg_func(&mod_sysPrxForUser, "sys_spu_image_import",             (void*)hle_stub);
    reg_func(&mod_sysPrxForUser, "sys_game_process_exitspawn",       (void*)bridge_exitspawn);

    mod_sysPrxForUser.loaded = true;
    ps3_register_module(&mod_sysPrxForUser);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: register all HLE modules
 * ═══════════════════════════════════════════════════════════════════════════ */

extern "C" void flow_register_hle_modules(void)
{
    printf("[init] Registering HLE modules...\n");

    /* Initialize filesystem path translation */
    cellfs_set_root_path(FLOW_GAME_DIR);

    /* Address-layout dump — helps diagnose the host-BSS corruption that
     * zeroes mod_cellSysutil mid-run. Print the module addresses and the
     * deltas from a couple of likely culprit symbols (vm_base, the
     * dispatch table) so we can correlate with stray write addresses. */
    fprintf(stderr, "[HLE-LAYOUT] mod_cellSysutil   = %p\n", (void*)&mod_cellSysutil);
    fprintf(stderr, "[HLE-LAYOUT] mod_cellGcmSys    = %p (delta=%+lld)\n", (void*)&mod_cellGcmSys,
            (long long)((char*)&mod_cellGcmSys - (char*)&mod_cellSysutil));
    fprintf(stderr, "[HLE-LAYOUT] mod_cellSysmodule = %p (delta=%+lld)\n", (void*)&mod_cellSysmodule,
            (long long)((char*)&mod_cellSysmodule - (char*)&mod_cellSysutil));
    fflush(stderr);

    register_cellSysutil();
    register_cellGcmSys();
    register_cellSysmodule();
    register_cellSpurs();
    register_cellAudio();
    register_cellSync();
    register_cellNetCtl();
    register_sceNp();
    register_sys_net();
    register_sys_io();
    register_sys_fs();
    register_sysPrxForUser();

    printf("[init] Registered %u HLE modules\n", g_ps3_module_registry.count);
    printf("[init]   Real bridges: cellSysutil, cellGcmSys, cellSysmodule, cellAudio,\n");
    printf("[init]                 cellPad, cellFs, sysPrxForUser (lwmutex, threads)\n");
    printf("[init]   Stubs:        cellSpurs, cellSync, cellNetCtl, sceNp, sys_net\n");
}

/* Self-heal: re-initialise any module struct in the registry whose
 * host memory has been zeroed mid-run. Walks the existing registry,
 * finds slots with NULL name, and replays the matching register_xxx().
 * The registry slot's pointer is left unchanged so we don't push a
 * duplicate entry — only the underlying struct is rebuilt in place. */
extern "C" int flow_repair_hle_modules(void)
{
    int repaired = 0;
    for (uint32_t i = 0; i < g_ps3_module_registry.count; i++) {
        ps3_module* m = g_ps3_module_registry.modules[i];
        if (m && m->name != NULL) continue;
        /* Log the offset between the wiped struct and a known sibling
         * — gives a hint at the corruption pattern. */
        fprintf(stderr, "[HLE-REPAIR] Detected wipe at %p (slot %u). First 32 bytes:",
                (void*)m, i);
        for (int b = 0; b < 32; b++)
            fprintf(stderr, " %02X", ((uint8_t*)m)[b]);
        fprintf(stderr, "\n"); fflush(stderr);
        /* m->name is NULL — struct was wiped. Identify by address and
         * re-init. We only have an address here, not the original name,
         * so map by pointer to the static module symbol. */
        ps3_module* prev_count_save = NULL;
        uint32_t saved_count = g_ps3_module_registry.count;
        if      (m == &mod_cellSysutil)  register_cellSysutil();
        else if (m == &mod_cellGcmSys)   register_cellGcmSys();
        else if (m == &mod_cellSysmodule) register_cellSysmodule();
        else if (m == &mod_cellSpurs)    register_cellSpurs();
        else if (m == &mod_cellAudio)    register_cellAudio();
        else if (m == &mod_cellSync)     register_cellSync();
        else if (m == &mod_cellNetCtl)   register_cellNetCtl();
        else if (m == &mod_sceNp)        register_sceNp();
        else if (m == &mod_sys_net)      register_sys_net();
        else if (m == &mod_sys_io)       register_sys_io();
        else if (m == &mod_sys_fs)       register_sys_fs();
        else if (m == &mod_sysPrxForUser) register_sysPrxForUser();
        else continue;
        /* register_xxx() called ps3_register_module() which appended a
         * new pointer to the same struct — rewind so we don't grow. */
        g_ps3_module_registry.count = saved_count;
        (void)prev_count_save;
        fprintf(stderr, "[HLE-REPAIR] Re-registered module slot %u ('%s' funcs=%u)\n",
                i, m->name ? m->name : "?", m->func_table.count);
        fflush(stderr);
        repaired++;
    }
    return repaired;
}
