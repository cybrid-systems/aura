#!/usr/bin/env python3
"""Issue #3737 / #5244: CompilerMetrics field list has ONE source.

Issue #5244 turned src/compiler/compiler_metrics_fields.inc into a GENERATED
artefact: struct CompilerMetrics in observability_metrics.h is the single
definition, and scripts/gen_compiler_metrics_fields.py expands it. This linter
is the gate half of the drift check.

  AC1  the .inc on disk equals what the generator derives from the struct —
       a field added/removed/renamed without regenerating fails here
  AC2  name parity both ways (belt for AC1: a hand-edited row is caught even
       if the generator would silently drop it)
  AC3  excluded names stay off the .inc: query_hash_overflow_total (#3020,
       a process atomic, not a CompilerMetrics field)
  AC4  gated by run_checks.py; no test_issue_3737.cpp; no docs/design/3737-*
  AC5  the .inc is generated (banner cites the generator) and the generated
       table has exactly one expansion site in src/
  AC6  the generator is not an orphan: this gated check is what runs it

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GENERATOR = ROOT / "scripts" / "gen_compiler_metrics_fields.py"

sys.path.insert(0, str(Path(__file__).resolve().parent))  # noqa: E402  (_wiring sibling)
from _wiring import WIRED_TEXT as wired_text  # noqa: E402  (sibling helper)

# Struct members that are deliberately NOT observables (#3020 process atomic).
# The generator excludes the same name.
EXCLUDED = frozenset({"query_hash_overflow_total"})


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
            continue
        # Issue #4717 (P4 step 3): aura::core::ShardedCounter is a scalar
        # atomic-equivalent counter (fetch_add / load / store, EXACT totals
        # by summing shards). A sharded field is still a CompilerMetrics
        # scalar observable, so it belongs in the .inc exactly like a
        # std::atomic<uint64_t> one.
        sm = re.search(r"aura::core::ShardedCounter\s+(\w+)", line)
        if sm:
            names.append(sm.group(1))
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
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")

    # AC1: the checked-in .inc is what the generator derives from the header.
    gen = subprocess.run([sys.executable, str(GENERATOR), "--check"], capture_output=True, text=True)
    if gen.returncode != 0:
        detail = (gen.stderr or gen.stdout).strip().replace("\n", "\n      ")
        fails.append(f"AC1: .inc is stale vs struct CompilerMetrics:\n      {detail}")

    atomics = [n for n in compiler_metrics_scalar_atomics(hdr) if n not in EXCLUDED]
    fields = inc_field_names(inc)
    if not atomics:
        fails.append("AC2: failed to parse CompilerMetrics atomics")
    missing = [n for n in atomics if n not in set(fields)]
    extra = [n for n in fields if n not in set(atomics)]
    if missing:
        fails.append(f"AC2: {len(missing)} struct atomics missing from .inc (first={missing[0]})")
    if extra:
        fails.append(f"AC2: {len(extra)} .inc names not on CompilerMetrics (first={extra[0]})")
    if len(fields) != len(set(fields)):
        fails.append("AC2: .inc lists a field more than once")
    must("stable_ref_handoff_reject_total", "AC2 mid-struct", inc)
    must("mutation_boundary_macro_hygiene_backstop_total", "AC2 struct-end", inc)

    # AC3: excluded names (not observables) stay off the generated list.
    if "query_hash_overflow_total" in set(fields):
        fails.append("AC3: query_hash_overflow_total must stay off .inc (#3020)")

    # AC4: gated by the runner + no plan docs (contract lives in the issue).
    must("Issue #3737", "AC4 cite", obs)
    must("check_compiler_metrics_fields_3737", "AC4 gated", wired_text)
    must("3737 AC", "AC4 test", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3737.cpp").is_file():
        fails.append("AC4: test_issue_3737.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3737-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    # AC5: generated banner + exactly one expansion site for the list.
    must("GENERATED FILE", "AC5 banner", inc)
    must("gen_compiler_metrics_fields.py", "AC5 banner cites generator", inc)
    sites = 0
    for p in (ROOT / "src").rglob("*"):
        if p.suffix not in {".cpp", ".h", ".hh", ".hpp", ".ixx", ".inc"} or not p.is_file():
            continue
        sites += _read(str(p.relative_to(ROOT))).count('#include "compiler_metrics_fields.inc"')
    if sites != 1:
        fails.append(f"AC5: expected exactly 1 include site for the generated list, found {sites}")

    # AC6: the generator cannot rot — the gated check above runs it.
    if not GENERATOR.is_file():
        fails.append("AC6: gen_compiler_metrics_fields.py missing")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        f"OK: Issue #5244 single-source CompilerMetrics fields — "
        f"{len(atomics)} scalar atomics, .inc is generated and current"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
