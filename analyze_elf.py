#!/usr/bin/env python3
"""
Comprehensive PS3 ELF analyzer for flOw EBOOT.elf
Produces a full structural analysis: segments, sections, functions, imports, relocations.
"""

import struct
import json
import os

ELF_PATH = r"D:\recomp\ps3games\flow\extracted\USRDIR\EBOOT.elf"
IMPORTS_PATH = r"D:\recomp\ps3games\flow\extracted\USRDIR\imports.json"
FUNCTIONS_PATH = r"D:\recomp\ps3games\flow\analysis.json"
OUTPUT_PATH = r"D:\recomp\ps3games\flow\elf_analysis.json"

# ELF constants
PT_LOAD = 1
PT_TLS = 7

PF_X = 0x1
PF_W = 0x2
PF_R = 0x4

SHT_NULL = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8

SHT_NAMES = {
    0: "NULL", 1: "PROGBITS", 2: "SYMTAB", 3: "STRTAB", 4: "RELA",
    5: "HASH", 6: "DYNAMIC", 7: "NOTE", 8: "NOBITS", 9: "REL",
    11: "DYNSYM",
}

# Known PS3 section addresses for flOw (from section header analysis)
# Since the strtab is zeroed, we infer names from known VA/characteristics
KNOWN_SECTIONS = {
    0x00010200: ".init",
    0x00010230: ".text",
    0x008164b8: ".text.fini",      # small code after main text
    0x008164dc: ".stub_code",      # import stub code (140 * 32 = 0x1180 bytes)
    0x0081765c: ".rodata.sceResident",
    0x00818150: ".lib.stub",       # import stub table headers
    0x00818208: ".fnid",           # function NID arrays
    0x00818438: ".vnid",
    0x0081843c: ".vstub",
    0x00818440: ".bnid",
    0x00818444: ".data.sceFStub",  # function stub pointer arrays
    0x00818654: ".rodata.sceProcess",
    0x00818658: ".sys_proc_param",
    0x00818678: ".sys_proc_prx_param",
    0x00820000: ".got.plt",         # PLT-like function pointer table
    0x008202a0: ".got",
    0x008204f0: ".ctors",
    0x008204f4: ".data",
    0x00827768: ".data.sceFStub.ptr",  # stub pointer table
    0x00827998: ".bss_like_data",
    0x00846ad0: ".opd",            # real procedure descriptors (func_addr + TOC)
    0x0088e9a8: ".data.2",
    0x00895cd0: ".tdata",
    0x00895cd8: ".tbss",
    0x10000000: ".rodata",
    0x10040000: ".data.large",
    0x1010ebd8: ".data.empty",
    0x1010ec00: ".data.rel",
    0x10111a80: ".bss",
}

def read_be16(d, o): return struct.unpack('>H', d[o:o+2])[0]
def read_be32(d, o): return struct.unpack('>I', d[o:o+4])[0]
def read_be64(d, o): return struct.unpack('>Q', d[o:o+8])[0]

def va_to_file(va, segments):
    for seg in segments:
        if seg['vaddr'] <= va < seg['vaddr'] + seg['filesz']:
            return va - seg['vaddr'] + seg['offset']
    return None

def read_cstring(data, offset, max_len=256):
    if offset is None or offset >= len(data):
        return None
    end = data.find(b'\x00', offset)
    if end < 0 or end > offset + max_len:
        end = offset + max_len
    try:
        return data[offset:end].decode('ascii')
    except:
        return None

def segment_type_name(pt):
    names = {
        0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE",
        6: "PHDR", 7: "TLS", 0x60000001: "PROC_PARAM", 0x60000002: "PROC_PRX",
    }
    return names.get(pt, f"0x{pt:08x}")

def segment_flags_str(flags):
    s = ""
    s += "R" if flags & PF_R else "-"
    s += "W" if flags & PF_W else "-"
    s += "X" if flags & PF_X else "-"
    return s

