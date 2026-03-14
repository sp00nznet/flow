#!/usr/bin/env python3
"""PS3 PKG file extractor - corrected for PS3 PKG format."""
import struct
import sys
import os
from Crypto.Cipher import AES

# PS3 PKG retail AES key
PKG_AES_KEY = bytes([
    0x2E, 0x7B, 0x71, 0xD7, 0xC9, 0xC9, 0xA1, 0x4E,
    0xA3, 0x22, 0x1F, 0x18, 0x88, 0x28, 0xB8, 0xF8
])

def decrypt_data(cipher_ecb, iv, encrypted, offset_in_data=0):
    """Decrypt PKG data using AES-128-CTR with ECB-based keystream."""
    result = bytearray()
    block_idx = offset_in_data // 16
    block_offset = offset_in_data % 16
    iv_int = int.from_bytes(iv, 'big')

    pos = 0
    while pos < len(encrypted):
        counter_val = iv_int + block_idx
        counter_bytes = (counter_val & ((1 << 128) - 1)).to_bytes(16, 'big')
        keystream = cipher_ecb.encrypt(counter_bytes)

        start = block_offset if block_idx == (offset_in_data // 16) else 0
        for j in range(start, 16):
            if pos >= len(encrypted):
                break
            result.append(encrypted[pos] ^ keystream[j])
            pos += 1
        block_idx += 1
    return bytes(result)


def extract_pkg(pkg_path, output_dir):
    with open(pkg_path, 'rb') as f:
        hdr = f.read(0xC0)

        magic = hdr[0:4]
        if magic != b'\x7FPKG':
            print(f"Not a PKG file (magic: {magic.hex()})")
            return False

        # Correct PS3 PKG v1 header layout
        item_count = struct.unpack_from('>I', hdr, 0x14)[0]
        total_size = struct.unpack_from('>Q', hdr, 0x18)[0]
        data_offset = struct.unpack_from('>Q', hdr, 0x20)[0]
        data_size = struct.unpack_from('>Q', hdr, 0x28)[0]
        content_id = hdr[0x30:0x60].split(b'\x00')[0].decode('ascii', errors='replace')
        pkg_iv = hdr[0x70:0x80]

        print(f"PKG: {os.path.basename(pkg_path)}")
        print(f"  Content ID: {content_id}")
        print(f"  Items:      {item_count}")
        print(f"  Data:       {data_size:,} bytes ({data_size/1024/1024:.1f} MB)")
        print()

        cipher = AES.new(PKG_AES_KEY, AES.MODE_ECB)
        os.makedirs(output_dir, exist_ok=True)

        # Read and decrypt file table (each entry = 32 bytes)
        table_size = item_count * 32
        f.seek(data_offset)
        enc_table = f.read(table_size)
        file_table = decrypt_data(cipher, pkg_iv, enc_table, 0)

        files_extracted = 0
        dirs_created = 0

        for i in range(item_count):
            entry = file_table[i*32:(i+1)*32]
            name_offset = struct.unpack_from('>I', entry, 0x00)[0]
            name_size = struct.unpack_from('>I', entry, 0x04)[0]
            file_data_offset = struct.unpack_from('>Q', entry, 0x08)[0]
            file_data_size = struct.unpack_from('>Q', entry, 0x10)[0]
            flags = struct.unpack_from('>I', entry, 0x18)[0]

            # Decrypt filename
            f.seek(data_offset + name_offset)
            enc_name = f.read(name_size)
            dec_name = decrypt_data(cipher, pkg_iv, enc_name, name_offset)
            filename = dec_name.split(b'\x00')[0].decode('utf-8', errors='replace')

            if not filename:
                continue

            is_dir = (flags & 0xFF) == 0x04
            full_path = os.path.join(output_dir, filename)

            if is_dir:
                os.makedirs(full_path, exist_ok=True)
                dirs_created += 1
            else:
                parent = os.path.dirname(full_path)
                if parent:
                    os.makedirs(parent, exist_ok=True)

                if file_data_size > 0:
                    f.seek(data_offset + file_data_offset)
                    chunk_size = 1024 * 1024
                    remaining = file_data_size
                    current_offset = file_data_offset

                    with open(full_path, 'wb') as out:
                        while remaining > 0:
                            to_read = min(chunk_size, remaining)
                            enc_chunk = f.read(to_read)
                            if not enc_chunk:
                                break
                            dec_chunk = decrypt_data(cipher, pkg_iv, enc_chunk, current_offset)
                            out.write(dec_chunk)
                            current_offset += len(enc_chunk)
                            remaining -= len(enc_chunk)
                else:
                    open(full_path, 'wb').close()

                files_extracted += 1
                if files_extracted % 50 == 0 or file_data_size > 1024*1024:
                    size_str = f"{file_data_size/1024/1024:.1f}MB" if file_data_size > 1024*1024 else f"{file_data_size:,}b"
                    print(f"  [{files_extracted}/{item_count}] {filename} ({size_str})")

        print(f"\nDone: {files_extracted} files, {dirs_created} directories -> {output_dir}/")
        return True


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <pkg_file> [output_dir]")
        sys.exit(1)

    pkg_file = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted'
    extract_pkg(pkg_file, out_dir)
