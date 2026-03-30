/* Auto-generated import stub bridges */
/* Each stub resolves its NID through the runtime module system */
#include "ppu_recomp.h"
#include "ps3emu/module.h"
#include <stdio.h>

/* NID dispatch: look up handler and call it.
 *
 * IMPORTANT: Save current TOC (r2) to the caller's expected stack location
 * before calling the handler. The PPC64 ELF ABI requires callers to restore
 * TOC from sp+40 after inter-module calls. The lifter doesn't emit the
 * 'std r2, 40(r1)' instruction that the PS3 PLT stubs would normally do,
 * so we do it here to prevent the caller's TOC restore from reading garbage. */
static void nid_dispatch(ppu_context* ctx, uint32_t nid, const char* name) {
    /* Save TOC to caller's stack frame at SP+0x28 (decimal 40) per PPC64 ABI */
    uint64_t saved_toc = ctx->gpr[2];
    vm_write64((uint32_t)ctx->gpr[1] + 0x28, saved_toc);

    /* Also save LR — some callers restore LR from the stack after import calls */
    vm_write64((uint32_t)ctx->gpr[1] + 0x10, ctx->lr);

    void* handler = ps3_resolve_func_nid(nid);
    if (handler) {
        fprintf(stderr, "[HLE] %s\n", name);
        ((int64_t(*)(ppu_context*))handler)(ctx);
    } else {
        fprintf(stderr, "[HLE] UNIMPLEMENTED: %s (NID 0x%08x)\n", name, nid);
    }

    /* Restore TOC — the calling code should do this via ld r2, 40(r1)
     * but if trampolines skip the restore instruction, TOC stays corrupted.
     * Force TOC to the game's known value since flOw is single-module. */
    ctx->gpr[2] = 0x008969A8;
}

/* cellSysutil::cellVideoOutConfigure */
void func_008164DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x0bae8772, "cellVideoOutConfigure");
}

/* cellSysutil::cellSysutilCheckCallback */
void func_008164FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x189a74da, "cellSysutilCheckCallback");
}

/* cellSysutil::cellSysutilGetSystemParamInt */
void func_0081651C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x40e895d3, "cellSysutilGetSystemParamInt");
}

/* cellSysutil::cellAudioOutConfigure */
void func_0081653C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4692ab35, "cellAudioOutConfigure");
}

/* cellSysutil::cellMsgDialogAbort */
void func_0081655C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x62b0f803, "cellMsgDialogAbort");
}

/* cellSysutil::cellMsgDialogOpen2 */
void func_0081657C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x7603d3db, "cellMsgDialogOpen2");
}

/* cellSysutil::cellVideoOutGetState */
void func_0081659C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x887572d5, "cellVideoOutGetState");
}

/* cellSysutil::cellHddGameCheck */
void func_008165BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x9117df20, "cellHddGameCheck");
}

/* cellSysutil::cellSysutilRegisterCallback */
void func_008165DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x9d98afa0, "cellSysutilRegisterCallback");
}

/* cellSysutil::cellSaveDataDelete */
void func_008165FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa4ed7dfe, "cellSaveDataDelete");
}

/* cellSysutil::cellAudioOutGetSoundAvailability */
void func_0081661C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xc01b4e7c, "cellAudioOutGetSoundAvailability");
}

/* cellSysutil::cellSaveDataAutoLoad */
void func_0081663C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xc22c79b5, "cellSaveDataAutoLoad");
}

/* cellSysutil::cellVideoOutGetResolution */
void func_0081665C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xe558748d, "cellVideoOutGetResolution");
}

/* cellSysutil::cellMsgDialogOpen */
void func_0081667C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xf81eca25, "cellMsgDialogOpen");
}

/* cellSysutil::cellSaveDataAutoSave */
void func_0081669C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xf8a175ec, "cellSaveDataAutoSave");
}

/* sys_net::socketpoll */
void func_008166BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x051ee3ee, "socketpoll");
}

