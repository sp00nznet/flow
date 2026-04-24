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

    print_stats(recomp_dir)
    print("\nDone! Ready to build.")


if __name__ == "__main__":
    main()
