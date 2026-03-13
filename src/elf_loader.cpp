/*
 * flOw Recomp -- ELF segment loader
 *
 * Parses the original EBOOT.elf and copies PT_LOAD segments into the
 * ps3recomp virtual memory.  This populates the guest address space
 * with the game's initialized data (.data, .rodata) and zeroes .bss.
 *
 * Code segments are skipped — we don't execute guest PowerPC code,
 * we call recompiled native functions instead.  But data segments are
 * essential because the recompiled code accesses globals at their
 * original guest addresses via vm_read/vm_write.
 */

#include "elf_loader.h"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

/* ps3recomp VM access */
extern "C" {
    extern uint8_t* vm_base;
    void* vm_translate(uint32_t addr);
}

/* ---------------------------------------------------------------------------
 * ELF64 big-endian structures (minimal, just what we need)
 * -----------------------------------------------------------------------*/

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

#define ELF_MAGIC       "\x7f" "ELF"
#define ELFCLASS64      2
#define ELFDATA2MSB     2
#define PT_LOAD         1

/* ELF64 header (big-endian) */
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/* ELF64 program header (big-endian) */
struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Parse big-endian ELF64 header from raw bytes */
static bool parse_ehdr(const uint8_t* data, size_t size, Elf64_Ehdr* out)
{
    if (size < 64) return false;
    if (memcmp(data, ELF_MAGIC, 4) != 0) return false;
    if (data[4] != ELFCLASS64) return false;
    if (data[5] != ELFDATA2MSB) return false;

    memcpy(out->e_ident, data, 16);
    const uint8_t* p = data + 16;
    out->e_type      = be16(p);      p += 2;
    out->e_machine   = be16(p);      p += 2;
    out->e_version   = be32(p);      p += 4;
    out->e_entry     = be64(p);      p += 8;
    out->e_phoff     = be64(p);      p += 8;
    out->e_shoff     = be64(p);      p += 8;
    out->e_flags     = be32(p);      p += 4;
    out->e_ehsize    = be16(p);      p += 2;
    out->e_phentsize = be16(p);      p += 2;
    out->e_phnum     = be16(p);      p += 2;
    out->e_shentsize = be16(p);      p += 2;
    out->e_shnum     = be16(p);      p += 2;
    out->e_shstrndx  = be16(p);
    return true;
}

/* Parse big-endian ELF64 program header */
static void parse_phdr(const uint8_t* data, Elf64_Phdr* out)
{
    out->p_type   = be32(data);      data += 4;
    out->p_flags  = be32(data);      data += 4;
    out->p_offset = be64(data);      data += 8;
    out->p_vaddr  = be64(data);      data += 8;
    out->p_paddr  = be64(data);      data += 8;
    out->p_filesz = be64(data);      data += 8;
    out->p_memsz  = be64(data);      data += 8;
    out->p_align  = be64(data);
}

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

bool elf_load_segments(const char* elf_path)
{
    FILE* fp = fopen(elf_path, "rb");
    if (!fp) {
        fprintf(stderr, "[elf_loader] Cannot open: %s\n", elf_path);
        return false;
    }

    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 64 * 1024 * 1024) {
        fprintf(stderr, "[elf_loader] Invalid file size: %ld\n", file_size);
        fclose(fp);
        return false;
    }

    uint8_t* file_data = (uint8_t*)malloc((size_t)file_size);
    if (!file_data) {
        fprintf(stderr, "[elf_loader] Out of memory\n");
        fclose(fp);
        return false;
    }

    if (fread(file_data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        fprintf(stderr, "[elf_loader] Read error\n");
        free(file_data);
        fclose(fp);
        return false;
    }
    fclose(fp);

    /* Parse ELF header */
    Elf64_Ehdr ehdr;
    if (!parse_ehdr(file_data, (size_t)file_size, &ehdr)) {
        fprintf(stderr, "[elf_loader] Not a valid ELF64 big-endian file\n");
        free(file_data);
        return false;
    }

    printf("[elf_loader] ELF64 BE, entry=0x%08llX, %u program headers\n",
           (unsigned long long)ehdr.e_entry, ehdr.e_phnum);

    /* Process program headers */
    int segments_loaded = 0;
    size_t bytes_loaded = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        uint64_t ph_off = ehdr.e_phoff + (uint64_t)i * ehdr.e_phentsize;
        if (ph_off + ehdr.e_phentsize > (uint64_t)file_size)
            break;

        Elf64_Phdr phdr;
        parse_phdr(file_data + ph_off, &phdr);

        if (phdr.p_type != PT_LOAD)
            continue;
        if (phdr.p_memsz == 0)
            continue;

        /* Truncate 64-bit vaddr to 32-bit guest address */
        uint32_t guest_addr = (uint32_t)(phdr.p_vaddr & 0xFFFFFFFF);

        /* Determine if this is a code or data segment */
        bool is_exec = (phdr.p_flags & 1) != 0;  /* PF_X */
        const char* seg_type = is_exec ? "CODE" : "DATA";

        printf("[elf_loader]   Segment %d: %s  vaddr=0x%08X  filesz=0x%llX  memsz=0x%llX\n",
               i, seg_type, guest_addr,
               (unsigned long long)phdr.p_filesz,
               (unsigned long long)phdr.p_memsz);

        /* Validate guest address is within VM range */
        if (guest_addr + phdr.p_memsz > 0xE0000000ULL) {
            printf("[elf_loader]     Skipping (address out of VM range)\n");
            continue;
        }

        /* Get host pointer for this guest address */
        uint8_t* host_ptr = vm_base + guest_addr;

        /* Zero the full memsz range (covers .bss) */
        memset(host_ptr, 0, (size_t)phdr.p_memsz);

        /* Copy initialized data from file */
        if (phdr.p_filesz > 0 && phdr.p_offset + phdr.p_filesz <= (uint64_t)file_size) {
            memcpy(host_ptr, file_data + phdr.p_offset, (size_t)phdr.p_filesz);
        }

        segments_loaded++;
        bytes_loaded += (size_t)phdr.p_memsz;
    }

    free(file_data);

    printf("[elf_loader] Loaded %d segments (%zu bytes) into VM\n",
           segments_loaded, bytes_loaded);

    return segments_loaded > 0;
}
