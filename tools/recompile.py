#!/usr/bin/env python3
"""
flOw recompilation pipeline -- master script

Chains together the ps3recomp tools to transform EBOOT.elf into compilable C:
  1. Parse ELF and extract code segments
  2. Find function boundaries
  3. Lift PPU assembly to C source code
  4. Generate the function table for the runtime

Usage:
    python tools/recompile.py [--config config.toml] [--elf game/EBOOT.elf]

Requires ps3recomp tools in ../ps3/tools/ (or set PS3RECOMP_DIR).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)

DEFAULT_ELF = os.path.join(PROJECT_DIR, "game", "EBOOT.elf")
DEFAULT_CONFIG = os.path.join(PROJECT_DIR, "config.toml")
DEFAULT_OUTPUT = os.path.join(PROJECT_DIR, "src", "recomp")
DEFAULT_PS3RECOMP = os.path.join(PROJECT_DIR, "..", "ps3")


def find_ps3recomp_tools(ps3recomp_dir: str) -> str:
    """Locate the ps3recomp tools directory."""
    tools_dir = os.path.join(ps3recomp_dir, "tools")
    if os.path.isdir(tools_dir):
        return tools_dir
    raise FileNotFoundError(
        f"ps3recomp tools not found at {tools_dir}\n"
        f"Set PS3RECOMP_DIR or place ps3recomp at {DEFAULT_PS3RECOMP}"
    )


def load_config(config_path: str) -> dict:
    """Load TOML config if available."""
    if not os.path.isfile(config_path):
        return {}
    try:
        import tomli
    except ImportError:
        try:
            import tomllib as tomli  # Python 3.11+
        except ImportError:
            print("Warning: tomli not installed, skipping config.toml", file=sys.stderr)
            return {}
    with open(config_path, "rb") as f:
        return tomli.load(f)


def run_tool(python: str, script: str, args: list[str], desc: str) -> subprocess.CompletedProcess:
    """Run a ps3recomp tool as a subprocess."""
    cmd = [python, script] + args
    print(f"\n{'='*60}")
    print(f"  {desc}")
    print(f"  {' '.join(cmd)}")
    print(f"{'='*60}\n")
    result = subprocess.run(cmd, capture_output=False)
    if result.returncode != 0:
        print(f"\nERROR: {desc} failed (exit code {result.returncode})", file=sys.stderr)
        sys.exit(result.returncode)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(
        description="flOw recompilation pipeline"
    )
    parser.add_argument("--elf", default=DEFAULT_ELF,
                        help=f"Path to decrypted EBOOT.elf (default: {DEFAULT_ELF})")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help=f"Path to config.toml (default: {DEFAULT_CONFIG})")
    parser.add_argument("--output", "-o", default=DEFAULT_OUTPUT,
                        help=f"Output directory for recompiled C (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--ps3recomp-dir", default=None,
                        help="Path to ps3recomp root (default: ../ps3 or PS3RECOMP_DIR env)")
    parser.add_argument("--python", default=sys.executable,
                        help="Python interpreter to use")
    parser.add_argument("--skip-find", action="store_true",
                        help="Skip function finding (reuse existing functions.json)")
    parser.add_argument("--functions-file", default=None,
                        help="Use a pre-existing function list JSON instead of finding")
    args = parser.parse_args()

    # Resolve ps3recomp location
    ps3recomp_dir = (
        args.ps3recomp_dir
        or os.environ.get("PS3RECOMP_DIR")
        or DEFAULT_PS3RECOMP
    )
    ps3recomp_dir = os.path.abspath(ps3recomp_dir)

    tools_dir = find_ps3recomp_tools(ps3recomp_dir)

    # Validate input
    elf_path = os.path.abspath(args.elf)
    if not os.path.isfile(elf_path):
        print(f"ERROR: ELF not found: {elf_path}", file=sys.stderr)
        print(f"Place your decrypted EBOOT.elf in game/", file=sys.stderr)
        sys.exit(1)

    # Load config
    config = load_config(args.config)
    output_dir = os.path.abspath(
        config.get("output", {}).get("output_dir", args.output)
    )
    func_prefix = config.get("output", {}).get("func_prefix", "recomp_")

    print(f"flOw Recompilation Pipeline")
    print(f"  ELF:        {elf_path}")
    print(f"  Output:     {output_dir}")
    print(f"  ps3recomp:  {ps3recomp_dir}")
    print(f"  Config:     {args.config}")

    os.makedirs(output_dir, exist_ok=True)

    # Intermediate files
    functions_json = args.functions_file or os.path.join(output_dir, "functions.json")

    start_time = time.time()

    # -----------------------------------------------------------------------
    # Step 1: Find function boundaries
    # -----------------------------------------------------------------------
    if args.functions_file:
        print(f"\nUsing pre-existing function list: {functions_json}")
    elif args.skip_find and os.path.isfile(functions_json):
        print(f"\nSkipping function finding (reusing {functions_json})")
    else:
        find_functions = os.path.join(tools_dir, "find_functions.py")
        run_tool(args.python, find_functions, [
            elf_path,
            "--output", functions_json,
        ], "Step 1/3: Finding function boundaries")

    # Verify functions file exists
    if not os.path.isfile(functions_json):
        print(f"ERROR: Function list not found: {functions_json}", file=sys.stderr)
        sys.exit(1)

    with open(functions_json) as f:
        func_list = json.load(f)
    print(f"  Found {len(func_list)} functions")

    # -----------------------------------------------------------------------
    # Step 2: Lift PPU assembly to C
    # -----------------------------------------------------------------------
    ppu_lifter = os.path.join(tools_dir, "ppu_lifter.py")
    run_tool(args.python, ppu_lifter, [
        elf_path,
        "--output", output_dir,
        "--functions", functions_json,
        "--header-name", "ppu_recomp.h",
        "--source-name", "ppu_recomp.c",
    ], "Step 2/3: Lifting PPU instructions to C")

    # -----------------------------------------------------------------------
    # Step 3: Generate integration files
    # -----------------------------------------------------------------------
    print(f"\n{'='*60}")
    print(f"  Step 3/3: Generating integration files")
    print(f"{'='*60}\n")

    # Generate a wrapper header that bridges the lifter's func_ADDR naming
    # to the runtime's RecompiledFunc table expected by main.cpp / stubs.cpp.
    #
    # The lifter outputs:
    #   - ppu_recomp.h: ppu_context struct + function declarations
    #   - ppu_recomp.c: function bodies + function_table[]
    #
    # We generate:
    #   - func_table.cpp: RecompiledFunc g_recompiled_funcs[] + recomp_game_main
    #     that bridges to the lifter's function_table and entry point.

    func_table_path = os.path.join(output_dir, "func_table.cpp")
    # Entry point: OPD at 0x846AE0 resolves to real code at 0x10230
    # The OPD descriptor contains (func_addr=0x10230, toc=0x8969A8)
    entry_point = 0x10230  # flOw real entry (OPD 0x846AE0 -> 0x10230)

    with open(func_table_path, "w") as f:
        f.write("/* Auto-generated by recompile.py -- do not edit by hand. */\n")
        f.write('#include "ppu_recomp.h"\n')
        f.write("#include <cstdio>\n\n")

        # Bridge RecompiledFunc table
        f.write("struct RecompiledFunc {\n")
        f.write("    uint32_t    guest_addr;\n")
        f.write("    void      (*host_func)(void* ctx);\n")
        f.write("};\n\n")

        # Cast function_table entries to RecompiledFunc format
        f.write("/* Populated from lifter's function_table */\n")
        f.write('extern "C" const RecompiledFunc g_recompiled_funcs[] = {\n')
        for func in func_list:
            addr = int(str(func["start"]), 0)
            f.write(f"    {{ 0x{addr:08X}, (void(*)(void*))func_{addr:08X} }},\n")
        f.write("    { 0, nullptr }\n")
        f.write("};\n\n")

        f.write(f'extern "C" const size_t g_recompiled_func_count = {len(func_list)};\n\n')

        # Entry point bridge
        f.write(f"/* Entry point: func_{entry_point:08X} */\n")
        f.write(f'extern "C" void recomp_game_main(void* ctx)\n')
        f.write("{\n")
        f.write(f'    printf("[recomp] Entering flOw at 0x{entry_point:08X}...\\n");\n')
        f.write(f"    func_{entry_point:08X}((ppu_context*)ctx);\n")
        f.write("}\n")

    print(f"  Wrote {func_table_path}")
    print(f"    {len(func_list)} functions in table")
    print(f"    Entry point: func_{entry_point:08X}")

    # -----------------------------------------------------------------------
    # Summary
    # -----------------------------------------------------------------------
    elapsed = time.time() - start_time
    print(f"\n{'='*60}")
    print(f"  Recompilation complete ({elapsed:.1f}s)")
    print(f"{'='*60}")
    print(f"  Functions: {len(func_list)}")
    print(f"  Output:    {output_dir}/")
    print(f"    ppu_recomp.h   - Context struct + declarations")
    print(f"    ppu_recomp.c   - Lifted C functions")
    print(f"    func_table.cpp - Runtime integration")
    print(f"    functions.json - Function boundaries")
    print(f"\n  Next steps:")
    print(f"    cmake -B build -DCMAKE_BUILD_TYPE=Release")
    print(f"    cmake --build build")
    print(f"    ./build/flow")


if __name__ == "__main__":
    main()