/* sys_net::sys_net_initialize_network_ex */
void func_008166DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x139a9e9b, "sys_net_initialize_network_ex");
}

/* sys_net::getsockname */
void func_008166FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x13efe7f5, "getsockname");
}

/* sys_net::recvfrom */
void func_0081671C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x1f953b9f, "recvfrom");
}

/* sys_net::listen */
void func_0081673C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x28e208bb, "listen");
}

/* sys_net::socketselect */
void func_0081675C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x3f09e20a, "socketselect");
}

/* sys_net::getsockopt */
void func_0081677C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x5a045bd1, "getsockopt");
}

/* sys_net::_sys_net_errno_loc */
void func_0081679C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x6005cde1, "_sys_net_errno_loc");
}

/* sys_net::connect */
void func_008167BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x64f66d35, "connect");
}

/* sys_net::socketclose */
void func_008167DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x6db6e8cd, "socketclose");
}

/* sys_net::gethostbyname */
void func_008167FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x71f4c717, "gethostbyname");
}

/* sys_net::setsockopt */
void func_0081681C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x88f03575, "setsockopt");
}

/* sys_net::sendto */
void func_0081683C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x9647570b, "sendto");
}

/* sys_net::socket */
void func_0081685C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x9c056962, "socket");
}

/* sys_net::shutdown */
void func_0081687C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa50777c6, "shutdown");
}

/* sys_net::inet_aton */
void func_0081689C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa9a079e0, "inet_aton");
}

/* sys_net::bind */
void func_008168BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xb0a59804, "bind");
}

/* sys_net::sys_net_finalize_network */
void func_008168DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xb68d5625, "sys_net_finalize_network");
}

/* sys_net::accept */
void func_008168FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xc94f6939, "accept");
}

/* sys_net::send */
void func_0081691C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xdc751b40, "send");
}

/* sys_net::recv */
void func_0081693C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xfba04f37, "recv");
}

/* cellGcmSys::cellGcmGetTiledPitchSize */
void func_0081695C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x055bd74d, "cellGcmGetTiledPitchSize");
}

/* cellGcmSys::_cellGcmInitBody */
void func_0081697C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x15bae46b, "_cellGcmInitBody");
}

/* cellGcmSys::cellGcmAddressToOffset */
void func_0081699C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x21ac3697, "cellGcmAddressToOffset");
}

/* cellGcmSys::_cellGcmFunc15 */
void func_008169BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x3a33c1fd, "_cellGcmFunc15");
}

/* cellGcmSys::cellGcmSetFlipMode */
void func_008169DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4ae8d215, "cellGcmSetFlipMode");
}

/* cellGcmSys::cellGcmGetFlipStatus */
void func_008169FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x72a577ce, "cellGcmGetFlipStatus");
}

/* cellGcmSys::cellGcmSetWaitFlip */
void func_00816A1C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x983fb9aa, "cellGcmSetWaitFlip");
}

/* cellGcmSys::cellGcmMapMainMemory */
void func_00816A3C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa114ec67, "cellGcmMapMainMemory");
}

/* cellGcmSys::cellGcmSetDisplayBuffer */
void func_00816A5C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa53d12ae, "cellGcmSetDisplayBuffer");
}

/* cellGcmSys::cellGcmGetControlRegister */
void func_00816A7C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa547adde, "cellGcmGetControlRegister");
}

/* cellGcmSys::cellGcmSetVBlankHandler */
void func_00816A9C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa91b0402, "cellGcmSetVBlankHandler");
}

/* cellGcmSys::cellGcmResetFlipStatus */
void func_00816ABC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xb2e761d4, "cellGcmResetFlipStatus");
}

/* cellGcmSys::cellGcmSetInvalidateTile */
void func_00816ADC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xbd6d60d9, "cellGcmSetInvalidateTile");
}

/* cellGcmSys::cellGcmSetTile */
void func_00816AFC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xd0b1d189, "cellGcmSetTile");
}

/* cellGcmSys::cellGcmSetZcull */
void func_00816B1C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xd34a420d, "cellGcmSetZcull");
}

