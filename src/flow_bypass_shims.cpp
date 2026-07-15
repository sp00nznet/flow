/* flow_bypass_shims.cpp — provide symbols the current ps3-draw runtime lib
 * references but the (older) bypass harness never defined. The honest build gets
 * these from ppu_hle.cpp/ppu_loader.cpp, which the bypass replaces with its own
 * vm_bridge/hle_modules. The 3 diagnostics are no-ops; ps3_hle_call is only
 * reached from the lib (the lifted chunks don't call it — the bypass routes
 * imports through its own stub table), so log-and-continue is safe. */
#include <cstdint>
#include <cstdio>
struct ppu_context;
extern "C" {
    void ppu_guard_page(uint32_t ea) { (void)ea; }
    void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
    uint32_t ppu_active_lr(void) { return 0; }
    void ps3_hle_call(uint32_t nid, ppu_context* ctx) {
        (void)ctx;
        static int n = 0;
        if (n++ < 40) fprintf(stderr, "[shim] ps3_hle_call nid=0x%08X (bypass HLE handles imports)\n", nid);
    }
}
