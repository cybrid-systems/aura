#!/usr/bin/env python3
"""Issue #5244: single-source the CompilerMetrics field list.

src/compiler/observability_metrics.h (struct CompilerMetrics) is the ONE place
a scalar metric field is declared. src/compiler/compiler_metrics_fields.inc is
a GENERATED expansion of that struct: the engine:metrics grouped dump, the
table-driven emitters and the gate all consume the .inc, never a hand copy.

Usage
  python3 scripts/gen_compiler_metrics_fields.py           # rewrite the .inc
  python3 scripts/gen_compiler_metrics_fields.py --check    # exit 1 if stale

Regenerate after adding/removing a scalar atomic on CompilerMetrics. The
generator is order-preserving (append-only history like the pre-#5244 file):
names keep their .inc position; new struct fields are appended in struct
order; names whose atomic disappeared from the struct are dropped. Comment
lines in the .inc are preserved as-is (they are not field state).

Gate: scripts/coverage/checks/check_compiler_metrics_fields_3737.py runs this
module in --check mode, so a struct field with no .inc row (or a hand-edited
.inc row) fails "python3 build.py gate".
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "compiler" / "observability_metrics.h"
INC = ROOT / "src" / "compiler" / "compiler_metrics_fields.inc"

FIELD_RE = re.compile(r"^AURA_COMPILER_METRICS_FIELD\((\w+)\)\s*(?://.*)?$")

# Issue #3020: process-global atomic, not a CompilerMetrics field — the .inc
# must never carry it (the gate keeps this honest).
EXCLUDED = frozenset({"query_hash_overflow_total"})

BANNER = """\
// CompilerMetrics scalar atomics for engine:metrics grouped dump.
// GENERATED FILE (Issue #5244) — do not hand-edit a FIELD row.
// Source of truth: struct CompilerMetrics in observability_metrics.h.
// Regenerate after adding/removing a scalar atomic:
//   python3 scripts/gen_compiler_metrics_fields.py
// {count} scalar fields. Generator + staleness gate:
// scripts/gen_compiler_metrics_fields.py --check (wired into build.py gate).
#ifndef AURA_COMPILER_METRICS_FIELD
#error "define AURA_COMPILER_METRICS_FIELD(name) before including"
#endif
"""


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def struct_scalar_fields(header: str) -> list[str]:
    """Scalar atomic-equivalents declared directly in struct CompilerMetrics."""
    m = re.search(r"struct CompilerMetrics \{", header)
    if not m:
        return []
    start = m.end()
    i, depth = start, 1
    while i < len(header) and depth:
        if header[i] == "{":
            depth += 1
        elif header[i] == "}":
            depth -= 1
        i += 1
    body = header[start : i - 1]
    names: list[str] = []
    depth = 0
    for line in body.splitlines():
        depth += line.count("{") - line.count("}")
        if depth != 0:
            continue
        if re.search(r"std::atomic<[^>]+>\s+\w+\s*\[", line):
            continue
        mm = re.search(r"std::atomic<\s*std::uint(?:64|32|16|8)_t\s*>\s+(\w+)", line)
        if mm:
            names.append(mm.group(1))
            continue
        # Issue #4717: ShardedCounter is a scalar atomic-equivalent counter.
        sm = re.search(r"aura::core::ShardedCounter\s+(\w+)", line)
        if sm:
            names.append(sm.group(1))
    return [n for n in names if n not in EXCLUDED]


def generate(header: str, inc: str) -> str:
    declared = struct_scalar_fields(header)
    declared_set = set(declared)
    current = [m.group(1) for m in map(FIELD_RE.match, inc.splitlines()) if m]
    assert current or not inc

    kept: list[str] = []
    out: list[str] = []
    started = False
    for line in inc.splitlines():
        m = FIELD_RE.match(line)
        if m:
            started = True
            name = m.group(1)
            if name in declared_set and name not in kept:
                kept.append(name)
                out.append(line)
            continue
        if not started:
            continue  # old banner / include guard, replaced below
        if line.strip() == "#undef AURA_COMPILER_METRICS_FIELD":
            continue  # re-appended at the end
        if line.startswith("#ifndef") or line.startswith("#error") or line.startswith("#endif"):
            continue
        out.append(line)

    missing = [n for n in declared if n not in set(kept)]
    out.extend("AURA_COMPILER_METRICS_FIELD(" + n + ")" for n in missing)

    while out and not out[-1].strip():
        out.pop()
    out.append("")
    out.append("#undef AURA_COMPILER_METRICS_FIELD")
    return BANNER.format(count=len(kept) + len(missing)) + "\n" + "\n".join(out) + "\n"


def first_difference(a: str, b: str) -> str:
    la, lb = a.splitlines(), b.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else "<eof>"
        y = lb[i] if i < len(lb) else "<eof>"
        if x != y:
            return f"first difference at line {i + 1}:\n  on disk: {x!r}\n  expected: {y!r}"
    return ""


def main(argv: list[str]) -> int:
    check = "--check" in argv[1:]
    header = read(HEADER)
    if not header:
        print(f"missing {HEADER}", file=sys.stderr)
        return 1
    if not struct_scalar_fields(header):
        print("failed to parse struct CompilerMetrics", file=sys.stderr)
        return 1
    want = generate(header, read(INC))
    have = read(INC)
    rows = len([m for m in map(FIELD_RE.match, want.splitlines()) if m])
    if have == want:
        if check:
            print(f"OK: compiler_metrics_fields.inc is current ({rows} scalar fields)")
        return 0
    if check:
        print(
            "FAIL: compiler_metrics_fields.inc is stale — struct CompilerMetrics "
            "changed without regenerating (Issue #5244).\n"
            "      run: python3 scripts/gen_compiler_metrics_fields.py\n" + first_difference(have, want),
            file=sys.stderr,
        )
        return 1
    INC.write_text(want, encoding="utf-8")
    print(f"wrote {INC.relative_to(ROOT)} ({rows} scalar fields)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
