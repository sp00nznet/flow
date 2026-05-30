#!/usr/bin/env python3
"""
Post-lift processing for flOw recompiled code.

Run this after the lifter produces ppu_recomp.c and ppu_recomp.h to:
1. Rename .c to .cpp (needed for ps3types.h C++ templates)
2. Patch ppu_recomp.h to use runtime's ppu_context and vm_bridge
3. Apply fallthrough fix for split functions
4. Count stats

Usage:
    python tools/post_lift.py [--recomp-dir src/recomp]
"""

import argparse
import os
import re
import sys

def rename_c_to_cpp(recomp_dir: str) -> None:
    """Rename ppu_recomp.c to ppu_recomp.cpp."""
    c_path = os.path.join(recomp_dir, "ppu_recomp.c")
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    if os.path.isfile(c_path):
        if os.path.isfile(cpp_path):
            os.remove(cpp_path)
        os.rename(c_path, cpp_path)
        print(f"  Renamed ppu_recomp.c -> ppu_recomp.cpp")
    elif os.path.isfile(cpp_path):
        print(f"  ppu_recomp.cpp already exists")
    else:
        print(f"  ERROR: Neither .c nor .cpp found!", file=sys.stderr)
        sys.exit(1)


def patch_header(recomp_dir: str) -> None:
    """Patch ppu_recomp.h to use runtime ppu_context and add compatibility shims."""
    h_path = os.path.join(recomp_dir, "ppu_recomp.h")
    with open(h_path, "r") as f:
        content = f.read()

    # Replace lifter's ppu_context struct with runtime include
    # The lifter generates its own ppu_context; we replace it with the runtime's
    if "runtime/ppu/ppu_context.h" not in content:
        # Remove the lifter's struct definition if present
        content = re.sub(
            r'typedef struct ppu_context \{.*?\} ppu_context;',
            '/* Use runtime ppu_context for ABI compatibility */\n'
            '#include "runtime/ppu/ppu_context.h"',
            content, flags=re.DOTALL
        )

    # Ensure math.h and string.h are included (needed for sqrt, memcpy in lifted code)
    if "<math.h>" not in content:
        content = content.replace("#include <stdint.h>",
                                  "#include <stdint.h>\n#include <string.h>\n#include <math.h>")

    # Add MSVC __builtin_clz compatibility if not present
    if "__builtin_clz" not in content:
        insert_after = "#pragma once\n"
        msvc_compat = """
/* MSVC compatibility for GCC builtins */
#ifdef _MSC_VER
#include <intrin.h>
static inline unsigned int __builtin_clz(unsigned int x) {
    unsigned long index;
    if (x == 0) return 32;
    _BitScanReverse(&index, x);
    return 31 - index;
}
#endif
"""
        content = content.replace(insert_after, insert_after + msvc_compat)

    # Add extern "C" vm_read/vm_write declarations if not present
    if "vm_read8" not in content:
        # Find the end of includes
        insert_pos = content.find("\n\n", content.rfind("#include"))
        if insert_pos == -1:
            insert_pos = content.find("\n", content.rfind("#include"))
        vm_decls = """

/* Memory access helpers (implemented in vm_bridge.cpp with C linkage) */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t  vm_read8 (uint64_t addr);
uint16_t vm_read16(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
uint64_t vm_read64(uint64_t addr);
void     vm_write8 (uint64_t addr, uint8_t  val);
void     vm_write16(uint64_t addr, uint16_t val);
void     vm_write32(uint64_t addr, uint32_t val);
void     vm_write64(uint64_t addr, uint64_t val);
#ifdef __cplusplus
}
#endif

/* Syscall dispatch (from runtime) */
#include "runtime/syscalls/lv2_syscall_table.h"
"""
        content = content[:insert_pos] + vm_decls + content[insert_pos:]

    # Wrap function declarations in extern "C" if not already
    if 'extern "C" {' not in content:
        # Find first function declaration
        first_func = content.find("\nvoid func_")
        if first_func != -1:
            content = content[:first_func] + '\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n' + content[first_func:]
            # Add closing before the end
            content += '\n#ifdef __cplusplus\n}\n#endif\n'

    with open(h_path, "w") as f:
        f.write(content)
    print(f"  Patched ppu_recomp.h")


