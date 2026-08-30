#!/usr/bin/env python3
"""Issue #3400: mutate:check-stable-ref probes the node_gen_ domain.

Contract (one row per AC):
  AC1 source: check-stable-ref body cites #3400, unpacks via
     unpack_stable_ref_arg, and validity = node_gen_for + is_live_node
     (not is_valid_in/get_safe — those still require ref.gen ==
     generation_). The old `flat.generation() == captured_gen`
     compare is gone.
  AC2 source: sibling-mutate-safe probe is node-gen domain (no
     workspace-gen compare anywhere in the prim body).
  AC3 fixture: restamped target old ref → #f row exists.
  AC4 source: production provenance-less pack → mev("stale-ref",
     "packed ref missing provenance") (same face as #3396 apply).
  AC5 fixture extended in tests/core/test_stale_ref_string_heap.cpp;
     no tests/issues/test_issue_3400.cpp; no docs/design/3400-*;
     linter wired into build.py.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    fixture = _read("tests/core/test_stale_ref_string_heap.cpp")
    build = _read("build.py")

    pos = mut.find('add_mutate(\n        "mutate:check-stable-ref"')
    if pos < 0:
        pos = mut.find('"mutate:check-stable-ref"')
    win = mut[pos : pos + 6000] if pos >= 0 else ""

    # AC1: node-gen domain probe
    must("Issue #3400", "AC1 cite", win)
    must("unpack_stable_ref_arg(a[0])", "AC1 SSOT unpack", win)
    must("node_gen_for(ref->id)", "AC1 node-gen validity", win)
    must("is_live_node(ref->id)", "AC1 occupancy check", win)
    if "flat.generation() == captured_gen" in win:
        fails.append("AC1: workspace-gen compare still present in check-stable-ref body")

    # AC2: no workspace-generation() compare feeding validity at all
    if "flat.generation()" in win:
        fails.append("AC2: flat.generation() still referenced by check-stable-ref body")

    # AC3: fixture restamp row
    must("3400 AC3", "AC3 fixture cite", fixture)
    must("restamped target old ref", "AC3 fixture row", fixture)
    must("3400 AC2", "AC2 fixture cite", fixture)
    must("live node after sibling mutate", "AC2 fixture row", fixture)

    # AC4: production provenance-less pack → stale-ref
    must('mev("stale-ref", "packed ref missing provenance")', "AC4 production face", win)
    must("production_defaults_active()", "AC4 production gate", win)

    # AC5: fixture placement + wiring
    must("check_stable_ref_probe_3400", "AC5 build wiring", build)
    must("cmd_stable_ref_probe_3400_coverage", "AC5 build cmd", build)
    if (ROOT / "tests" / "issues" / "test_issue_3400.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3400.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3400-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3400 check-stable-ref node-gen domain probe — all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
