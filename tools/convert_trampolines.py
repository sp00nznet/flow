#!/usr/bin/env python3
"""
Convert fallthrough calls in ppu_recomp.cpp to trampoline pattern.

Finds func_XXXXXXXX(ctx); calls that are the last statement before }
(real fallthroughs, not mid-function bl calls) and converts them to:
    { extern uint32_t g_trampoline_next; g_trampoline_next = 0xXXXXXXXX; return; }
"""

import re
import sys

def convert(path):
    with open(path, 'r', errors='replace') as f:
        lines = f.readlines()

    call_re = re.compile(r'^(\s+)(func_([0-9A-Fa-f]{8}))\(ctx\);(\s*)$')
    converted = 0
    i = 0
    while i < len(lines):
        m = call_re.match(lines[i])
        if m:
            indent = m.group(1)
            func_name = m.group(2)
            addr = m.group(3)

            # Check if this is a fallthrough: is } the next non-empty/comment line?
            is_fallthrough = False
            j = i + 1
            while j < len(lines):
                stripped = lines[j].strip()
                if stripped == '' or stripped.startswith('/*') or stripped.startswith('//') or stripped == ';':
                    j += 1
                    continue
                if stripped == '}':
                    is_fallthrough = True
                break

            # Also check it's NOT after a return (dead code)
            if is_fallthrough:
                k = i - 1
                while k >= 0:
                    stripped = lines[k].strip()
                    if stripped == '' or stripped.startswith('/*') or stripped.startswith('//') or stripped == ';':
                        k -= 1
                        continue
                    if stripped == 'return;':
                        is_fallthrough = False  # dead code, skip
                    break

            if is_fallthrough:
                lines[i] = (f'{indent}{{ extern uint32_t g_trampoline_next; '
                           f'g_trampoline_next = 0x{addr}; return; }}\n')
                converted += 1
        i += 1

    with open(path, 'w', errors='replace') as f:
        f.writelines(lines)

    print(f'Converted {converted} fallthrough calls to trampoline pattern')

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'src/recomp/ppu_recomp.cpp'
    convert(path)