/* cellGcmSys::cellGcmSetFlip */
void func_00816B3C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xdc09357e, "cellGcmSetFlip");
}

/* cellGcmSys::cellGcmGetConfiguration */
void func_00816B5C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xe315a0b2, "cellGcmGetConfiguration");
}

/* cellGcmSys::cellGcmGetLabelAddress */
void func_00816B7C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xf80196c1, "cellGcmGetLabelAddress");
}

/* sys_io::cellPadInit */
void func_00816B9C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x1cf98800, "cellPadInit");
}

/* sys_io::cellKbClearBuf */
void func_00816BBC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x2073b7f6, "cellKbClearBuf");
}

/* sys_io::cellKbGetInfo */
void func_00816BDC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x2f1774d5, "cellKbGetInfo");
}

/* sys_io::cellMouseGetData */
void func_00816BFC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x3138e632, "cellMouseGetData");
}

/* sys_io::cellPadGetInfo */
void func_00816C1C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x3aaad464, "cellPadGetInfo");
}

/* sys_io::cellPadGetRawData */
void func_00816C3C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x3f797dff, "cellPadGetRawData");
}

/* sys_io::cellKbInit */
void func_00816C5C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x433f6ec0, "cellKbInit");
}

/* sys_io::cellPadEnd */
void func_00816C7C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4d9b75d5, "cellPadEnd");
}

/* sys_io::cellMouseGetInfo */
void func_00816C9C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x5baf30fb, "cellMouseGetInfo");
}

/* sys_io::cellPadInfoSensorMode */
void func_00816CBC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x78200559, "cellPadInfoSensorMode");
}

/* sys_io::cellPadGetData */
void func_00816CDC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x8b72cda1, "cellPadGetData");
}

/* sys_io::cellKbSetCodeType */
void func_00816CFC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa5f85e4d, "cellKbSetCodeType");
}

/* sys_io::cellPadSetSensorMode */
void func_00816D1C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xbe5be3ba, "cellPadSetSensorMode");
}

/* sys_io::cellKbEnd */
void func_00816D3C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xbfce3285, "cellKbEnd");
}

/* sys_io::cellMouseInit */
void func_00816D5C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xc9030138, "cellMouseInit");
}

/* sys_io::cellMouseEnd */
void func_00816D7C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xe10183ce, "cellMouseEnd");
}

/* sys_io::cellKbRead */
void func_00816D9C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xff0a21b7, "cellKbRead");
}

/* cellSysmodule::cellSysmoduleUnloadModule */
void func_00816DBC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x112a5ee9, "cellSysmoduleUnloadModule");
}

/* cellSysmodule::cellSysmoduleLoadModule */
void func_00816DDC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x32267a31, "cellSysmoduleLoadModule");
}

/* cellSysmodule::cellSysmoduleInitialize */
void func_00816DFC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x63ff6ff9, "cellSysmoduleInitialize");
}

/* cellSysmodule::cellSysmoduleFinalize */
void func_00816E1C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x96c07adf, "cellSysmoduleFinalize");
}

/* cellSpurs::cellSpursDetachLv2EventQueue */
void func_00816E3C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4e66d483, "cellSpursDetachLv2EventQueue");
}

/* cellSpurs::cellSpursRemoveWorkload */
void func_00816E5C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x57e4dec3, "cellSpursRemoveWorkload");
}

/* cellSpurs::cellSpursWaitForWorkloadShutdown */
void func_00816E7C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x5fd43fe4, "cellSpursWaitForWorkloadShutdown");
}

/* cellSpurs::cellSpursAddWorkload */
void func_00816E9C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x69726aa2, "cellSpursAddWorkload");
}

/* cellSpurs::cellSpursWakeUp */
void func_00816EBC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x7e4ea023, "cellSpursWakeUp");
}

/* cellSpurs::cellSpursShutdownWorkload */
void func_00816EDC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x98d5b343, "cellSpursShutdownWorkload");
}

