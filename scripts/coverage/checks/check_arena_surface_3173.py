#!/usr/bin/env python3
"""Issue #3173: sink fine-grained arena: compact/defrag knobs.

Public arena: add() surface is compact + compact-with-policy +
request-defrag + set-auto-compact-threshold (≤4). Algorithm variants
(live-compact / defrag-now / adaptive-compact / sticky-densify / …)
stay as sink_arena_prim bodies but are not registered.

  AC1 Public arena: add() names ≤ 4 and only the four KEEP names
  AC2 sink_arena_prim holds the 10 sunk names; no add("arena:defrag"
  AC3 compact-with-policy + set-auto-compact-threshold remain the policy switch
  AC4 No new public query key; SlimSurface shrinks
  AC5 Extend arena-batch + gc-compact + production-sweep suites
  AC6 This linter + build.py; no test_issue_3173.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]

ADD_RE = re.compile(r'add\(\s*"([^"]+)"')
SUNK = (
    "arena:adaptive-compact",
    "arena:auto-compact-threshold",
    "arena:compact-all",
    "arena:defrag",
    "arena:defrag-now",
    "arena:live-compact",
    "arena:recover-moving-sticky-densify",
    "arena:set-compact-threshold",
    "arena:should-auto-compact?",
    "arena:shrink-to-fit",
)
KEEP = (
    "arena:compact",
    "arena:compact-with-policy",
    "arena:request-defrag",
    "arena:set-auto-compact-threshold",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mem = _read("src/compiler/evaluator_primitives_memory.cpp")
    batch = _read("tests/core/test_arena_batch.cpp")
    gc = _read("tests/serve/test_gc_compact_batch.cpp")
    sweep = _read("tests/compiler/test_production_sweep.cpp")
    build = _read("build.py")
    q = read_query_prims()

    public = [n for n in ADD_RE.findall(mem) if n.startswith("arena:")]
    if len(public) > 4:
        fails.append(f"AC1: public arena: add() count {len(public)} > 4: {public}")
    for k in KEEP:
        if k not in public:
            fails.append(f"AC1: missing public {k}")
    extra = sorted(set(public) - set(KEEP))
    if extra:
        fails.append(f"AC1: unexpected public arena: {extra}")

    must("sink_arena_prim", "AC2 helper", mem)
    must("Issue #3173", "AC2 cite", mem)
    for name in SUNK:
        if not re.search(rf'sink_arena_prim\(\s*"{re.escape(name)}"', mem):
            fails.append(f"AC2: sunk body missing {name}")
        if re.search(rf'add\(\s*"{re.escape(name)}"', mem):
            fails.append(f"AC2: public add() still registers {name}")

    must("CompactPolicy", "AC3 policy enum", mem)
    must("set_compact_threshold", "AC3 threshold setter", mem)
    must("compact-with-policy", "AC3 policy prim", mem)

    if "query:arena-set-policy" in q or "query:arena-surface" in q:
        fails.append("AC4: new top-level query key (forbidden)")
    must("3173: compact public", "AC5 test", batch)
    must("test_arena().defrag()", "AC5 gc C++ defrag", gc)
    must("arena:compact", "AC5 sweep", sweep)
    must("check_arena_surface_3173", "AC6 build", build)
    must("cmd_arena_surface_3173", "AC6 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3173.cpp").is_file():
        fails.append("AC6: test_issue_3173.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3173.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3173.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3173-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #3173 arena: surface reduction — public={public} sunk={len(SUNK)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