def classify_function(va, segments):
    for seg in segments:
        if seg['vaddr'] <= va < seg['vaddr'] + seg['memsz']:
            return seg.get('label', 'unknown')
    return 'unmapped'

def main():
    with open(ELF_PATH, 'rb') as f:
        raw = f.read()

    file_size = len(raw)

    # === ELF Header ===
    assert raw[:4] == b'\x7fELF', "Not an ELF file"
    ei_class = raw[4]
    ei_data = raw[5]
    e_type = read_be16(raw, 16)
    e_machine = read_be16(raw, 18)
    e_entry = read_be64(raw, 24)
    e_phoff = read_be64(raw, 32)
    e_shoff = read_be64(raw, 40)
    e_flags = read_be32(raw, 48)
    e_phentsize = read_be16(raw, 54)
    e_phnum = read_be16(raw, 56)
    e_shentsize = read_be16(raw, 58)
    e_shnum = read_be16(raw, 60)
    e_shstrndx = read_be16(raw, 62)

    elf_header = {
        'class': '64-bit' if ei_class == 2 else '32-bit',
        'endian': 'Big Endian' if ei_data == 2 else 'Little Endian',
        'type': {2: 'EXEC', 0xFE00: 'SCE_EXEC', 0xFE04: 'SCE_RELEXEC'}.get(e_type, f'0x{e_type:04x}'),
        'machine': {0x15: 'PPC', 0x16: 'PPC64'}.get(e_machine, f'0x{e_machine:04x}'),
        'entry_point': f"0x{e_entry:08x}",
        'flags': f"0x{e_flags:08x}",
        'phnum': e_phnum,
        'shnum': e_shnum,
    }

    print(f"{'='*72}")
    print(f"  flOw PS3 EBOOT.elf - COMPREHENSIVE ELF ANALYSIS")
    print(f"{'='*72}")
    print(f"  File size: {file_size:,} bytes ({file_size/1024/1024:.1f} MB)")
    print(f"  Class: {elf_header['class']}, Endian: {elf_header['endian']}")
    print(f"  Type: {elf_header['type']}, Machine: {elf_header['machine']}")
    print(f"  Entry point: {elf_header['entry_point']}")

    # === Program Headers ===
    print(f"\n  --- SEGMENTS ({e_phnum} program headers) ---")
    segments = []
    text_seg = None

    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = read_be32(raw, off)
        p_flags = read_be32(raw, off + 4)
        p_offset = read_be64(raw, off + 8)
        p_vaddr = read_be64(raw, off + 16)
        p_paddr = read_be64(raw, off + 24)
        p_filesz = read_be64(raw, off + 32)
        p_memsz = read_be64(raw, off + 40)
        p_align = read_be64(raw, off + 48)

        seg = {
            'index': i, 'type': segment_type_name(p_type), 'type_raw': p_type,
            'flags': segment_flags_str(p_flags), 'flags_raw': p_flags,
            'offset': p_offset, 'vaddr': p_vaddr, 'paddr': p_paddr,
            'filesz': p_filesz, 'memsz': p_memsz, 'align': p_align,
        }

        if p_type == PT_LOAD:
            if p_flags & PF_X:
                seg['label'] = 'text'
                text_seg = seg
            elif p_flags & PF_W:
                seg['label'] = 'data+bss' if p_filesz < p_memsz else 'data'
            else:
                seg['label'] = 'rodata'
        elif p_type == PT_TLS:
            seg['label'] = 'tls'
        elif p_type == 0x60000001:
            seg['label'] = 'proc_param'
        elif p_type == 0x60000002:
            seg['label'] = 'proc_prx'
        else:
            seg['label'] = seg['type'].lower()

        segments.append(seg)
        end_va = p_vaddr + p_memsz
        print(f"  [{i}] {seg['type']:12s} {seg['flags']} "
              f"0x{p_vaddr:08x}-0x{end_va:08x} "
              f"FileSz=0x{p_filesz:08x} MemSz=0x{p_memsz:08x} ({seg['label']})")

    # === Section Headers (names inferred since strtab is zeroed) ===
    sections = []
    opd_section = None
    if e_shnum > 0 and e_shoff > 0:
        print(f"\n  --- SECTIONS ({e_shnum} headers, names inferred) ---")
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            sh_name = read_be32(raw, off)
            sh_type = read_be32(raw, off + 4)
            sh_flags = read_be64(raw, off + 8)
            sh_addr = read_be64(raw, off + 16)
            sh_offset = read_be64(raw, off + 24)
            sh_size = read_be64(raw, off + 32)
            sh_link = read_be32(raw, off + 40)
            sh_info = read_be32(raw, off + 44)
            sh_entsize = read_be64(raw, off + 56)

            # Infer name from known VA map
            name = KNOWN_SECTIONS.get(sh_addr, f"sec_{i}")
            type_name = SHT_NAMES.get(sh_type, f"0x{sh_type:08x}")

            sec = {
                'index': i, 'name': name, 'type': type_name, 'type_raw': sh_type,
                'flags': sh_flags, 'addr': sh_addr, 'offset': sh_offset,
                'size': sh_size, 'link': sh_link, 'info': sh_info, 'entsize': sh_entsize,
            }
            sections.append(sec)

            if sh_type != SHT_NULL:
                flags_str = ""
                if sh_flags & 0x1: flags_str += "W"
                if sh_flags & 0x2: flags_str += "A"
                if sh_flags & 0x4: flags_str += "X"
                print(f"  [{i:2d}] {name:25s} {type_name:10s} {flags_str:4s} "
                      f"VA=0x{sh_addr:08x} Size=0x{sh_size:08x}")

            # Detect OPD
            if name == '.opd':
                opd_section = sec

    # === Load imports ===
    imports_data = {}
    if os.path.exists(IMPORTS_PATH):
        with open(IMPORTS_PATH) as f:
            imports_data = json.load(f)

    # Build stub code address -> function name mapping
    # Each library's fstub_va points to a table of 4-byte pointers
    # Each pointer targets a 32-byte stub code block starting at 0x008164dc
    stub_code_map = {}  # stub code VA -> import info
    stub_ptr_map = {}   # stub pointer table VA -> import info
    if imports_data:
        for lib_name, lib_info in imports_data.get('libraries', {}).items():
            fstub_va = int(lib_info['fstub_va'], 16)
            fstub_foff = va_to_file(fstub_va, segments)
            if fstub_foff is None:
                continue

            for i, func in enumerate(lib_info['functions']):
                ptr_va = fstub_va + i * 4
                # Read the pointer to get stub code address
                code_va = read_be32(raw, fstub_foff + i * 4)
                info = {
                    'name': func['name'],
                    'nid': func['nid'],
                    'library': lib_name,
                    'stub_ptr_va': f"0x{ptr_va:08x}",
                    'stub_code_va': f"0x{code_va:08x}",
                }
                stub_code_map[code_va] = info
                stub_ptr_map[ptr_va] = info

    # === OPD Analysis ===
    # The real OPD is at .opd (0x846ad0) with valid func_addr+TOC pairs
    # .got.plt at 0x820000 is a different structure (PLT-like pointers)
    opd_entries = []
    toc_base = None
    opd_func_map = {}  # opd_va -> func_addr

    TEXT_START = 0x00010000
    TEXT_END = 0x008186a0

    for sec in sections:
        if sec['name'] == '.opd' and sec['type_raw'] == SHT_PROGBITS:
            off = sec['offset']
            end = off + sec['size']
            va = sec['addr']
            while off + 8 <= end:
                func_addr = read_be32(raw, off)
                toc = read_be32(raw, off + 4)
                # Validate: func must be in text segment, TOC must be non-zero
                if TEXT_START <= func_addr < TEXT_END and toc != 0:
                    opd_entries.append({
                        'opd_va': va,
                        'func_addr': func_addr,
                        'toc': toc,
                    })
                    opd_func_map[va] = func_addr
                    if toc_base is None:
                        toc_base = toc
                off += 8
                va += 8

    print(f"\n  --- OPD (Procedure Descriptors) ---")
    print(f"    Total OPD entries: {len(opd_entries)}")
    if toc_base:
        print(f"    TOC base: 0x{toc_base:08x}")
    if opd_entries:
        print(f"    First: OPD 0x{opd_entries[0]['opd_va']:08x} -> func 0x{opd_entries[0]['func_addr']:08x}")
        print(f"    Last:  OPD 0x{opd_entries[-1]['opd_va']:08x} -> func 0x{opd_entries[-1]['func_addr']:08x}")

    # === Entry point analysis ===
    entry_va = int(elf_header['entry_point'], 16)
    print(f"\n  --- ENTRY POINT ---")
    print(f"    Entry VA: 0x{entry_va:08x}")
    if entry_va in opd_func_map:
        real_entry = opd_func_map[entry_va]
        print(f"    OPD -> real code at: 0x{real_entry:08x}")
    else:
        print(f"    (Direct code entry, not OPD)")

    # === Load function boundaries ===
    func_data = []
    if os.path.exists(FUNCTIONS_PATH):
        with open(FUNCTIONS_PATH) as f:
            func_data = json.load(f)

    # Build function start set
    func_start_set = set(int(e['start'], 16) for e in func_data)

    # Check how many OPD targets match known functions
    opd_matched = sum(1 for e in opd_entries if e['func_addr'] in func_start_set)
    opd_import_stubs = sum(1 for e in opd_entries if e['func_addr'] in stub_code_map)
    print(f"\n    OPD entries pointing to known functions: {opd_matched}/{len(opd_entries)}")
    print(f"    OPD entries pointing to import stubs: {opd_import_stubs}")

    # === Classify functions ===
    print(f"\n  --- FUNCTION ANALYSIS ({len(func_data)} functions) ---")

    # Import stub range: 0x008164dc to 0x0081765c (section [4])
    STUB_CODE_START = 0x008164dc
    STUB_CODE_END = 0x0081765c

    func_sizes = []
    import_funcs = []
    game_funcs = []
    tiny_funcs = []

    for entry in func_data:
        start = int(entry['start'], 16)
        end_addr = int(entry['end'], 16)
        size = end_addr - start

        func_info = {
            'start': f"0x{start:08x}",
            'end': f"0x{end_addr:08x}",
            'size': size,
        }

        if start in stub_code_map:
            func_info['type'] = 'import_stub'
            func_info['name'] = stub_code_map[start]['name']
            func_info['library'] = stub_code_map[start]['library']
            func_info['nid'] = stub_code_map[start]['nid']
            import_funcs.append(func_info)
        elif STUB_CODE_START <= start < STUB_CODE_END:
            # In stub range but not in our map — probably a shared stub
            func_info['type'] = 'import_stub_unmapped'
            tiny_funcs.append(func_info)
        elif size <= 16:
            func_info['type'] = 'thunk'
            tiny_funcs.append(func_info)
        else:
            func_info['type'] = 'game'
            func_info['segment'] = classify_function(start, segments)
            game_funcs.append(func_info)

        func_sizes.append(size)

    if func_sizes:
        func_sizes.sort()
        total_code = sum(func_sizes)
        avg_size = total_code / len(func_sizes)
        median_size = func_sizes[len(func_sizes) // 2]

        print(f"    Total functions:  {len(func_data):,}")
        print(f"    Game functions:   {len(game_funcs):,}")
        print(f"    Import stubs:     {len(import_funcs):,} (of 140 imports)")
        print(f"    Tiny/thunks:      {len(tiny_funcs):,}")
        print(f"    Total code size:  {total_code:,} bytes ({total_code/1024/1024:.2f} MB)")
        print(f"    Avg func size:    {avg_size:.0f} bytes")
        print(f"    Median func size: {median_size} bytes")
        print(f"    Largest func:     {max(func_sizes):,} bytes")
        print(f"    Smallest func:    {min(func_sizes)} bytes")

        # Size distribution
        brackets = [(0, 16, "0-16"), (17, 64, "17-64"), (65, 256, "65-256"),
                     (257, 1024, "257-1K"), (1025, 4096, "1K-4K"),
                     (4097, 16384, "4K-16K"), (16385, 65536, "16K-64K"),
                     (65537, float('inf'), "64K+")]
        print("\n    Size distribution:")
        for lo, hi, label in brackets:
            count = sum(1 for s in func_sizes if lo <= s <= hi)
            if count:
                bar = "#" * min(count // 20 + 1, 50)
                print(f"      {label:10s}: {count:5d}  {bar}")

    # === Import stub details ===
    print(f"\n  --- IMPORT STUBS (mapped: {len(import_funcs)}/140) ---")
    by_lib = {}
    for f in import_funcs:
        lib = f['library']
        by_lib.setdefault(lib, []).append(f)
    for lib in sorted(by_lib.keys()):
        funcs = by_lib[lib]
        print(f"    {lib}: {len(funcs)} stubs")
        for fn in funcs[:3]:
            print(f"      {fn['start']}: {fn['name']}")
        if len(funcs) > 3:
            print(f"      ... and {len(funcs)-3} more")

    # === Memory Map Summary ===
    print(f"\n  --- MEMORY MAP ---")
    for seg in segments:
        if seg['type_raw'] == PT_LOAD and seg['memsz'] > 0:
            end_va = seg['vaddr'] + seg['memsz']
            print(f"    0x{seg['vaddr']:08x} - 0x{end_va:08x}  "
                  f"{seg['flags']} {seg['label']:10s} "
                  f"({seg['memsz']:,} bytes = {seg['memsz']/1024/1024:.2f} MB)")

    # === PRX PARAM ===
    print(f"\n  --- PRX PARAM ---")
    for seg in segments:
        if seg['type_raw'] == 0x60000001:
            pp_off = seg['offset']
            pp_size = read_be32(raw, pp_off)
            pp_magic = read_be32(raw, pp_off + 4)
            pp_version = read_be32(raw, pp_off + 8)
            print(f"    Size: {pp_size}, Magic: 0x{pp_magic:08x}, SDK ver: 0x{pp_version:08x}")
            break

    # === Top 20 largest functions ===
    print(f"\n  --- TOP 20 LARGEST FUNCTIONS ---")
    all_funcs_sorted = sorted(
        [{'start': int(e['start'], 16), 'end': int(e['end'], 16),
          'size': int(e['end'], 16) - int(e['start'], 16)} for e in func_data],
        key=lambda x: x['size'], reverse=True
    )
    for i, fn in enumerate(all_funcs_sorted[:20]):
        name = stub_code_map.get(fn['start'], {}).get('name', '')
        label = f"  ({name})" if name else ""
        print(f"    {i+1:2d}. 0x{fn['start']:08x} - 0x{fn['end']:08x}  "
              f"{fn['size']:6,} bytes{label}")

    # === Cross-reference: calls to import stubs ===
    # Scan text segment for bl (branch-and-link) instructions targeting stub addresses
    print(f"\n  --- IMPORT CALL FREQUENCY (scanning for bl to stubs) ---")
    call_counts = {}
    if text_seg:
        text_off = text_seg['offset']
        text_end = text_off + text_seg['filesz']
        text_va_base = text_seg['vaddr']
        pos = text_off
        while pos + 4 <= text_end:
            insn = read_be32(raw, pos)
            # bl instruction: opcode 18 (bits 0-5 = 010010), AA=0, LK=1
            if (insn & 0xFC000003) == 0x48000001:
                # Extract signed 24-bit offset (bits 6-29, shifted left 2)
                li = insn & 0x03FFFFFC
                if li & 0x02000000:  # sign extend
                    li -= 0x04000000
                current_va = text_va_base + (pos - text_off)
                target_va = current_va + li
                if target_va in stub_code_map:
                    name = stub_code_map[target_va]['name']
                    call_counts[name] = call_counts.get(name, 0) + 1
            pos += 4

    if call_counts:
        sorted_calls = sorted(call_counts.items(), key=lambda x: -x[1])
        print(f"    Top 30 most-called imports:")
        for name, count in sorted_calls[:30]:
            lib = stub_code_map.get(
                next((va for va, info in stub_code_map.items() if info['name'] == name), 0),
                {}
            ).get('library', '?')
            print(f"      {count:5d}x  {name:45s} [{lib}]")
        print(f"    Total import calls found: {sum(call_counts.values()):,}")
        uncalled = [info['name'] for va, info in stub_code_map.items()
                    if info['name'] not in call_counts]
        if uncalled:
            print(f"    Uncalled imports ({len(uncalled)}):")
            for name in uncalled:
                print(f"      - {name}")

    # === Export JSON ===
    output = {
        'file': os.path.basename(ELF_PATH),
        'file_size': file_size,
        'elf_header': elf_header,
        'segments': [{
            'index': s['index'], 'type': s['type'], 'flags': s['flags'],
            'vaddr': f"0x{s['vaddr']:08x}", 'offset': f"0x{s['offset']:08x}",
            'filesz': s['filesz'], 'memsz': s['memsz'], 'label': s.get('label', ''),
        } for s in segments],
        'sections': [{
            'index': s['index'], 'name': s['name'], 'type': s['type'],
            'addr': f"0x{s['addr']:08x}", 'offset': f"0x{s['offset']:08x}", 'size': s['size'],
        } for s in sections if s['type_raw'] != SHT_NULL],
        'memory_map': [{
            'start': f"0x{s['vaddr']:08x}",
            'end': f"0x{s['vaddr'] + s['memsz']:08x}",
            'flags': s['flags'], 'label': s.get('label', ''), 'size': s['memsz'],
        } for s in segments if s['type_raw'] == PT_LOAD and s['memsz'] > 0],
        'toc_base': f"0x{toc_base:08x}" if toc_base else None,
        'entry_point': elf_header['entry_point'],
        'opd': {
            'total_entries': len(opd_entries),
            'matched_to_functions': opd_matched,
            'matched_to_stubs': opd_import_stubs,
        },
        'functions': {
            'total': len(func_data),
            'game': len(game_funcs),
            'import_stubs': len(import_funcs),
            'thunks': len(tiny_funcs),
            'total_code_bytes': sum(func_sizes) if func_sizes else 0,
            'size_stats': {
                'avg': round(avg_size, 1),
                'median': median_size,
                'max': max(func_sizes),
                'min': min(func_sizes),
            } if func_sizes else {},
        },
        'import_call_frequency': dict(sorted(call_counts.items(), key=lambda x: -x[1])),
        'import_stubs': [{
            'name': f['name'], 'library': f['library'],
            'addr': f['start'], 'nid': f['nid'],
        } for f in import_funcs],
        'top_20_largest': [{
            'start': f"0x{fn['start']:08x}", 'end': f"0x{fn['end']:08x}",
            'size': fn['size'],
        } for fn in all_funcs_sorted[:20]],
    }

    with open(OUTPUT_PATH, 'w') as f:
        json.dump(output, f, indent=2)
    print(f"\n  Analysis exported to: {OUTPUT_PATH}")
    print(f"{'='*72}")

if __name__ == '__main__':
    main()
