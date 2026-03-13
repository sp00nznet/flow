/*
 * flOw Recomp -- ELF segment loader
 *
 * Loads PT_LOAD segments from the original EBOOT.elf into the ps3recomp
 * virtual address space at runtime.  The recompiled code references global
 * variables and read-only data via vm_read/vm_write at their original
 * guest addresses, so these segments must be mapped before execution.
 */
#ifndef FLOW_ELF_LOADER_H
#define FLOW_ELF_LOADER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load all PT_LOAD segments from the ELF file into virtual memory.
 * Returns true on success, false if the file is missing/corrupt.
 *
 * Only data segments (non-executable) are loaded — the code segments
 * have already been statically recompiled to native C.
 */
bool elf_load_segments(const char* elf_path);

#ifdef __cplusplus
}
#endif

#endif /* FLOW_ELF_LOADER_H */
