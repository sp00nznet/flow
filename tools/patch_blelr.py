"""
Patch auto-generated ppu_recomp.cpp to replace commented-out conditional
return placeholders (/* blelr cr7 */; etc.) with real `if (cond) return;`
statements, matching what the fixed lifter now emits.

Usage: python patch_blelr.py <path-to-ppu_recomp.cpp>
"""
import re
import sys

COND_MAP = {
    "eq": "((ctx->cr >> {s}) & 2)",
    "ne": "(!((ctx->cr >> {s}) & 2))",
    "lt": "((ctx->cr >> {s}) & 8)",
    "ge": "(!((ctx->cr >> {s}) & 8))",
    "gt": "((ctx->cr >> {s}) & 4)",
    "le": "(!((ctx->cr >> {s}) & 4))",
    "so": "((ctx->cr >> {s}) & 1)",
    "ns": "(!((ctx->cr >> {s}) & 1))",
    "dnz": "((ctx->ctr = (uint32_t)(ctx->ctr - 1)) != 0)",
    "dz":  "((ctx->ctr = (uint32_t)(ctx->ctr - 1)) == 0)",
}

# /* <mn> [operands] */;    where mn ends with "lr" or "ctr"
LINE_RE = re.compile(
    r"(?P<prefix>\s*)/\*\s*"
    r"(?P<mn>b(?:eq|ne|lt|le|gt|ge|so|ns|dnz|dz)(?:lr|ctr))"
    r"(?:\s+(?P<rest>[^*]*?))?"
    r"\s*\*/;"
)


def build_if(mn: str, rest: str) -> str | None:
    """Return a C statement replacing the placeholder, or None to leave it alone."""
    suffix = "lr" if mn.endswith("lr") else "ctr"
    cond_name = mn[1:-len(suffix)]  # strip leading "b" and trailing "lr"/"ctr"

    # Parse CR field from rest (e.g. "cr7" -> 7). Default: cr0.
    cr_field = 0
    if rest:
        m = re.match(r"cr(\d)", rest.strip())
        if m:
            cr_field = int(m.group(1))

    shift = (7 - cr_field) * 4
    tmpl = COND_MAP.get(cond_name)
    if tmpl is None:
        return None
    cond = tmpl.format(s=shift)

    if suffix == "lr":
        return f"if ({cond}) return;"
    else:  # ctr
        return f"if ({cond}) {{ ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx); return; }}"


FUNC_RE = re.compile(r"^void (func_[0-9A-Fa-f]+)\(")


def patch_file(path: str, only: set[str] | None = None) -> None:
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("latin-1")

    replaced = 0
    skipped = 0

    # Process line by line so we can track which function we're in.
    out_lines: list[str] = []
    cur_func: str | None = None
    for line in text.splitlines(keepends=True):
        m = FUNC_RE.match(line)
        if m:
            cur_func = m.group(1)
        if cur_func == "" or (line.startswith("}") and cur_func is not None):
            # End of function: update cur_func at next function entry.
            pass

        def replace(match: re.Match) -> str:
            nonlocal replaced, skipped
            if only is not None and cur_func not in only:
                return match.group(0)
            mn = match.group("mn")
            rest = match.group("rest") or ""
            repl = build_if(mn, rest)
            if repl is None:
                skipped += 1
                return match.group(0)
            replaced += 1
            return f"{match.group('prefix')}{repl}"

        out_lines.append(LINE_RE.sub(replace, line))

    with open(path, "wb") as f:
        f.write("".join(out_lines).encode("latin-1"))

    print(f"Replaced {replaced} conditional returns, skipped {skipped}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: patch_blelr.py <ppu_recomp.cpp> [--only func1,func2,...]",
              file=sys.stderr)
        sys.exit(2)
    path = sys.argv[1]
    only: set[str] | None = None
    for arg in sys.argv[2:]:
        if arg.startswith("--only="):
            only = set(arg[len("--only="):].split(","))
    patch_file(path, only)