def insert_drain_trampoline(recomp_dir: str) -> int:
    """Add DRAIN_TRAMPOLINE(ctx) after every bl (func_XXX) call.

    The lifter now emits trampoline patterns for cross-fragment branches
    and fallthroughs. But mid-function bl calls are still direct calls.
    Each direct call may trigger a trampoline chain in the callee, so
    we need to drain after every call.

    Also adds the global trampoline variable and macro at the top.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "r", errors="replace") as f:
        lines = f.readlines()

    call_re = re.compile(r'^(\s+)(func_[0-9A-Fa-f]{8})\(ctx\);(\s*)$')
    new_lines = []
    added = 0

    # Add trampoline global and macro at top (after first #include)
    header_added = False
    for i, line in enumerate(lines):
        new_lines.append(line)
        if not header_added and line.strip().startswith('#include'):
            new_lines.append('extern "C" void (*g_trampoline_fn)(void*);\n')
            new_lines.append('#define DRAIN_TRAMPOLINE(ctx) do { \\\n')
            new_lines.append('    while (g_trampoline_fn) { \\\n')
            new_lines.append('        void(*_tf)(void*) = g_trampoline_fn; \\\n')
            new_lines.append('        g_trampoline_fn = 0; \\\n')
            new_lines.append('        _tf((void*)(ctx)); \\\n')
            new_lines.append('    } \\\n')
            new_lines.append('} while(0)\n\n')
            header_added = True
            continue

        m = call_re.match(line)
        if m:
            indent = m.group(1)
            new_lines.append(f'{indent}DRAIN_TRAMPOLINE(ctx);\n')
            added += 1

    with open(cpp_path, "w", errors="replace") as f:
        f.writelines(new_lines)

    print(f"  Added {added} DRAIN_TRAMPOLINE calls")
    return added


def apply_fallthrough_fix(recomp_dir: str) -> int:
    """Add fallthrough calls for split functions."""
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "r") as f:
        content = f.read()

    # Parse all function addresses
    func_pattern = re.compile(r'void (func_([0-9A-Fa-f]{8}))\(ppu_context\* ctx\) \{')
    func_positions = [(m.start(), m.group(1), int(m.group(2), 16)) for m in func_pattern.finditer(content)]
    func_positions.sort(key=lambda x: x[2])

    next_func = {}
    for i in range(len(func_positions) - 1):
        next_func[func_positions[i][1]] = func_positions[i + 1][1]

    lines = content.split('\n')
    new_lines = []
    modified = 0
    i = 0

    while i < len(lines):
        line = lines[i]
        new_lines.append(line)

        if line.strip() == '}' and i > 0:
            j = i - 1
            while j >= 0:
                prev = lines[j].strip()
                if prev and not prev.startswith('/*') and not prev.startswith('//') and prev != '{':
                    break
                j -= 1

            if j >= 0:
                prev_stmt = lines[j].strip().rstrip(';')
                needs_ft = True
                # Only skip fallthrough if the last statement UNCONDITIONALLY exits:
                # - bare return
                # - bare function call (not inside an if)
                # - bare goto (not inside an if)
                # - lv2_syscall (not inside an if)
                # Conditional exits (if (...) { func_X(); return; }) still need
                # fallthrough for the false path!
                is_conditional = prev_stmt.startswith('if ')
                if not is_conditional:
                    if (prev_stmt.startswith('return') or
                        prev_stmt.startswith('func_') or
                        prev_stmt.startswith('lv2_syscall') or
                        ('goto ' in prev_stmt)):
                        needs_ft = False

                if needs_ft:
                    for k in range(i, max(i - 200, -1), -1):
                        m = func_pattern.match(lines[k])
                        if m:
                            fname = m.group(1)
                            if fname in next_func:
                                ft_call = f'        {next_func[fname]}(ctx);'
                                new_lines.insert(-1, ft_call)
                                modified += 1
                            break
        i += 1

    with open(cpp_path, "w") as f:
        f.write('\n'.join(new_lines))

    print(f"  Added {modified} fallthrough calls")
    return modified


def patch_bctrl(recomp_dir: str) -> int:
    """Replace unsafe bctrl casts with ps3_indirect_call dispatch."""
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    old = b"((void(*)(ppu_context*))ctx->ctr)(ctx);"
    new = b"ps3_indirect_call(ctx);"
    count = data.count(old)

    if count > 0:
        data = data.replace(old, new)

        # Add declaration if not present
        decl = b'extern "C" void ps3_indirect_call(ppu_context* ctx);'
        if decl not in data:
            data = data.replace(
                b'#include "ppu_recomp.h"',
                b'#include "ppu_recomp.h"\nextern "C" void ps3_indirect_call(ppu_context* ctx);'
            )

        with open(cpp_path, "wb") as f:
            f.write(data)

    print(f"  Patched {count} bctrl calls -> ps3_indirect_call")
    return count


def patch_malloc(recomp_dir: str) -> int:
    """Patch the CRT malloc (func_006B738C) to call hle_guest_malloc."""
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    target = b"void func_006B738C(ppu_context* ctx) {"
    idx = data.find(target)
    if idx < 0:
        print("  func_006B738C not found — skipping malloc patch")
        return 0

    # Find the end of this function (next void func_ declaration)
    end = data.find(b"\nvoid func_", idx + 100)
    if end < 0:
        print("  Could not find end of func_006B738C")
        return 0

    # Replace with HLE malloc call
    new_func = (
        b"void func_006B738C(ppu_context* ctx) {\n"
        b"    /* PATCHED: HLE bump allocator malloc */\n"
        b"    extern \"C\" void hle_guest_malloc(ppu_context* ctx);\n"
        b"    hle_guest_malloc(ctx);\n"
        b"}\n"
    )

    # Add global declaration near top if not present
    decl = b'extern "C" void hle_guest_malloc(ppu_context* ctx);'
    if decl not in data[:10000]:
        data = data.replace(
            b'#include "ppu_recomp.h"',
            b'#include "ppu_recomp.h"\nextern "C" void hle_guest_malloc(ppu_context* ctx);'
        )

    data = data[:idx] + new_func + data[end:]

    with open(cpp_path, "wb") as f:
        f.write(data)

    print("  Patched func_006B738C -> hle_guest_malloc")
    return 1


def patch_crt_assert(recomp_dir: str) -> int:
    """Stub func_006B6C80, the CRT assertion-printer wrapper.

    Fires from inside fopen when an internal precondition (a CRT lock or
    FILE-table slot we never initialised because of the SPU bypass) fails.
    The default behaviour is to print "abort() is called from..." via
    sys_tty_write and then sys_process_exit(1) — which kills the redirect
    chain and forces the game-loop fallback.

    Stubbing it so fopen returns gracefully (returning 0/NULL up the call
    chain) lets the caller's own "if (fp == NULL) return" path handle the
    failure. Concretely this unblocks func_000CBFE4 -> func_0070C248
    (cellGcmInit), which currently never runs.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    target = b"void func_006B6C80(ppu_context* ctx) {"
    idx = data.find(target)
    if idx < 0:
        print("  func_006B6C80 not found — skipping crt-assert patch")
        return 0

    end = data.find(b"\nvoid func_", idx + 100)
    if end < 0:
        print("  Could not find end of func_006B6C80")
        return 0

    new_func = (
        b"void func_006B6C80(ppu_context* ctx) {\n"
        b"    /* PATCHED (wake-init): CRT abort-printer wrapper no-op.\n"
        b"     * See post_lift.patch_crt_assert. Return 0 in r3 so the\n"
        b"     * caller's NULL-check path handles the failure cleanly. */\n"
        b"    static int s_log = 0;\n"
        b"    if (s_log++ < 4) {\n"
        b"        fprintf(stderr, \"[WAKE-INIT] Skipping CRT assert (func_006B6C80) call #%d\\n\", s_log);\n"
        b"        fflush(stderr);\n"
        b"    }\n"
        b"    ctx->gpr[3] = 0;\n"
        b"}\n"
    )

    data = data[:idx] + new_func + data[end:]

    with open(cpp_path, "wb") as f:
        f.write(data)

    print("  Patched func_006B6C80 -> wake-init no-op (CRT assert)")
    return 1


