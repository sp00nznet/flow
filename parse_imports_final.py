#!/usr/bin/env python3
"""
Final comprehensive PS3 ELF import table parser for flOw EBOOT.elf
Resolves 139/140 NIDs (99.3%)
"""

import struct
import hashlib
import json

ELF_PATH = r"D:\recomp\ps3games\flow\extracted\USRDIR\EBOOT.elf"
NID_SUFFIX = b"\x67\x59\x65\x99\x04\x25\x04\x90\x56\x64\x27\x49\x94\x89\x74\x1A"

def compute_nid(name):
    h = hashlib.sha1(name.encode('ascii') + NID_SUFFIX)
    return struct.unpack('<I', h.digest()[:4])[0]

# Complete NID database built from RPCS3-compatible function names
ALL_FUNCTIONS = [
    # cellSysutil
    "cellSysutilRegisterCallback", "cellSysutilUnregisterCallback",
    "cellSysutilCheckCallback", "cellSysutilGetSystemParamInt",
    "cellSysutilGetSystemParamString",
    "cellVideoOutGetState", "cellVideoOutGetResolution",
    "cellVideoOutConfigure", "cellAudioOutConfigure",
    "cellAudioOutGetSoundAvailability",
    "cellMsgDialogOpen", "cellMsgDialogOpen2", "cellMsgDialogAbort",
    "cellSaveDataAutoSave", "cellSaveDataAutoLoad", "cellSaveDataDelete",
    "cellHddGameCheck",
    # sys_net
    "socketpoll", "sys_net_initialize_network_ex", "getsockname",
    "recvfrom", "listen", "socketselect", "getsockopt",
    "_sys_net_errno_loc",
    "connect", "socketclose", "gethostbyname",
    "setsockopt", "sendto", "socket", "shutdown",
    "inet_aton", "bind", "sys_net_finalize_network",
    "accept", "send", "recv",
    # cellGcmSys
    "cellGcmGetTiledPitchSize", "_cellGcmInitBody",
    "cellGcmAddressToOffset", "_cellGcmFunc15",
    "cellGcmSetFlipMode", "cellGcmGetFlipStatus",
    "cellGcmSetWaitFlip", "cellGcmMapMainMemory",
    "cellGcmSetDisplayBuffer", "cellGcmGetControlRegister",
    "cellGcmSetVBlankHandler", "cellGcmResetFlipStatus",
    "cellGcmSetInvalidateTile", "cellGcmSetTile",
    "cellGcmSetZcull", "cellGcmSetFlip",
    "cellGcmGetConfiguration", "cellGcmGetLabelAddress",
    # sys_io
    "cellPadInit", "cellKbClearBuf", "cellKbGetInfo",
    "cellMouseGetData", "cellPadGetInfo", "cellPadGetRawData",
    "cellKbInit", "cellPadEnd", "cellMouseGetInfo",
    "cellPadInfoSensorMode", "cellPadGetData",
    "cellKbSetCodeType", "cellPadSetSensorMode",
    "cellKbEnd", "cellMouseInit", "cellMouseEnd", "cellKbRead",
    # cellSysmodule
    "cellSysmoduleUnloadModule", "cellSysmoduleLoadModule",
    "cellSysmoduleInitialize", "cellSysmoduleFinalize",
    # cellSpurs
    "cellSpursDetachLv2EventQueue", "cellSpursRemoveWorkload",
    "cellSpursWaitForWorkloadShutdown", "cellSpursAddWorkload",
    "cellSpursWakeUp", "cellSpursShutdownWorkload",
    "cellSpursInitialize", "cellSpursAttachLv2EventQueue",
    "cellSpursFinalize", "cellSpursReadyCountStore",
    "cellSpursRequestIdleSpu", "cellSpursGetInfo",
    "cellSpursSetPriorities", "cellSpursSetExceptionEventHandler",
    # cellNetCtl
    "cellNetCtlNetStartDialogLoadAsync",
    "cellNetCtlNetStartDialogUnloadAsync",
    "cellNetCtlTerm", "cellNetCtlGetInfo", "cellNetCtlInit",
    # sys_fs
    "cellFsClose", "cellFsOpendir", "cellFsRead",
    "cellFsReaddir", "cellFsOpen", "cellFsUnlink",
    "cellFsLseek", "cellFsGetFreeSize", "cellFsWrite",
    "cellFsFstat", "cellFsClosedir",
    # cellAudio
    "cellAudioInit", "cellAudioPortClose", "cellAudioPortStop",
    "cellAudioGetPortConfig", "cellAudioPortStart",
    "cellAudioQuit", "cellAudioPortOpen",
    # cellSync
    "cellSyncBarrierInitialize", "cellSyncBarrierWait",
    "cellSyncBarrierTryWait",
    # sceNp
    "sceNpManagerGetTicket", "sceNpTerm",
    "sceNpManagerRequestTicket", "sceNpManagerGetStatus",
    "sceNpBasicRegisterHandler", "sceNpInit",
    "sceNpUtilCmpNpId", "sceNpBasicGetEvent",
    "sceNpManagerGetNpId", "sceNpManagerRegisterCallback",
    # sysPrxForUser
    "sys_lwmutex_lock", "sys_lwmutex_unlock",
    "sys_ppu_thread_create", "sys_lwmutex_create",
    "sys_ppu_thread_get_id", "sys_process_is_stack",
    "sys_initialize_tls", "sys_time_get_system_time",
    "sys_prx_exitspawn_with_level", "sys_lwmutex_trylock",
    "sys_ppu_thread_exit", "sys_lwmutex_destroy",
    "sys_process_exit", "sys_spu_image_import",
    "sys_game_process_exitspawn",
]

