#!/usr/bin/env python3
"""Issue #3175: prune diagnostic / low-frequency query: primitives.

Public query: add() stays under 30. Hygiene/pin-count/skeleton/templates/
occurrence-stale/schema-of-marker/primitives-meta/build-index/result-fresh
stay as sink_query_prim bodies (C++ + existing engine:metrics) but are
not registered. Agents use calls/defines/dirty/provenance/by-marker.

  AC1 Public query: add() count < 30; core keep list present
  AC2 sink_query_prim holds the 15 sunk names; no add("query:hygiene-…
  AC3 Pin/hygiene counters remain on engine:metrics (register_stats_impl)
  AC4 No new public query key; SlimSurface shrinks
  AC5 Extend query-namespace-audit + pattern-hygiene suites
  AC6 This linter + build.py; no test_issue_3175.cpp; no docs/design/

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
    "query:hygiene-diagnostic",
    "query:hygiene-skip-count",
    "query:safe-span-pin-count",
    "query:generate-primitive-skeleton",
    "query:macro-introduced",
    "query:mark-occurrence-stale",
    "query:occurrence-stale?",
    "query:schema-of-marker",
    "query:templates",
    "query:ffi-pin-count",
    "query:primitives-meta",
    "query:build-index",
    "query:macro-provenance-chain",
    "query:result-fresh?",
    "query:result-matches",
    "query:primitives-by-category",
    "query:schema-of-primitive",
    "query:sv-interface",
    "query:sv-property",
)
KEEP = (
    "query:calls",
    "query:defines",
    "query:by-marker",
    "query:filter",
    "query:where",
    "query:pattern",
    "query:root",
    "query:dirty-nodes",
    "query:dirty-subtree",
    "query:dirty-impact",
    "query:mutation-impact",
    "query:mutations-since",
    "query:provenance-of",
    "query:stable-ref",
    "query:as-stable-ref",
    "query:ref-valid?",
    "query:node-type",
)
SCAN_GLOBS = (
    "src/compiler/evaluator_primitives*.cpp",
    "src/compiler/ffi_primitives_impl.cpp",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _scan_sources() -> str:
    chunks: list[str] = []
    for glob in SCAN_GLOBS:
        for p in sorted(ROOT.glob(glob)):
            chunks.append(p.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(chunks)


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    src = _scan_sources()
    obs_mid = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    audit = _read("tests/compiler/test_query_namespace_audit.cpp")
    hyg = _read("tests/compiler/test_query_pattern_default_hygiene.cpp")
    build = _read("build.py")
    q = read_query_prims()

    public = [n for n in ADD_RE.findall(src) if n.startswith("query:")]
    if len(public) >= 30:
        fails.append(f"AC1: public query: add() count {len(public)} >= 30: {public}")
    for k in KEEP:
        if k not in public:
            fails.append(f"AC1: missing public {k}")
    extra_sunk = sorted(set(public) & set(SUNK))
    if extra_sunk:
        fails.append(f"AC1: sunk names still public add(): {extra_sunk}")

    must("sink_query_prim", "AC2 helper", src)
    must("Issue #3175", "AC2 cite", src)
    for name in SUNK:
        if not re.search(rf'sink_query_prim\(\s*"{re.escape(name)}"', src):
            fails.append(f"AC2: sunk body missing {name}")
        if re.search(rf'add\(\s*"{re.escape(name)}"', src):
            fails.append(f"AC2: public add() still registers {name}")

    must('register_stats_impl(\n        "query:hygiene-skip-count"', "AC3 skip metrics", obs_mid)
    must('register_stats_impl(\n        "query:safe-span-pin-count"', "AC3 pin metrics", obs_mid)

    if "query:query-diagnostic" in q or "query:query-surface" in q:
        fails.append("AC4: new top-level query key (forbidden)")
    must("3175: query:templates sunk", "AC5 audit", audit)
    must('(query:by-marker \\"MacroIntroduced\\")', "AC5 hygiene", hyg)
    must("check_query_surface_3175", "AC6 build", build)
    must("cmd_query_surface_3175", "AC6 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3175.cpp").is_file():
        fails.append("AC6: test_issue_3175.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3175-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #3175 query: surface reduction — public={len(public)} sunk={len(SUNK)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
