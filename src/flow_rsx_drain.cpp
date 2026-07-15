/*
 * flOw — RSX command-buffer drain (bypass build)
 *
 * The bypass build doesn't run the draw-engine's boot_main vblank_ticker, so
 * nothing pumps the GCM FIFO into the D3D12 backend. The render injection
 * (ppu_recomp_003.cpp) writes a complete NV40 command stream — SET_SURFACE,
 * viewport, vertex-array binding, DRAW_ARRAYS — into the guest command buffer
 * in HOST-endian order, then flips. This drains that buffer directly through
 * the backend: rsx_process_command_buffer decodes the LE headers and dispatches
 * each method to rsx_process_method -> s_backend->draw_arrays (D3D12).
 *
 * rsx_process_command_buffer reads buf[pos] with no byteswap, matching the
 * injection's host-endian command writes (the vertex *data* is big-endian via
 * vm_write32, matching read_rsx_vertex's rd_bef byteswap).
 */
#include "libs/video/rsx_commands.h"
#include <cstdint>

extern "C" uint8_t* vm_base;
extern "C" uint32_t cellGcmResolveOffset(uint32_t offset);

extern "C" void flow_drain_gcm(uint32_t guest_buf_addr, uint32_t num_dwords)
{
    if (!vm_base || !guest_buf_addr || !num_dwords) return;
    static rsx_state s_state;
    static int s_inited = 0;
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; }
    const uint32_t* buf = (const uint32_t*)(vm_base + guest_buf_addr);
    /* One-time sanity: dump the first two vertices the backend will read
     * (guest 0xC2000000, big-endian float3 pos + ubyte4 color). */
    static int s_dumped = 0;
    if (!s_dumped) {
        s_dumped = 1;
        auto rdbe_f = [](uint32_t ea) {
            const uint8_t* p = vm_base + ea;
            uint32_t w = (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
            float f; __builtin_memcpy(&f, &w, 4); return f;
        };
        const uint8_t* c0 = vm_base + 0xC2000000u + 12;
        fprintf(stderr, "[DRAIN-DBG] v0 pos=(%.3f,%.3f,%.3f) col=%02X%02X%02X%02X | "
                        "v1 pos=(%.3f,%.3f,%.3f)\n",
                rdbe_f(0xC2000000u+0), rdbe_f(0xC2000000u+4), rdbe_f(0xC2000000u+8),
                c0[0], c0[1], c0[2], c0[3],
                rdbe_f(0xC2000000u+16), rdbe_f(0xC2000000u+20), rdbe_f(0xC2000000u+24));
        /* THE critical link: the backend fetches verts from
         * vm_base + cellGcmResolveOffset(attrib.offset). We WROTE at 0xC2000000
         * assuming resolve(0x02000000)==0xC2000000. Print what it really returns
         * and what the backend will actually read there. */
        {
            uint32_t ea = cellGcmResolveOffset(0x02000000u);
            fprintf(stderr, "[DRAIN-DBG] resolve(0x02000000) = 0x%08X (expect 0xC2000000) "
                            "-> backend reads v0 pos=(%.3f,%.3f,%.3f)\n",
                    ea, rdbe_f(ea + 0), rdbe_f(ea + 4), rdbe_f(ea + 8));
        }
        fflush(stderr);
    }
    rsx_process_command_buffer(&s_state, buf, num_dwords * 4);
}