def patch_wake_init(recomp_dir: str) -> int:
    """Stub func_006CDE50 (the std::cin/cout/cerr/clog stream binder).

    Multiple ios_base::Init wrappers (one per TU using <iostream>) call
    this function. It tries to bind the standard streams to stdin/stdout/
    stderr file descriptors that don't exist in our recompiled environment,
    sets failbit, and the unhandled std::ios_base::failure exception aborts
    the CRT — which is how we ended up needing the SPU/longjmp bypass.

    Game code never reads from cin or writes to cout (it uses PhyreEngine
    logging), so leaving the streams unbound is safe. Stubbing this one
    function lets static-init complete normally and unblocks deeper init
    (cellSysmodule loads, engine construction, etc.). See
    memory/project_flow_recomp.md "Proper-Init Wake" for the trace.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    target = b"void func_006CDE50(ppu_context* ctx) {"
    idx = data.find(target)
    if idx < 0:
        print("  func_006CDE50 not found — skipping wake-init patch")
        return 0

    end = data.find(b"\nvoid func_", idx + 100)
    if end < 0:
        print("  Could not find end of func_006CDE50")
        return 0

    new_func = (
        b"void func_006CDE50(ppu_context* ctx) {\n"
        b"    /* PATCHED (wake-init): stream binder no-op. See post_lift.patch_wake_init. */\n"
        b"    static int s_log = 0;\n"
        b"    if (s_log++ == 0) {\n"
        b"        fprintf(stderr, \"[WAKE-INIT] Skipping stream binder (func_006CDE50)\\n\");\n"
        b"        fflush(stderr);\n"
        b"    }\n"
        b"    ctx->gpr[3] = 0;\n"
        b"}\n"
    )

    data = data[:idx] + new_func + data[end:]

    with open(cpp_path, "wb") as f:
        f.write(data)

    print("  Patched func_006CDE50 -> wake-init no-op (skips std stream binding)")
    return 1


def patch_worker_wake(recomp_dir: str) -> int:
    """Inject a one-shot func_000A7944 call into func_000C858C's pre-loop init.

    func_000A7944 is the subsystem worker-pool spawner: lazy-init flag at
    *(TOC-0x5CE4), then iterates a count and calls the thread-create
    wrapper func_0007163C N times. Each spawned thread bootstraps via
    func_00071F38 which bctrls through an OPD held in the desc block.

    ps3recomp's sys_ppu_thread_create + g_ppu_thread_entry_trampoline is
    already wired (main.cpp installs ps3_thread_entry), so each spawn
    produces a real host thread running the guest entry.

    The natural caller chain that invokes func_000A7944 is gated by init
    we haven't woken yet. Calling it explicitly from the per-frame loop's
    one-shot setup primes the manager — the first call takes the
    "uninit" path (creates 4 lwmutex pairs, allocs the manager state),
    sets the init flag, and returns. On its first invocation the work
    queue is empty so no thread spawns yet, but the infrastructure is
    now in place for whatever code path later pushes work items.

    We anchor the insertion right after the SPURS-SEED block end and
    before the "Minimal render context" block in func_000C858C.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    # Anchor: the comment that opens the render-context init block.
    anchor = b"            /* Minimal render context (func_000E2BE0 stuck in data loop)."
    idx = data.find(anchor)
    if idx < 0:
        print("  Render-context anchor not found — skipping worker-wake patch")
        return 0

    # Don't double-patch
    if b"[WORKER-WAKE]" in data:
        print("  Worker-wake injection already present")
        return 0

    injection = (
        b"            /* Wake the subsystem worker-thread pool (one-shot, SEH-wrapped).\n"
        b"             *\n"
        b"             * func_000A7944 is a subsystem worker-pool spawner: lazy-init\n"
        b"             * flag at *(TOC-0x5CE4), allocates a manager + 4 lwmutex\n"
        b"             * objects, then enters a spawn loop that calls the thread-create\n"
        b"             * wrapper func_0007163C for each pending work item. The natural\n"
        b"             * caller chain is gated by init we haven't woken; invoking it\n"
        b"             * here primes the manager so any later code path that pushes\n"
        b"             * work items causes real PPU threads to spawn via the wired\n"
        b"             * sys_ppu_thread_create + ps3_thread_entry trampoline.\n"
        b"             *\n"
        b"             * On entry the first call takes the uninit branch (alloc + 4\n"
        b"             * lwmutex pairs + atexit reg + branch back to the function top),\n"
        b"             * sets the init-flag byte, and returns without spawning since\n"
        b"             * the work queue is empty. */\n"
        b"            {\n"
        b"                static int s_workers_woken = 0;\n"
        b"                static int s_workers_disabled = 0;\n"
        b"                if (!s_workers_woken && !s_workers_disabled) {\n"
        b"                    s_workers_woken = 1;\n"
        b"                    fprintf(stderr, \"[WORKER-WAKE] Calling func_000A7944 to spawn worker pool\\n\");\n"
        b"                    fflush(stderr);\n"
        b"                    __try {\n"
        b"                        ppu_context wctx = *ctx;\n"
        b"                        wctx.gpr[3] = (uint32_t)ctx->gpr[3]; /* engine instance */\n"
        b"                        wctx.gpr[2] = 0x008969A8;            /* TOC base */\n"
        b"                        func_000A7944(&wctx);\n"
        b"                        DRAIN_TRAMPOLINE(&wctx);\n"
        b"                        fprintf(stderr, \"[WORKER-WAKE] func_000A7944 returned cleanly\\n\");\n"
        b"                        fflush(stderr);\n"
        b"                    } __except (EXCEPTION_EXECUTE_HANDLER) {\n"
        b"                        s_workers_disabled = 1;\n"
        b"                        fprintf(stderr, \"[WORKER-WAKE] func_000A7944 threw \xe2\x80\x94 disabling further attempts\\n\");\n"
        b"                        fflush(stderr);\n"
        b"                    }\n"
        b"                }\n"
        b"            }\n"
        b"\n"
    )

    data = data[:idx] + injection + data[idx:]

    with open(cpp_path, "wb") as f:
        f.write(data)

    print("  Patched func_000C858C -> one-shot func_000A7944 worker-wake")
    return 1


