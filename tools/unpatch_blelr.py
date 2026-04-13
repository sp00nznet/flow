"""
Revert the blelr patch in-place — turn the real `if (...) return;`
statements back into `/* blelr cr7 */;` comments so the existing
ppu_recomp.cpp builds equivalent to its pre-patch state.

The proper fix lives in the ps3recomp lifter (tools/ppu_lifter.py),
which now emits these as real statements. A re-lift of flOw will pick
that up. Reverting in the cpp lets us keep using the old placeholder
path that the game was taking before the fix exposed deeper engine
blockers.

Usage: python unpatch_blelr.py <ppu_recomp.cpp>
"""
import re, sys

# if (<cond>) return;  with comment captured after
LINE_RE = re.compile(
    r"(\s*)if \(([^)]*(?:\([^)]*\))?[^)]*)\) return;"
)


def main():
    if len(sys.argv) != 2:
        print("Usage: unpatch_blelr.py <ppu_recomp.cpp>", file=sys.stderr)
        sys.exit(2)
    path = sys.argv[1]
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("latin-1")

    # Replace `if ((...)) return;` with `/* blelr cr7 */;` ONLY when the
    # condition matches `(ctx->cr >> N) & M` forms we generated. Anything
    # more complex (game-level early return checks) is left alone.
    pattern = re.compile(
        r"(?P<pre>\s*)if \((?:\(\(ctx->cr >> \d+\) & \d+\)|"
        r"\(!\(\(ctx->cr >> \d+\) & \d+\)\))\) return;"
    )
    replaced = 0

    def replace(match):
        nonlocal replaced
        replaced += 1
        return f"{match.group('pre')}/* blelr cr7 */;"

    new_text = pattern.sub(replace, text)
    with open(path, "wb") as f:
        f.write(new_text.encode("latin-1"))
    print(f"Reverted {replaced} blelr statements")


if __name__ == "__main__":
    main()