NID_DB = {}
for name in ALL_FUNCTIONS:
    nid = compute_nid(name)
    NID_DB[nid] = name

def read_be32(data, off):
    return struct.unpack('>I', data[off:off+4])[0]

def read_be16(data, off):
    return struct.unpack('>H', data[off:off+2])[0]

def read_be64(data, off):
    return struct.unpack('>Q', data[off:off+8])[0]

def va_to_file(va, segments):
    for seg in segments:
        if seg['p_vaddr'] <= va < seg['p_vaddr'] + seg['p_filesz']:
            return va - seg['p_vaddr'] + seg['p_offset']
    return None

def read_string(data, offset, max_len=256):
    if offset is None or offset >= len(data):
        return None
    end = data.find(b'\x00', offset)
    if end < 0 or end > offset + max_len:
        end = offset + max_len
    try:
        return data[offset:end].decode('ascii')
    except:
        return None

def main():
    with open(ELF_PATH, 'rb') as f:
        raw = f.read()

    # Parse ELF header
    e_phoff = read_be64(raw, 0x20)
    e_phentsize = read_be16(raw, 0x36)
    e_phnum = read_be16(raw, 0x38)

    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        segments.append({
            'p_type': read_be32(raw, off),
            'p_flags': read_be32(raw, off+4),
            'p_offset': read_be64(raw, off+8),
            'p_vaddr': read_be64(raw, off+16),
            'p_paddr': read_be64(raw, off+24),
            'p_filesz': read_be64(raw, off+32),
            'p_memsz': read_be64(raw, off+40),
        })

    # sys_process_prx_param at file offset 0x808678
    prx_param_off = 0x808678
    libstub_start_va = read_be32(raw, prx_param_off + 0x18)
    libstub_end_va = read_be32(raw, prx_param_off + 0x1C)
    libstub_fstart = va_to_file(libstub_start_va, segments)
    libstub_fend = va_to_file(libstub_end_va, segments)

    # Parse stub entries
    stub_data = raw[libstub_fstart:libstub_fend]
    pos = 0
    lib_imports = []

    while pos < len(stub_data):
        entry_size = stub_data[pos]
        if entry_size == 0:
            pos += 1
            continue
        if pos + entry_size > len(stub_data):
            break

        entry = stub_data[pos:pos+entry_size]
        if entry_size == 0x2C:
            s_nfunc = read_be16(entry, 6)
            s_nvar = read_be16(entry, 8)
            s_ntls = read_be16(entry, 10)
            s_name_va = read_be32(entry, 16)
            s_fnid_va = read_be32(entry, 20)
            s_fstub_va = read_be32(entry, 24)
            s_vnid_va = read_be32(entry, 28)
            s_vstub_va = read_be32(entry, 32)

            name_foff = va_to_file(s_name_va, segments)
            lib_name = read_string(raw, name_foff) if name_foff else "???"

            funcs = []
            if s_fnid_va and s_nfunc > 0:
                fnid_foff = va_to_file(s_fnid_va, segments)
                if fnid_foff:
                    for fi in range(s_nfunc):
                        nid = read_be32(raw, fnid_foff + fi*4)
                        name = NID_DB.get(nid, f"unknown_{nid:08x}")
                        funcs.append((nid, name))

            lib_imports.append({
                'name': lib_name,
                'nfunc': s_nfunc,
                'nvar': s_nvar,
                'funcs': funcs,
                'fnid_va': s_fnid_va,
                'fstub_va': s_fstub_va,
            })

        pos += entry_size

    # Output
    print("=" * 72)
    print("  flOw PS3 (NPUA80069) EBOOT.elf - COMPLETE IMPORT TABLE")
    print("=" * 72)
    print(f"  Source: {ELF_PATH}")
    print(f"  PRX param: libstub {libstub_start_va:#010x}-{libstub_end_va:#010x}")
    print(f"  Stub entries: {len(lib_imports)} libraries")
    print()

    total_funcs = 0
    total_resolved = 0
    export_data = {}

    for lib in lib_imports:
        n_resolved = sum(1 for _, n in lib['funcs'] if not n.startswith('unknown_'))
        total_funcs += lib['nfunc']
        total_resolved += n_resolved
        pct = n_resolved * 100 // max(lib['nfunc'], 1)

        print(f"  {lib['name']} ({lib['nfunc']} imports) [{pct}% resolved]")
        print(f"  {'=' * (len(lib['name']) + 30)}")

        export_lib = []
        for i, (nid, name) in enumerate(lib['funcs']):
            resolved = "+" if not name.startswith('unknown_') else "?"
            print(f"    {resolved} {nid:08x}  {name}")
            export_lib.append({"nid": f"{nid:08x}", "name": name})

        export_data[lib['name']] = {
            "num_functions": lib['nfunc'],
            "fnid_va": f"{lib['fnid_va']:#010x}",
            "fstub_va": f"{lib['fstub_va']:#010x}",
            "functions": export_lib,
        }
        print()

    print("=" * 72)
    print(f"  TOTAL: {len(lib_imports)} libraries, {total_funcs} function imports")
    print(f"  Resolved: {total_resolved}/{total_funcs} ({total_resolved*100//max(total_funcs,1)}%)")

    unresolved = [(lib['name'], nid) for lib in lib_imports
                  for nid, name in lib['funcs'] if name.startswith('unknown_')]
    if unresolved:
        print(f"\n  Unresolved ({len(unresolved)}):")
        for libname, nid in unresolved:
            print(f"    {libname}: {nid:08x}")

    print("=" * 72)

    # Export JSON
    output = {
        "game": "flOw",
        "title_id": "NPUA80069",
        "elf": "EBOOT.elf",
        "total_libraries": len(lib_imports),
        "total_imports": total_funcs,
        "resolved": total_resolved,
        "resolution_pct": round(total_resolved * 100 / max(total_funcs, 1), 1),
        "libraries": export_data,
    }

    json_path = ELF_PATH.replace("EBOOT.elf", "imports.json")
    with open(json_path, 'w') as jf:
        json.dump(output, jf, indent=2)
    print(f"\n  Exported to: {json_path}")

if __name__ == '__main__':
    main()
