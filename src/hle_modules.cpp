/*
 * flOw HLE module registration
 *
 * Registers all PS3 library modules used by flOw with their NID→handler
 * mappings. Each handler calls the runtime's HLE implementation.
 *
 * Libraries used by flOw:
 *   cellSysutil, cellGcmSys, cellSysmodule, cellSpurs, cellAudio,
 *   cellSync, cellNetCtl, sceNp, sys_net, sys_io, sys_fs, sysPrxForUser
 */

#include <ps3emu/module.h>
#include <ps3emu/nid.h>
#include <cstdio>

/* Runtime HLE headers */
#include "runtime/ppu/ppu_context.h"

/* Include lib headers for HLE function declarations */
/* Note: headers may not exist for all modules; we use stubs for unimplemented ones */

/* ---------------------------------------------------------------------------
 * Helper: register a function NID computed from name
 * Uses ps3_compute_nid() from the runtime (now fixed to use correct
 * 16-byte suffix + little-endian byte order).
 * -----------------------------------------------------------------------*/
static void reg_func(ps3_module* m, const char* name, void* handler)
{
    uint32_t nid = ps3_compute_nid(name);
    ps3_nid_table_add(&m->func_table, nid, name, handler);
}

/* ---------------------------------------------------------------------------
 * Stub handler for not-yet-implemented HLE functions.
 * Returns CELL_OK (0).
 * -----------------------------------------------------------------------*/
static int64_t hle_stub(ppu_context* ctx)
{
    (void)ctx;
    return 0; /* CELL_OK */
}

/* ---------------------------------------------------------------------------
 * Real HLE handlers for critical functions
 * -----------------------------------------------------------------------*/

#include "runtime/memory/vm.h"

/* sys_initialize_tls(tls_addr, tls_filesize, tls_memsize, tls_align)
 * Sets up thread-local storage. We allocate a TLS block in guest memory
 * and point r13 to it (SDA2 base per PPC64 ABI). */
static int64_t hle_sys_initialize_tls(ppu_context* ctx)
{
    uint32_t tls_addr = (uint32_t)ctx->gpr[3];
    uint32_t tls_filesz = (uint32_t)ctx->gpr[4];
    uint32_t tls_memsz = (uint32_t)ctx->gpr[5];
    uint32_t tls_align = (uint32_t)ctx->gpr[6];

    fprintf(stderr, "[HLE] sys_initialize_tls(addr=0x%x, filesz=0x%x, memsz=0x%x, align=%u)\n",
            tls_addr, tls_filesz, tls_memsz, tls_align);

    /* Allocate TLS block in guest memory (use a fixed address in main RAM) */
    static uint32_t tls_alloc_ptr = 0x0F000000; /* near end of main mem */
    uint32_t tls_base = tls_alloc_ptr;
    if (tls_align > 0)
        tls_base = (tls_base + tls_align - 1) & ~(tls_align - 1);

    /* Copy TLS template data */
    if (tls_filesz > 0 && tls_addr != 0) {
        memcpy(vm_base + tls_base, vm_base + tls_addr, tls_filesz);
    }
    /* Zero BSS portion */
    if (tls_memsz > tls_filesz) {
        memset(vm_base + tls_base + tls_filesz, 0, tls_memsz - tls_filesz);
    }

    tls_alloc_ptr = tls_base + tls_memsz + 0x1000; /* advance for next thread */

    /* Set r13 to TLS base + 0x7000 (PPC64 TLS ABI convention) */
    ctx->gpr[13] = tls_base + 0x7000;

    fprintf(stderr, "[HLE] TLS block at 0x%x, r13 = 0x%llx\n",
            tls_base, (unsigned long long)ctx->gpr[13]);
    return 0;
}

/* sys_process_exit(status) - terminate the process */
static int64_t hle_sys_process_exit(ppu_context* ctx)
{
    int32_t status = (int32_t)ctx->gpr[3];
    fprintf(stderr, "[HLE] sys_process_exit(%d)\n", status);
    exit(status);
    return 0;
}

/* sys_time_get_system_time() - returns microseconds since epoch */
static int64_t hle_sys_time_get_system_time(ppu_context* ctx)
{
    /* Return a fake but increasing value */
    static uint64_t fake_time = 1000000;
    fake_time += 16667; /* ~60fps frame time */
    ctx->gpr[3] = fake_time;
    return 0;
}