def patch_state_probe(recomp_dir: str) -> int:
    """Inject a one-shot probe + (disabled) vtable-method sweep into func_000C858C.

    A C++ class with vtable at 0x0085C8D0 has 8 statically constructed
    instances in seg3 BSS (0x100802A8, 0x10083C70, 0x10086440, 0x100864B8,
    0x10086530, 0x100865A8, 0x10086620, 0x10086698) — all alive after CRT
    static-init, all with valid substate pointers at +0x38 / +0x50..+0x58.

    Vtable-method sweep findings (one-shot call on instance [0], r4=0):
      vt[0] func_0026A7BC: returns cleanly; HEAVIEST — sets a state that
                           makes the engine vt[3] tick do extra subsystem work.
      vt[1] func_0026AAF4: update method; reads a float at r4+0x1C so it
                           NEEDS a valid 2nd arg (frame/render context).
                           With r4=0 it walks into a NULL OPD at 0xD00000
                           and the spin-escape aborts.
      vt[2] func_0026AE3C: LIGHTWEIGHT tick — transform/vector compute
                           (copies position fields, fsubs deltas), returns
                           cleanly with just `this`. The closest thing to a
                           pure per-frame tick among the four.
      vt[3] func_0026AF6C: bigger update; same r4-context requirement as vt[1].

    IMPORTANT CORRECTION: this class is NOT the HddGameCheck title-state path.
    The cellHddGameCheck call at func_001392F0 (gated by
    sys_memory_container_create success at func_00138B7C) is separate code
    in the 0x138xxx region. The vt[2] math (positions + deltas) suggests
    these 8 instances are render/transform objects, not the init state machine.

    The block logs each instance once. The actual vt dispatch is gated
    behind VT_PROBE_IDX (default -1 = disabled, restores full FPS). Set it
    to 0..3 at compile time to re-run the sweep.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    anchor = b"            /* Minimal render context (func_000E2BE0 stuck in data loop)."
    idx = data.find(anchor)
    if idx < 0:
        print("  Render-context anchor not found — skipping state-probe patch")
        return 0

    if b"[STATE-PROBE]" in data:
        print("  State-probe injection already present")
        return 0

    injection = (
        b"            /* One-shot probe of the 0x0085C8D0-vtable class instances\n"
        b"             * plus an optional vtable-method sweep (VT_PROBE_IDX, default\n"
        b"             * -1 = disabled). See tools/post_lift.patch_state_probe for the\n"
        b"             * per-method findings. */\n"
        b"            {\n"
        b"                static int s_probed = 0;\n"
        b"                if (!s_probed) {\n"
        b"                    s_probed = 1;\n"
        b"                    static const uint32_t kStateInstances[] = {\n"
        b"                        0x100802A8, 0x10083C70, 0x10086440, 0x100864B8,\n"
        b"                        0x10086530, 0x100865A8, 0x10086620, 0x10086698,\n"
        b"                    };\n"
        b"                    fprintf(stderr, \"[STATE-PROBE] Scanning 0x0085C8D0-vtable class instances:\\n\");\n"
        b"                    for (unsigned i = 0; i < sizeof(kStateInstances)/sizeof(kStateInstances[0]); i++) {\n"
        b"                        uint32_t inst = kStateInstances[i];\n"
        b"                        uint32_t vt = vm_read32(inst);\n"
        b"                        uint32_t off38 = vm_read32(inst + 0x38);\n"
        b"                        fprintf(stderr, \"  [%u] 0x%08X: vtable=0x%08X substate=0x%08X%s\\n\",\n"
        b"                                i, inst, vt, off38,\n"
        b"                                (vt == 0x0085C8D0) ? \"\" : \" (UNEXPECTED)\");\n"
        b"                    }\n"
        b"                    uint32_t globals = vm_read32(0x008969A8 - 0x4B9C);\n"
        b"                    fprintf(stderr, \"[STATE-PROBE] *(TOC-0x4B9C)=0x%08X (class globals)\\n\", globals);\n"
        b"                    fflush(stderr);\n"
        b"\n"
        b"                    /* Optional vtable-method sweep. vt[2] (func_0026AE3C) is\n"
        b"                     * the lightweight tick; vt[1]/vt[3] need a valid render\n"
        b"                     * context in r4. Default disabled to keep full FPS. */\n"
        b"                    #ifndef VT_PROBE_IDX\n"
        b"                    #define VT_PROBE_IDX (-1)\n"
        b"                    #endif\n"
        b"                    if (VT_PROBE_IDX >= 0) {\n"
        b"                        extern void func_0026A7BC(ppu_context*);  /* vt[0] */\n"
        b"                        extern void func_0026AAF4(ppu_context*);  /* vt[1] */\n"
        b"                        extern void func_0026AE3C(ppu_context*);  /* vt[2] */\n"
        b"                        extern void func_0026AF6C(ppu_context*);  /* vt[3] */\n"
        b"                        typedef void (*ppu_fn)(ppu_context*);\n"
        b"                        ppu_fn methods[] = { func_0026A7BC, func_0026AAF4,\n"
        b"                                             func_0026AE3C, func_0026AF6C };\n"
        b"                        fprintf(stderr, \"[VT-SWEEP] Calling vt[%d] on inst[0]\\n\", VT_PROBE_IDX);\n"
        b"                        fflush(stderr);\n"
        b"                        __try {\n"
        b"                            ppu_context tctx = *ctx;\n"
        b"                            tctx.gpr[3] = 0x100802A8;\n"
        b"                            tctx.gpr[4] = 0;\n"
        b"                            tctx.gpr[2] = 0x008969A8;\n"
        b"                            methods[VT_PROBE_IDX & 3](&tctx);\n"
        b"                            DRAIN_TRAMPOLINE(&tctx);\n"
        b"                            fprintf(stderr, \"[VT-SWEEP] vt[%d] returned cleanly\\n\", VT_PROBE_IDX);\n"
        b"                            fflush(stderr);\n"
        b"                        } __except (EXCEPTION_EXECUTE_HANDLER) {\n"
        b"                            fprintf(stderr, \"[VT-SWEEP] vt[%d] threw\\n\", VT_PROBE_IDX);\n"
        b"                            fflush(stderr);\n"
        b"                        }\n"
        b"                    }\n"
        b"                }\n"
        b"            }\n"
        b"\n"
    )

    data = data[:idx] + injection + data[idx:]

    with open(cpp_path, "wb") as f:
        f.write(data)

    print("  Patched func_000C858C -> state probe + disabled vtable sweep")
    return 1


def patch_papp_probes(recomp_dir: str) -> int:
    """Persistent entry-probes on the PhyreEngine PApplication lifecycle.

    Ghidra-recovered names (ps3recomp 1a32c8a):
      func_000CF0DC = PApplication::Init
      func_000CF310 = PApplication::onInit
      func_000CF890 = PApplication::Frame   <- per-frame app loop

    These are NEVER reached in our recomp — the natural construction that
    creates the PApplication instance and starts its main loop is gated by
    init we skip. Confirmed empirically: probes never fired in a 12 s run.

    Adds a rate-limited (first 3 entries each) fprintf at the top of each so
    a future regen or wake attempt can confirm when/if PApplication actually
    starts running, without flooding the log.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    if b"[PAPP] Frame entered" in data:
        print("  PApplication probes already present")
        return 0

    targets = [
        (b"void func_000CF0DC(ppu_context* ctx) {",   "Init"),
        (b"void func_000CF310(ppu_context* ctx) {",   "onInit"),
        (b"void func_000CF890(ppu_context* ctx) {",   "Frame"),
    ]
    patched = 0
    for header, name in targets:
        idx = data.find(header)
        if idx < 0:
            print(f"  PApplication::{name} signature not found")
            continue
        insert_at = idx + len(header)
        probe = (
            b"\n        { static int s=0; if (s++ < 3) { "
            b"fprintf(stderr, \"[PAPP] " + name.encode() +
            b" entered #%d this=0x%08X\\n\", s, (uint32_t)ctx->gpr[3]); "
            b"fflush(stderr); } }"
        )
        data = data[:insert_at] + probe + data[insert_at:]
        patched += 1

    with open(cpp_path, "wb") as f:
        f.write(data)

    print(f"  Patched {patched} PApplication lifecycle probes")
    return patched


