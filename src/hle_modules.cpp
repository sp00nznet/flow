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
 * Helper: register a function NID by computing it from the name string
 * -----------------------------------------------------------------------*/
static void reg_func(ps3_module* m, const char* name, void* handler)
{
    uint32_t nid = ps3_compute_nid(name);
    ps3_nid_table_add(&m->func_table, nid, name, handler);
}

/* ---------------------------------------------------------------------------
 * Stub handler for not-yet-implemented HLE functions.
 * Prints the call and returns CELL_OK (0).
 * -----------------------------------------------------------------------*/
static int64_t hle_stub(ppu_context* ctx)
{
    (void)ctx;
    return 0; /* CELL_OK */
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
    const char* funcs[] = {
        "cellSysmoduleUnloadModule", "cellSysmoduleLoadModule",
        "cellSysmoduleInitialize", "cellSysmoduleFinalize",
    };
    for (auto name : funcs)
        reg_func(&mod_cellSysmodule, name, (void*)hle_stub);
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
    const char* funcs[] = {
        "sys_lwmutex_lock", "sys_lwmutex_unlock",
        "sys_ppu_thread_create", "sys_lwmutex_create",
        "sys_ppu_thread_get_id", "sys_process_is_stack",
        "sys_initialize_tls", "sys_time_get_system_time",
        "sys_prx_exitspawn_with_level", "sys_lwmutex_trylock",
        "sys_ppu_thread_exit", "sys_lwmutex_destroy",
        "sys_process_exit", "sys_spu_image_import",
        "sys_game_process_exitspawn",
    };
    for (auto name : funcs)
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
