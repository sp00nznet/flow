#!/usr/bin/env python3
"""
VMX re-lift splice.

The working ppu_recomp.cpp carries extensive hand-edits (engine-run frame
loop, scene builder, SPURS seed, render dispatch, ctor hooks, diagnostics)
across a known set of functions, plus file-scope declarations/helpers in the
preamble. A clean re-lift would lose all of that.

This script takes:
  - BASE: the working hand-edited ppu_recomp.cpp (src/recomp.bak), and
  - FRESH: a freshly re-lifted + post_lift'd ppu_recomp.cpp (correct VMX),
and produces an output that is BASE with each NON-hand-edited function body
replaced by FRESH's version when (and only when) they differ. Hand-edited
functions, the preamble, and the trailing tables are taken verbatim from BASE.

Both inputs must have had the SAME post_lift transformations applied, so the
only per-function differences are the corrected VMX opcodes.
"""
import re
import sys

# Functions with hand-edits — preserved verbatim from BASE. Derived from the
# custom [TAG] log-marker audit of the working ppu_recomp.cpp.
PRESERVE = {
    "func_000C858C",  # engine-run frame loop, scene builder, SPURS seed, probes
    "func_000C8590",  # GAME-LOOP tail-entry adjacent to 858C
    "func_000DACA8",  # BEGINFRAME hook
    "func_0010012C",  # CTOR-UI hook
    "func_00816450",  # CTOR walker hook
    "func_000CB9D0",  # GAME-MAIN / SUBSYS
    "func_000CBF4C",  # GAME (engine_run wrapper)
    "func_006BA00C",  # MEMCPY diagnostic
    "func_006BB970",  # MEMSET diagnostic
    "func_000CBF94",  # PROLOGUE / VERIFY
    "func_000DBD6C",  # RENDER-DISPATCH
    "func_000C91F0",  # SUBSYS
    "func_000CBFE4",  # TRACE
    "func_006B9720",  # TRACE
    "func_0070C248",  # TRACE (cellGcmInit path)
    "func_000CC85C",  # VERIFY
    "func_000C6CB8",  # VTABLE
    "func_006CBD1C",  # FAILBIT-THROW diagnostic
    # Wake stubs (post_lift handles these identically in FRESH, but preserve
    # from BASE to be safe — they're tiny stubs either way):
    "func_006B6C80",  # WAKE-INIT crt-assert stub
    "func_006CDE50",  # WAKE-INIT stream-binder stub
}

FUNC_RE = re.compile(r'^void (func_\w+)\(ppu_context\* ctx\) \{')


def split_segments(text):
    """Parse into an ordered list of segments.

    Each segment is either:
      ('func', name, block)  — a 'void func_X(ppu_context* ctx) {' ... '}' body
      ('raw', text)          — preamble, interspersed hand-edit decls/helpers,
                               and the trailing tables (everything non-function)

    Function bodies close at the first line that is exactly '}' (column 0).
    This handles hand-edited declaration/helper blocks interspersed between
    functions (e.g. the FlowLevel forward-decl before func_000C858C).
    """
    lines = text.split('\n')
    n = len(lines)
    segs = []
    raw = []
    i = 0
    while i < n:
        m = FUNC_RE.match(lines[i])
        if not m:
            raw.append(lines[i]); i += 1; continue
        if raw:
            segs.append(('raw', '\n'.join(raw))); raw = []
        name = m.group(1)
        start = i
        # Single-line function: post_lift's malloc stub etc. are written as
        # `void X(ctx) { ... }` on one line, with '}' NOT at column 0. Don't
        # scan past it or we'd eat the following function(s).
        if lines[i].rstrip().endswith('}'):
            segs.append(('func', name, lines[i]))
            i += 1
            continue
        i += 1
        while i < n and lines[i] != '}':
            # A new top-level function before we found our close means the
            # previous function was malformed; stop here so we don't swallow it.
            if FUNC_RE.match(lines[i]):
                break
            i += 1
        if i < n and lines[i] == '}':
            i += 1
        segs.append(('func', name, '\n'.join(lines[start:i])))
    if raw:
        segs.append(('raw', '\n'.join(raw)))
    return segs


def func_map(text):
    m = {}
    for seg in split_segments(text):
        if seg[0] == 'func':
            m[seg[1]] = seg[2]
    return m


def load_names(path):
    """Load ghidra_names.py output -> {int_addr: label}."""
    import json
    with open(path, 'r', encoding='utf-8') as f:
        raw = json.load(f)
    out = {}
    for k, v in raw.items():
        try:
            a = int(str(k), 16)
        except ValueError:
            continue
        label = v.get('label') if isinstance(v, dict) else str(v)
        if label:
            out[a] = label
    return out