def patch_papp_scan(recomp_dir: str) -> int:
    """One-shot BSS/data scan in func_000C858C for PApplication instances.

    Looks for any 4-byte slot in seg3 (0x10000000..0x10112000) whose value
    matches a candidate PApplication-class vtable address. Confirmed in
    this session: zero hits, i.e., no PApplication instance has been
    constructed anywhere we reach. The probe stays in place as a
    regression check — the moment a future fix wakes the construction
    path, the scan will report the new instance addresses.
    """
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    with open(cpp_path, "rb") as f:
        data = f.read()

    anchor = b"            /* Minimal render context (func_000E2BE0 stuck in data loop)."
    idx = data.find(anchor)
    if idx < 0:
        print("  Render-context anchor not found - skipping PApplication scan")
        return 0
    if b"[PAPP-SCAN]" in data:
        print("  PApplication scan injection already present")
        return 0

    injection = (
        b"            {\n"
        b"                static int s_papp_scanned = 0;\n"
        b"                if (!s_papp_scanned) {\n"
        b"                    s_papp_scanned = 1;\n"
        b"                    uint32_t vt_candidates[] = {0x1006E5F8, 0x10070338, 0x10070368};\n"
        b"                    int total = 0;\n"
        b"                    fprintf(stderr, \"[PAPP-SCAN] Scanning seg3 for PApplication vtable refs:\\n\");\n"
        b"                    for (uint32_t a = 0x10000000; a < 0x10112000 && total < 50; a += 4) {\n"
        b"                        uint32_t v = vm_read32(a);\n"
        b"                        for (unsigned i = 0; i < sizeof(vt_candidates)/sizeof(vt_candidates[0]); i++) {\n"
        b"                            if (v == vt_candidates[i]) {\n"
        b"                                fprintf(stderr, \"  inst @ 0x%08X -> vt 0x%08X\\n\", a, v);\n"
        b"                                total++; break;\n"
        b"                            }\n"
        b"                        }\n"
        b"                    }\n"
        b"                    fprintf(stderr, \"[PAPP-SCAN] %d candidate instance(s) found\\n\", total);\n"
        b"                    fflush(stderr);\n"
        b"                }\n"
        b"            }\n"
        b"\n"
    )

    data = data[:idx] + injection + data[idx:]
    with open(cpp_path, "wb") as f:
        f.write(data)
    print("  Patched func_000C858C -> PApplication BSS scan")
    return 1


