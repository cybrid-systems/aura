#!/usr/bin/env python3
"""Issue #3737: compiler_metrics_fields.inc lists every CompilerMetrics scalar atomic.

  AC1  every std::atomic uint field on CompilerMetrics (not arrays) appears
       in compiler_metrics_fields.inc (name match)
  AC2  extra FIELD() names that are not struct members fail (except none)
  AC3  append-only — linter diffs; does not rewrite mid-struct order
  AC4  wired in build.py; no test_issue_3737.cpp; no docs/design/3737-*
  AC5  query_hash_overflow_total stays off the .inc (process atomic, #3020)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def compiler_metrics_scalar_atomics(header: str) -> list[str]:
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
    d = 0
    for line in body.splitlines():
        d += line.count("{") - line.count("}")
        if d != 0:
            continue
        if re.search(r"std::atomic<[^>]+>\s+\w+\s*\[", line):
            continue
        mm = re.search(r"std::atomic<\s*std::uint(?:64|32|16|8)_t\s*>\s+(\w+)", line)
        if mm:
            names.append(mm.group(1))
    return names


def inc_field_names(inc: str) -> list[str]:
    return re.findall(r"^AURA_COMPILER_METRICS_FIELD\((\w+)\)", inc, re.M)


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hdr = _read("src/compiler/observability_metrics.h")
    inc = _read("src/compiler/compiler_metrics_fields.inc")
    test = _read("tests/compiler/test_engine_metrics_facade.cpp")
    build = _read("build.py")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")

    atomics = compiler_metrics_scalar_atomics(hdr)
    fields = inc_field_names(inc)
    if not atomics:
        fails.append("AC1: failed to parse CompilerMetrics atomics")
    missing = [n for n in atomics if n not in set(fields)]
    extra = [n for n in fields if n not in set(atomics)]
    if missing:
        fails.append(f"AC1: {len(missing)} struct atomics missing from .inc (first={missing[0]})")
    if extra:
        fails.append(f"AC2: {len(extra)} .inc names not on CompilerMetrics (first={extra[0]})")
    must("stable_ref_handoff_reject_total", "AC1 mid-struct", inc)
    must("mutation_boundary_macro_hygiene_backstop_total", "AC1 struct-end", inc)

    must("Issue #3737", "AC3 cite", obs)
    must("stable_ref_", "AC3 mutate group", obs)

    must("check_compiler_metrics_fields_3737", "AC4 build", build)
    must("3737 AC", "AC4 test", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3737.cpp").is_file():
        fails.append("AC4: test_issue_3737.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3737-*")):
            fails.append(f"AC4: docs/design/{f.name} present")
    if "query_hash_overflow_total" in set(fields):
        fails.append("AC5: query_hash_overflow_total must stay off .inc (#3020)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #3737 compiler_metrics_fields.inc — {len(atomics)} scalar atomics match .inc")
    return 0


if __name__ == "__main__":
    sys.exit(main())
