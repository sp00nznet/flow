/* Bridge between lifter's memory API and ps3recomp runtime.
 *
 * Implements vm_read/vm_write with uint64_t addresses (lifter convention)
 * by truncating to uint32_t and performing big-endian byte swaps via vm_base.
 */

#include <cstdint>
#include <cstring>
#include <cstdio>

#ifdef _MSC_VER
#include <stdlib.h>
static inline uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
static inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v);  }
static inline uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

/* vm_base is defined in main.cpp with C linkage and set by vm_init() */
extern "C" uint8_t* vm_base;

static inline uint8_t* translate(uint64_t addr) {
    return vm_base + (uint32_t)addr;
}

extern "C" {

uint8_t vm_read8(uint64_t addr) {
    return *translate(addr);
}

uint16_t vm_read16(uint64_t addr) {
    uint16_t raw;
    memcpy(&raw, translate(addr), 2);
    return bswap16(raw);
}

uint32_t vm_read32(uint64_t addr) {
    uint32_t raw;
    memcpy(&raw, translate(addr), 4);
    return bswap32(raw);
}

uint64_t vm_read64(uint64_t addr) {
    uint64_t raw;
    memcpy(&raw, translate(addr), 8);
    return bswap64(raw);
}

void vm_write8(uint64_t addr, uint8_t val) {
    *translate(addr) = val;
}

void vm_write16(uint64_t addr, uint16_t val) {
    uint16_t raw = bswap16(val);
    memcpy(translate(addr), &raw, 2);
}

void vm_write32(uint64_t addr, uint32_t val) {
    uint32_t raw = bswap32(val);
    memcpy(translate(addr), &raw, 4);
}

void vm_write64(uint64_t addr, uint64_t val) {
    uint64_t raw = bswap64(val);
    memcpy(translate(addr), &raw, 8);
}

/* Syscall dispatch stub */
void lv2_syscall(void* ctx) {
    fprintf(stderr, "[recomp] lv2_syscall called (stub)\n");
}

} /* extern "C" */