/* cellSpurs::cellSpursInitialize */
void func_00816EFC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xacfc8dbc, "cellSpursInitialize");
}

/* cellSpurs::cellSpursAttachLv2EventQueue */
void func_00816F1C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xb9bc6207, "cellSpursAttachLv2EventQueue");
}

/* cellSpurs::cellSpursFinalize */
void func_00816F3C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xca4c4600, "cellSpursFinalize");
}

/* cellSpurs::cellSpursReadyCountStore */
void func_00816F5C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xf843818d, "cellSpursReadyCountStore");
}

/* cellNetCtl::cellNetCtlNetStartDialogLoadAsync */
void func_00816F7C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x04459230, "cellNetCtlNetStartDialogLoadAsync");
}

/* cellNetCtl::cellNetCtlNetStartDialogUnloadAsync */
void func_00816F9C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x0f1f13d3, "cellNetCtlNetStartDialogUnloadAsync");
}

/* cellNetCtl::cellNetCtlTerm */
void func_00816FBC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x105ee2cb, "cellNetCtlTerm");
}

/* cellNetCtl::cellNetCtlGetInfo */
void func_00816FDC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x1e585b5d, "cellNetCtlGetInfo");
}

/* cellNetCtl::cellNetCtlInit */
void func_00816FFC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xbd5a59fc, "cellNetCtlInit");
}

/* sys_fs::cellFsClose */
void func_0081701C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x2cb51f0d, "cellFsClose");
}

/* sys_fs::cellFsOpendir */
void func_0081703C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x3f61245c, "cellFsOpendir");
}

/* sys_fs::cellFsRead */
void func_0081705C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4d5ff8e2, "cellFsRead");
}

/* sys_fs::cellFsReaddir */
void func_0081707C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x5c74903d, "cellFsReaddir");
}

/* sys_fs::cellFsOpen */
void func_0081709C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x718bf5f8, "cellFsOpen");
}

/* sys_fs::cellFsUnlink */
void func_008170BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x7f4677a8, "cellFsUnlink");
}

/* sys_fs::cellFsLseek */
void func_008170DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa397d042, "cellFsLseek");
}

/* sys_fs::cellFsGetFreeSize */
void func_008170FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xaa3b4bcd, "cellFsGetFreeSize");
}

/* sys_fs::cellFsWrite */
void func_0081711C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xecdcf2ab, "cellFsWrite");
}

/* sys_fs::cellFsFstat */
void func_0081713C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xef3efa34, "cellFsFstat");
}

/* sys_fs::cellFsClosedir */
void func_0081715C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xff42dcc3, "cellFsClosedir");
}

/* cellAudio::cellAudioInit */
void func_0081717C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x0b168f92, "cellAudioInit");
}

/* cellSync::cellSyncBarrierInitialize */
void func_0081719C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x07254fda, "cellSyncBarrierInitialize");
}

/* cellSync::cellSyncBarrierWait */
void func_008171BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x35f21355, "cellSyncBarrierWait");
}

/* cellSync::cellSyncBarrierTryWait */
void func_008171DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x6c272124, "cellSyncBarrierTryWait");
}

/* sceNp::sceNpManagerGetTicket */
void func_008171FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x0968aa36, "sceNpManagerGetTicket");
}

/* sceNp::sceNpTerm */
void func_0081721C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4885aa18, "sceNpTerm");
}

/* sceNp::sceNpManagerRequestTicket */
void func_0081723C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x7e2fef28, "sceNpManagerRequestTicket");
}

/* sceNp::sceNpManagerGetStatus */
void func_0081725C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa7bff757, "sceNpManagerGetStatus");
}

/* sceNp::sceNpBasicRegisterHandler */
void func_0081727C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xbcc09fe7, "sceNpBasicRegisterHandler");
}

/* sceNp::sceNpInit */
void func_0081729C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xbd28fdbf, "sceNpInit");
}

/* sceNp::sceNpUtilCmpNpId */
void func_008172BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xd208f91d, "sceNpUtilCmpNpId");
}

