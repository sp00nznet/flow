#!/usr/bin/env python3
"""
Generate missing_stubs.cpp for functions called but not defined.

Usage:
    python tools/gen_stubs.py [--recomp-dir src/recomp]
"""

import argparse
import os
import re


def main():
    parser = argparse.ArgumentParser(description="Generate missing stubs")
    parser.add_argument("--recomp-dir", default="src/recomp")
    args = parser.parse_args()

    recomp_dir = os.path.abspath(args.recomp_dir)
    cpp_path = os.path.join(recomp_dir, "ppu_recomp.cpp")
    h_path = os.path.join(recomp_dir, "ppu_recomp.h")
    stubs_path = os.path.join(recomp_dir, "missing_stubs.cpp")
    imports_path = os.path.join(recomp_dir, "import_stubs.cpp")

    print(f"Scanning {cpp_path}...")

    with open(cpp_path, "r") as f:
        content = f.read()

    called = set(re.findall(r'(func_[0-9A-Fa-f]{8})\(ctx\)', content))
    defined = set(re.findall(r'void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\)', content))

    with open(h_path, "r") as f:
        header = f.read()
    declared = set(re.findall(r'void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\)', header))

    import_defined = set()
    if os.path.isfile(imports_path):
        with open(imports_path, "r") as f:
            import_defined = set(re.findall(
                r'void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\)',
                f.read()))

    available = defined | declared | import_defined
    missing = sorted(called - available)

    print(f"  Called: {len(called)}")
    print(f"  Defined: {len(defined)}")
    print(f"  Declared: {len(declared)}")
    print(f"  Import stubs: {len(import_defined)}")
    print(f"  Missing: {len(missing)}")

    # Generate stubs
    with open(stubs_path, "w", newline="\n") as f:
        f.write('#include "ppu_recomp.h"\n')
        f.write('#include <stdio.h>\n\n')
        f.write('static int g_stub_calls = 0;\n')
        f.write('static void stub_hit(const char* name) {\n')
        f.write('    if (g_stub_calls < 100) {\n')
        f.write('        fputs("[STUB] ", stderr);\n')
        f.write('        fputs(name, stderr);\n')
        f.write('        fputc(10, stderr);\n')
        f.write('        g_stub_calls++;\n')
        f.write('    }\n')
        f.write('}\n\n')
        for name in missing:
            f.write(f'void {name}(ppu_context* ctx) {{ stub_hit("{name}"); }}\n')

    # Add declarations to header
    with open(h_path, "r") as f:
        h = f.read()

    new_decls = "\n".join(f"void {name}(ppu_context* ctx);" for name in missing)
    if '#ifdef __cplusplus\n}\n#endif' in h:
        h = h.replace('#ifdef __cplusplus\n}\n#endif',
                       new_decls + '\n\n#ifdef __cplusplus\n}\n#endif')
    else:
        h += "\n" + new_decls + "\n"

    with open(h_path, "w") as f:
        f.write(h)

    print(f"  Generated {len(missing)} stubs in {stubs_path}")
    print(f"  Added {len(missing)} declarations to {h_path}")


if __name__ == "__main__":
    main()