/* sys_ppu_thread_get_id(thread_id_ptr) */
static int64_t hle_sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t ptr = (uint32_t)ctx->gpr[3];
    /* Write a fake thread ID = 1 */
    if (ptr && vm_base) {
        uint32_t tid = 1;
        uint32_t tid_be = _byteswap_ulong(tid);
        memcpy(vm_base + ptr, &tid_be, 4);
    }
    ctx->gpr[3] = 0; /* CELL_OK */
    return 0;
}

/* cellSysmoduleLoadModule(id) - just return CELL_OK */
static int64_t hle_cellSysmoduleLoadModule(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[HLE] cellSysmoduleLoadModule(%u)\n", id);
    return 0;
}

/* cellSysmoduleUnloadModule(id) */
static int64_t hle_cellSysmoduleUnloadModule(ppu_context* ctx)
{
    (void)ctx;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Module definitions
 * -----------------------------------------------------------------------*/

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
 * Registration helpers per module
 * -----------------------------------------------------------------------*/

static void register_cellSysutil(void)
{
    ps3_module_init(&mod_cellSysutil, "cellSysutil");
    const char* funcs[] = {
        "cellSysutilRegisterCallback", "cellSysutilUnregisterCallback",
        "cellSysutilCheckCallback", "cellSysutilGetSystemParamInt",
        "cellSysutilGetSystemParamString",
        "cellVideoOutGetState", "cellVideoOutGetResolution",
        "cellVideoOutConfigure", "cellAudioOutConfigure",
        "cellAudioOutGetSoundAvailability",
        "cellMsgDialogOpen", "cellMsgDialogOpen2", "cellMsgDialogAbort",
        "cellSaveDataAutoSave", "cellSaveDataAutoLoad", "cellSaveDataDelete",
        "cellHddGameCheck",
    };
    for (auto name : funcs)
        reg_func(&mod_cellSysutil, name, (void*)hle_stub);
    mod_cellSysutil.loaded = true;
    ps3_register_module(&mod_cellSysutil);
}

static void register_cellGcmSys(void)
{
    ps3_module_init(&mod_cellGcmSys, "cellGcmSys");
    const char* funcs[] = {
        "cellGcmGetTiledPitchSize", "_cellGcmInitBody",
        "cellGcmAddressToOffset", "_cellGcmFunc15",
        "cellGcmSetFlipMode", "cellGcmGetFlipStatus",
        "cellGcmSetWaitFlip", "cellGcmMapMainMemory",
        "cellGcmSetDisplayBuffer", "cellGcmGetControlRegister",
        "cellGcmSetVBlankHandler", "cellGcmResetFlipStatus",
        "cellGcmSetInvalidateTile", "cellGcmSetTile",
        "cellGcmSetZcull", "cellGcmSetFlip",
        "cellGcmGetConfiguration", "cellGcmGetLabelAddress",
    };
    for (auto name : funcs)
        reg_func(&mod_cellGcmSys, name, (void*)hle_stub);
    mod_cellGcmSys.loaded = true;
    ps3_register_module(&mod_cellGcmSys);
}

static void register_cellSysmodule(void)
{
    ps3_module_init(&mod_cellSysmodule, "cellSysmodule");
    reg_func(&mod_cellSysmodule, "cellSysmoduleLoadModule", (void*)hle_cellSysmoduleLoadModule);
    reg_func(&mod_cellSysmodule, "cellSysmoduleUnloadModule", (void*)hle_cellSysmoduleUnloadModule);
    reg_func(&mod_cellSysmodule, "cellSysmoduleInitialize", (void*)hle_stub);
    reg_func(&mod_cellSysmodule, "cellSysmoduleFinalize", (void*)hle_stub);
    mod_cellSysmodule.loaded = true;
    ps3_register_module(&mod_cellSysmodule);
}

static void register_cellSpurs(void)
{
    ps3_module_init(&mod_cellSpurs, "cellSpurs");
    const char* funcs[] = {
        "cellSpursDetachLv2EventQueue", "cellSpursRemoveWorkload",
        "cellSpursWaitForWorkloadShutdown", "cellSpursAddWorkload",
        "cellSpursWakeUp", "cellSpursShutdownWorkload",
        "cellSpursInitialize", "cellSpursAttachLv2EventQueue",
        "cellSpursFinalize", "cellSpursReadyCountStore",
        "cellSpursRequestIdleSpu", "cellSpursGetInfo",
        "cellSpursSetPriorities", "cellSpursSetExceptionEventHandler",
    };
    for (auto name : funcs)
        reg_func(&mod_cellSpurs, name, (void*)hle_stub);
    mod_cellSpurs.loaded = true;
    ps3_register_module(&mod_cellSpurs);
}

static void register_cellAudio(void)
{
    ps3_module_init(&mod_cellAudio, "cellAudio");
    const char* funcs[] = {
        "cellAudioInit", "cellAudioPortClose", "cellAudioPortStop",
        "cellAudioGetPortConfig", "cellAudioPortStart",
        "cellAudioQuit", "cellAudioPortOpen",
    };
    for (auto name : funcs)
        reg_func(&mod_cellAudio, name, (void*)hle_stub);
    mod_cellAudio.loaded = true;
    ps3_register_module(&mod_cellAudio);
}

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

static void register_cellNetCtl(void)
{
    ps3_module_init(&mod_cellNetCtl, "cellNetCtl");
    const char* funcs[] = {
        "cellNetCtlNetStartDialogLoadAsync",
        "cellNetCtlNetStartDialogUnloadAsync",
        "cellNetCtlTerm", "cellNetCtlGetInfo", "cellNetCtlInit",
    };
    for (auto name : funcs)
        reg_func(&mod_cellNetCtl, name, (void*)hle_stub);
    mod_cellNetCtl.loaded = true;
    ps3_register_module(&mod_cellNetCtl);
}

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

static void register_sys_io(void)
{
    ps3_module_init(&mod_sys_io, "sys_io");
    const char* funcs[] = {
        "cellPadInit", "cellKbClearBuf", "cellKbGetInfo",
        "cellMouseGetData", "cellPadGetInfo", "cellPadGetRawData",
        "cellKbInit", "cellPadEnd", "cellMouseGetInfo",
        "cellPadInfoSensorMode", "cellPadGetData",
        "cellKbSetCodeType", "cellPadSetSensorMode",
        "cellKbEnd", "cellMouseInit", "cellMouseEnd", "cellKbRead",
    };
    for (auto name : funcs)
        reg_func(&mod_sys_io, name, (void*)hle_stub);
    mod_sys_io.loaded = true;
    ps3_register_module(&mod_sys_io);
}

static void register_sys_fs(void)
{
    ps3_module_init(&mod_sys_fs, "sys_fs");
    const char* funcs[] = {
        "cellFsClose", "cellFsOpendir", "cellFsRead",
        "cellFsReaddir", "cellFsOpen", "cellFsUnlink",
        "cellFsLseek", "cellFsGetFreeSize", "cellFsWrite",
        "cellFsFstat", "cellFsClosedir",
    };
    for (auto name : funcs)
        reg_func(&mod_sys_fs, name, (void*)hle_stub);
    mod_sys_fs.loaded = true;
    ps3_register_module(&mod_sys_fs);
}

static void register_sysPrxForUser(void)
{
    ps3_module_init(&mod_sysPrxForUser, "sysPrxForUser");

    /* Real implementations */
    reg_func(&mod_sysPrxForUser, "sys_initialize_tls", (void*)hle_sys_initialize_tls);
    reg_func(&mod_sysPrxForUser, "sys_process_exit", (void*)hle_sys_process_exit);
    reg_func(&mod_sysPrxForUser, "sys_time_get_system_time", (void*)hle_sys_time_get_system_time);
    reg_func(&mod_sysPrxForUser, "sys_ppu_thread_get_id", (void*)hle_sys_ppu_thread_get_id);

    /* Stubs for the rest */
    const char* stub_funcs[] = {
        "sys_lwmutex_lock", "sys_lwmutex_unlock",
        "sys_ppu_thread_create", "sys_lwmutex_create",
        "sys_process_is_stack",
        "sys_prx_exitspawn_with_level", "sys_lwmutex_trylock",
        "sys_ppu_thread_exit", "sys_lwmutex_destroy",
        "sys_spu_image_import", "sys_game_process_exitspawn",
    };
    for (auto name : stub_funcs)
        reg_func(&mod_sysPrxForUser, name, (void*)hle_stub);

    mod_sysPrxForUser.loaded = true;
    ps3_register_module(&mod_sysPrxForUser);
}

/* ---------------------------------------------------------------------------
 * Public: register all HLE modules
 * -----------------------------------------------------------------------*/

extern "C" void flow_register_hle_modules(void)
{
    printf("[init] Registering HLE modules...\n");

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

    printf("[init] Registered %u modules with %d total NID handlers\n",
           g_ps3_module_registry.count, 140);
}