/* sceNp::sceNpBasicGetEvent */
void func_008172DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xe035f7d6, "sceNpBasicGetEvent");
}

/* sceNp::sceNpManagerRegisterCallback */
void func_008172FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xe7dcd3b4, "sceNpManagerRegisterCallback");
}

/* sceNp::sceNpManagerGetNpId */
void func_0081731C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xfe37a7f4, "sceNpManagerGetNpId");
}

/* cellAudio::cellAudioPortClose */
void func_0081733C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4129fe2d, "cellAudioPortClose");
}

/* cellAudio::cellAudioPortStop */
void func_0081735C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x5b1e2c73, "cellAudioPortStop");
}

/* cellAudio::cellAudioGetPortConfig */
void func_0081737C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x74a66af0, "cellAudioGetPortConfig");
}

/* cellAudio::cellAudioPortStart */
void func_0081739C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x89be28f2, "cellAudioPortStart");
}

/* cellAudio::cellAudioQuit */
void func_008173BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xca5ac370, "cellAudioQuit");
}

/* cellAudio::cellAudioPortOpen */
void func_008173DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xcd7bc431, "cellAudioPortOpen");
}

/* cellSpurs::cellSpursRequestIdleSpu */
void func_008173FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x182d9890, "cellSpursRequestIdleSpu");
}

/* cellSpurs::cellSpursGetInfo */
void func_0081741C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x1f402f8f, "cellSpursGetInfo");
}

/* cellSpurs::cellSpursSetPriorities */
void func_0081743C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x80a29e27, "cellSpursSetPriorities");
}

/* cellSpurs::cellSpursSetExceptionEventHandler */
void func_0081745C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xd2e23fa9, "cellSpursSetExceptionEventHandler");
}

/* sysPrxForUser::sys_lwmutex_lock */
void func_0081747C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x1573dc3f, "sys_lwmutex_lock");
}

/* sysPrxForUser::sys_lwmutex_unlock */
void func_0081749C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x1bc200f4, "sys_lwmutex_unlock");
}

/* sysPrxForUser::sys_ppu_thread_create */
void func_008174BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x24a1ea07, "sys_ppu_thread_create");
}

/* sysPrxForUser::sys_lwmutex_create */
void func_008174DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x2f85c0ef, "sys_lwmutex_create");
}

/* sysPrxForUser::sys_ppu_thread_get_id */
void func_008174FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0x350d454e, "sys_ppu_thread_get_id");
}

/* sysPrxForUser::sys_process_is_stack */
void func_0081751C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x4f7172c9, "sys_process_is_stack");
}

/* sysPrxForUser::sys_initialize_tls */
void func_0081753C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x744680a2, "sys_initialize_tls");
}

/* sysPrxForUser::sys_time_get_system_time */
void func_0081755C(ppu_context* ctx) {
    nid_dispatch(ctx, 0x8461e528, "sys_time_get_system_time");
}

/* sysPrxForUser::sys_prx_exitspawn_with_level */
void func_0081757C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xa2c7ba64, "sys_prx_exitspawn_with_level");
}

/* sysPrxForUser::sys_lwmutex_trylock */
void func_0081759C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xaeb78725, "sys_lwmutex_trylock");
}

/* sysPrxForUser::sys_ppu_thread_exit */
void func_008175BC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xaff080a4, "sys_ppu_thread_exit");
}

/* sysPrxForUser::sys_lwmutex_destroy */
void func_008175DC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xc3476d0c, "sys_lwmutex_destroy");
}

/* sysPrxForUser::sys_process_exit */
void func_008175FC(ppu_context* ctx) {
    nid_dispatch(ctx, 0xe6f2c1e7, "sys_process_exit");
}

/* sysPrxForUser::sys_spu_image_import */
void func_0081761C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xebe5f72f, "sys_spu_image_import");
}

/* sysPrxForUser::sys_game_process_exitspawn */
void func_0081763C(ppu_context* ctx) {
    nid_dispatch(ctx, 0xfc52a7a9, "sys_game_process_exitspawn");
}