def print_stats(recomp_dir: str) -> None:
    """Print statistics about the recompiled code."""
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    h_path = os.path.join(recomp_dir, "ppu_recomp.h")

    cpp_size = os.path.getsize(cpp_path) if os.path.isfile(cpp_path) else 0
    h_size = os.path.getsize(h_path) if os.path.isfile(h_path) else 0

    with open(cpp_path, "r") as f:
        content = f.read()

    func_count = content.count("void func_")
    todo_count = content.count("/* TODO:")

    print(f"\n  Stats:")
    print(f"    Source size: {cpp_size / 1024 / 1024:.1f} MB")
    print(f"    Header size: {h_size / 1024:.0f} KB")
    print(f"    Functions: {func_count}")
    print(f"    TODO instructions: {todo_count}")


def main():
    parser = argparse.ArgumentParser(description="Post-lift processing for flOw")
    parser.add_argument("--recomp-dir", default="src/recomp",
                        help="Recompiled code directory (default: src/recomp)")
    args = parser.parse_args()

    recomp_dir = os.path.abspath(args.recomp_dir)
    print(f"Post-lift processing: {recomp_dir}")

    print("\n1. Renaming .c -> .cpp")
    rename_c_to_cpp(recomp_dir)

    print("\n2. Patching header")
    patch_header(recomp_dir)

    print("\n3. Inserting trampoline drain after bl calls")
    insert_drain_trampoline(recomp_dir)

    print("\n4. Patching bctrl indirect calls")
    patch_bctrl(recomp_dir)

    print("\n5. Patching malloc -> HLE bump allocator")
    patch_malloc(recomp_dir)

    print("\n6. Patching wake-init (stub stream binder)")
    patch_wake_init(recomp_dir)

    print("\n7. Patching CRT assert wrapper (wake fopen)")
    patch_crt_assert(recomp_dir)

    print("\n8. Patching worker-wake (one-shot func_000A7944 in func_000C858C)")
    patch_worker_wake(recomp_dir)

    print("\n9. Patching state-machine probe (diagnostic dump of 8 BSS instances)")
    patch_state_probe(recomp_dir)

    print("\n10. Patching PApplication lifecycle probes (Init/onInit/Frame)")
    patch_papp_probes(recomp_dir)

    print("\n11. Patching PApplication BSS scan (one-shot instance search)")
    patch_papp_scan(recomp_dir)

    print_stats(recomp_dir)
    print("\nDone! Ready to build.")


if __name__ == "__main__":
    main()