def collect_external_funcs(out_path):
    """Functions DEFINED in sibling .cpp files of the output dir.

    ppu_recomp_patch*.cpp, missing_stubs.cpp, trampoline_stubs.cpp and
    import_stubs.cpp each provide hand-edited / generated bodies for a set of
    func_XXXX that are deliberately NOT emitted into ppu_recomp.cpp. The fresh
    lifter re-emits some of them, so they must be excluded from the spliced
    output or the link fails with duplicate-symbol errors. Scan the siblings
    (everything in the out dir except the file we're writing) for definitions.
    """
    import os, glob
    out_dir = os.path.dirname(os.path.abspath(out_path))
    out_base = os.path.basename(out_path)
    def_re = re.compile(r'^\s*void\s+(func_[0-9A-Fa-f]+)\s*\(\s*ppu_context', re.M)
    provided = set()
    files = []
    for p in glob.glob(os.path.join(out_dir, '*.cpp')):
        if os.path.basename(p) == out_base:
            continue
        files.append(os.path.basename(p))
        with open(p, 'r', encoding='utf-8', errors='replace') as f:
            provided.update(def_re.findall(f.read()))
    print(f"External-provided funcs from {files}: {len(provided)} (excluded from output)")
    return provided


def main():
    base_path, fresh_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    names_path = sys.argv[4] if len(sys.argv) > 4 else None
    with open(base_path, 'r', encoding='utf-8', errors='replace') as f:
        base = f.read()
    with open(fresh_path, 'r', encoding='utf-8', errors='replace') as f:
        fresh = f.read()

    external = collect_external_funcs(out_path)

    names = load_names(names_path) if names_path else {}
    if names:
        print(f"Loaded {len(names)} recovered names for annotation")

    base_segs = split_segments(base)
    fresh_map = func_map(fresh)

    base_names = {s[1] for s in base_segs if s[0] == 'func'}
    fresh_names = set(fresh_map)
    print(f"BASE funcs: {len(base_names)}  FRESH funcs: {len(fresh_names)}")
    print(f"  only in BASE:  {len(base_names - fresh_names)}  (kept verbatim)")
    print(f"  only in FRESH: {len(fresh_names - base_names)}  (APPENDED at EOF)")
    sample_ob = sorted(base_names - fresh_names)[:8]
    if sample_ob:
        print("  sample only-BASE:", sample_ob)

    addr_re = re.compile(r'func_([0-9A-Fa-f]+)')
    annotated = 0

    def annotate(name, block):
        nonlocal annotated
        m = addr_re.match(name)
        if not m:
            return block
        try:
            a = int(m.group(1), 16)
        except ValueError:
            return block
        label = names.get(a)
        if label and "/* " + label + " */" not in block:
            annotated += 1
            return f"/* {label} */\n{block}"
        return block

    replaced = preserved_he = kept_same = no_fresh = 0
    out_parts = []
    for seg in base_segs:
        if seg[0] == 'raw':
            out_parts.append(seg[1]); continue
        name, base_block = seg[1], seg[2]
        if name in PRESERVE:
            out_parts.append(annotate(name, base_block)); preserved_he += 1; continue
        fresh_block = fresh_map.get(name)
        if fresh_block is None:
            out_parts.append(annotate(name, base_block)); no_fresh += 1; continue
        if fresh_block != base_block:
            out_parts.append(annotate(name, fresh_block)); replaced += 1
        else:
            out_parts.append(annotate(name, base_block)); kept_same += 1

    # Newly-discovered functions (jump-table case targets, prologue-recovered
    # entries) exist only in FRESH. They are referenced by FRESH's func_table.cpp
    # (g_recompiled_funcs) and by bl-targets inside replaced FRESH bodies, and
    # declared in FRESH's ppu_recomp.h — so they MUST be emitted or the link
    # breaks with undefined func_XXXX symbols. Append them at EOF (valid C++:
    # the header provides their prototypes ahead of any reference).
    appended = 0
    excluded_ext = sorted((fresh_names - base_names) & external)
    only_fresh = sorted((fresh_names - base_names) - external)
    if excluded_ext:
        print(f"  of only-FRESH, excluded as externally-provided: {len(excluded_ext)}")
    if only_fresh:
        out_parts.append(
            "\n/* ===== Functions newly discovered by the re-lift "
            "(jump-table case targets + prologue-recovered), absent from BASE. "
            "Appended so func_table.cpp / bl-targets resolve. ===== */")
        for name in only_fresh:
            out_parts.append(annotate(name, fresh_map[name]))
            appended += 1

    out = '\n'.join(out_parts)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(out)
    print(f"\nReplaced (VMX-corrected) bodies: {replaced}")
    print(f"Preserved hand-edited functions: {preserved_he}")
    print(f"Unchanged functions kept:        {kept_same}")
    print(f"Functions absent from FRESH kept: {no_fresh}")
    print(f"New FRESH-only functions appended: {appended}")
    print(f"Name comments annotated:         {annotated}")
    print(f"Wrote {out_path} ({len(out)} bytes)")


if __name__ == '__main__':
    main()
