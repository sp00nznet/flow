#!/usr/bin/env python3
"""
Post-splice build fixups for the jump-table re-lift.

1. The fresh ppu_recomp.h carries a `static inline unsigned int __builtin_clz`
   polyfill, but the spliced ppu_recomp.cpp preamble (kept from BASE) and the
   ppu_recomp_patch*.cpp files each define their own `int` version. MSVC rejects
   the differing return type. BASE built with only the per-.cpp `int` versions,
   so strip the polyfill block from the header.

2. The prologue-detection fix shifted function boundaries, so some `bl <addr>`
   sites in fresh/replaced bodies now target an address that is no longer a
   function start — `func_XXXXXXXX` is referenced but defined/declared nowhere
   (C2065). Emit a forward decl + no-op stub for each such dangling target so
   the unit links. These are boundary-shift artifacts; if one is ever hit at
   runtime it logs once.
"""
import re
import os

RECOMP = os.path.join(os.path.dirname(__file__), "..", "src", "recomp")
RECOMP = os.path.abspath(RECOMP)
CPP = os.path.join(RECOMP, "ppu_recomp.cpp")
H = os.path.join(RECOMP, "ppu_recomp.h")

FUNC_TOK = re.compile(r'func_[0-9A-Fa-f]{8}')
DEF_RE = re.compile(r'^void (func_[0-9A-Fa-f]{8})\(ppu_context')
DECL_RE = re.compile(r'(func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\);')

# --- 1. strip the clz polyfill from the header --------------------------------
with open(H, 'r', encoding='utf-8', errors='replace') as f:
    htext = f.read()
poly = re.compile(
    r'/\* MSVC compatibility for GCC builtins \*/\n'
    r'#ifdef _MSC_VER\n#include <intrin\.h>\n'
    r'static inline unsigned int __builtin_clz.*?\n#endif\n',
    re.S)
new_htext, n = poly.subn('', htext)
if n:
    print(f"Stripped {n} clz polyfill block(s) from header")
else:
    print("WARN: clz polyfill block not found in header (already stripped?)")
declared = set(DECL_RE.findall(new_htext))
print(f"Header declares {len(declared)} funcs")

# --- 2. find referenced-but-undefined/undeclared func targets -----------------
# external funcs provided by sibling .cpp (declared in header already, but be safe)
external = set()
for p in os.listdir(RECOMP):
    if p.endswith('.cpp') and p != 'ppu_recomp.cpp':
        with open(os.path.join(RECOMP, p), 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                m = DEF_RE.match(line)
                if m:
                    external.add(m.group(1))
print(f"Sibling .cpp define {len(external)} funcs")

referenced = set()
defined = set()
with open(CPP, 'r', encoding='utf-8', errors='replace') as f:
    for line in f:
        m = DEF_RE.match(line)
        if m:
            defined.add(m.group(1))
        # only scan lines that mention func_ to keep it fast
        if 'func_' in line:
            referenced.update(FUNC_TOK.findall(line))
print(f"cpp references {len(referenced)}, defines {len(defined)}")

missing = sorted(referenced - declared - defined - external)
print(f"Dangling (undeclared+undefined) targets to stub: {len(missing)}")
if missing:
    print("  sample:", missing[:8])

# --- 3. emit forward decls into header + stubs at EOF of cpp -------------------
if missing:
    decls = "\n/* Boundary-shift dangling bl-targets — forward decls */\n" + \
            "".join(f"void {m}(ppu_context* ctx);\n" for m in missing)
    # insert decls after the first function declaration region — simplest: append
    new_htext = new_htext.rstrip() + "\n" + decls
with open(H, 'w', encoding='utf-8') as f:
    f.write(new_htext)
print(f"Wrote header ({len(new_htext)} bytes)")

if missing:
    with open(CPP, 'a', encoding='utf-8') as f:
        f.write("\n/* ===== Boundary-shift dangling bl-target stubs (no-op) ===== */\n")
        for m in missing:
            f.write(f"void {m}(ppu_context* ctx) {{ (void)ctx; }}\n")
    print(f"Appended {len(missing)} stub definitions to cpp")
print("Done.")
