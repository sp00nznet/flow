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
    fflush(stderr);

    /* During CRT or game main, intercept exit and redirect */
    if (g_abort_redirect == 0) {
        /* CRT phase: redirect to game main */
        g_abort_redirect = 1;
        fprintf(stderr, "[HLE] CRT abort intercepted — longjmp to main\n");
        fflush(stderr);
        longjmp(g_abort_jmp, 1);
    }

    /* Game main phase: ignore first few exit(1) calls from assertion handler.
     * The assertions fire on null PhyreEngine pointers but the game may
     * still be able to proceed past them to reach GCM init. */
    if (g_abort_redirect >= 2) {
        static int s_exit_ignored = 0;
        s_exit_ignored++;
        if (s_exit_ignored <= 5 && status != 0) {
            fprintf(stderr, "[HLE] Ignoring sys_process_exit(%d) #%d — continuing\n",
                    status, s_exit_ignored);
            fflush(stderr);
            ctx->gpr[3] = 0;
            return 0; /* return to caller */
        }
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

/* cellSysutilRegisterCallback(slot, func, userdata) */
static int64_t bridge_cellSysutilRegisterCallback(ppu_context* ctx)
{
    s32 slot         = (s32)ctx->gpr[3];
    /* func and userdata are guest addresses; store as-is for callback dispatch */
    uint32_t func    = (uint32_t)ctx->gpr[4];
    uint32_t userdata = (uint32_t)ctx->gpr[5];

    fprintf(stderr, "[HLE] cellSysutilRegisterCallback(slot=%d, func=0x%x, TOC=0x%llX)\n",
            slot, func, (unsigned long long)ctx->gpr[2]);

    /* Call real implementation with cast pointers (it just stores them) */
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

/* _cellGcmInitBody(cmdSize, ioSize, ioAddress) — maps to cellGcmInit */
/* Forward declarations for RSX command processor */
extern "C" {
    void rsx_state_init(void*);       /* from rsx_commands.c */
    extern void* rsx_get_backend(void); /* from rsx_commands.c */
    void hle_guest_malloc(ppu_context* ctx); /* from malloc_override.cpp */
}

static int64_t bridge_cellGcmInitBody(ppu_context* ctx)
{
    uint32_t cmdSize   = (uint32_t)ctx->gpr[3];
    uint32_t ioSize    = (uint32_t)ctx->gpr[4];
    uint32_t ioAddress = (uint32_t)ctx->gpr[5];

    fprintf(stderr, "[HLE] _cellGcmInitBody(cmdSize=0x%x, ioSize=0x%x, ioAddr=0x%x)\n",
            cmdSize, ioSize, ioAddress);

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

/* Guest address for the GCM control register mirror */
static uint32_t g_gcm_control_guest = 0;

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
            /* Initialize: put=0, get=0, ref=0 */
            vm_write32(g_gcm_control_guest + 0, 0); /* put */
            vm_write32(g_gcm_control_guest + 4, 0); /* get */
            vm_write32(g_gcm_control_guest + 8, 0); /* ref */
            fprintf(stderr, "[HLE] cellGcmGetControlRegister -> 0x%08X\n",
                    g_gcm_control_guest);
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

    if (rc == CELL_OK && offset_addr)
        vm_write32(offset_addr, host_offset);

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}

/* cellGcmMapMainMemory(ea, size, offset_ptr) */
static int64_t bridge_cellGcmMapMainMemory(ppu_context* ctx)
{
    uint32_t ea          = (uint32_t)ctx->gpr[3];
    uint32_t size        = (uint32_t)ctx->gpr[4];
    uint32_t offset_addr = (uint32_t)ctx->gpr[5];

    uint32_t host_offset = 0;
    s32 rc = cellGcmMapMainMemory(ea, size, &host_offset);

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

/* cellGcmSetFlip(bufferId) */
static int64_t bridge_cellGcmSetFlip(ppu_context* ctx)
{
    s32 rc = cellGcmSetFlipCommand((uint32_t)ctx->gpr[3]);
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
    reg_func(&mod_cellSysutil, "cellHddGameCheck",                 (void*)hle_stub);

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
