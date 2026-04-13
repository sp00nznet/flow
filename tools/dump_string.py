"""Dump a string at a given guest offset in EBOOT.elf."""
import sys, struct

if len(sys.argv) < 4:
    print("Usage: dump_string.py <elf> <addr_hex> <len_hex>")
    sys.exit(2)

elf_path = sys.argv[1]
addr = int(sys.argv[2], 16)
n = int(sys.argv[3], 16)

with open(elf_path, "rb") as f:
    data = f.read()

# Parse ELF64 program headers
e_phoff  = struct.unpack(">Q", data[0x20:0x28])[0]
e_phnum  = struct.unpack(">H", data[0x38:0x3A])[0]
e_phentsize = struct.unpack(">H", data[0x36:0x38])[0]

for i in range(e_phnum):
    ph_off = e_phoff + i * e_phentsize
    p_type, p_flags = struct.unpack(">II", data[ph_off:ph_off+8])
    p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
        struct.unpack(">QQQQQQ", data[ph_off+8:ph_off+8+48])
    if p_vaddr <= addr < p_vaddr + p_filesz:
        file_off = p_offset + (addr - p_vaddr)
        chunk = data[file_off:file_off + n]
        print(f"segment vaddr=0x{p_vaddr:08X} type={p_type} flags={p_flags:#x}")
        print(f"bytes: {chunk.hex()}")
        print(f"ascii: {chunk.decode('latin-1')!r}")
        sys.exit(0)

print(f"0x{addr:08X} not in any loaded segment")
