#!/usr/bin/env python3
"""Issue #3640: add_mutate isolation gate single spine (wrap != tenant).

Contract (one row per AC):
  AC1  the add_mutate gate parses packed StableNodeRefs through the same
       unpack_stable_ref_arg as resolve_mutate_node_arg (#3396 v2) and
       takes ref_tenant from the packed tenant slot; the old shallow
       wrap-as-tenant parse (inner.cdr.car) is gone
  AC2  resolve_mutate_node_arg keeps its unpack (#3415 AC7); the #3396
       v2 linter stays wired; the v2 spine walker is untouched
  AC3  ACs live in tests/compiler/test_require_effect_auto_isolation.cpp
       (3640 markers: deny / source-cite / allow / Soft v1)
  AC4  no tests/**/test_issue_3640.cpp; no docs/design/3640-*
  AC5  build.py wires check_unpack_spine_gate_3640 + root allowlist

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    hygiene = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: gate single spine ───────────────────────────────────────────
    must("unpack_stable_ref_arg](std::span<const EvalValue> a)", "AC1 gate captures unpack", mut)
    must("Issue #3640", "AC1 gate cites #3640", mut)
    gate_idx = mut.find("Issue #3640")
    if gate_idx < 0:
        fails.append("AC1: gate block missing")
    else:
        gate_window = mut[gate_idx : gate_idx + 1600]
        if "ref_tenant = packed->tenant_id;" not in gate_window:
            fails.append("AC1: ref_tenant not taken from packed tenant")
        if "target_node = static_cast<aura::ast::NodeId>(packed->id);" not in gate_window:
            fails.append("AC1: target_node not taken from packed id")
    bare = mut.count("auto c2 = ev.pairs_[inner].cdr;")
    if bare != 0:
        fails.append(f"AC1: shallow wrap-as-tenant parse still present (x{bare})")

    # ── AC2: resolve spine + #3396 lineage unchanged ─────────────────────
    resolve_idx = mut.find("auto resolve_mutate_node_arg")
    if resolve_idx < 0:
        fails.append("AC2: resolve_mutate_node_arg missing")
    elif "if (auto packed = unpack_stable_ref_arg(arg))" not in mut[resolve_idx:]:
        fails.append("AC2: resolve_mutate_node_arg unpack removed (#3415 regression)")
    must("auto walk_v2", "AC2 v2 spine walker present", mut)
    must("check_unpack_stable_ref_arg_v2_3396", "AC2 #3396 linter still wired", build)

    # ── AC3: test markers (source-cite in auto_isolation, runtime in
    # the hygiene closed-loop binary) ────────────────────────────────────
    for marker in ("3640 AC1", "3640 AC3", "3640 AC4"):
        must(marker, f"AC3 runtime marker {marker}", hygiene)
    must("3640 AC2", "AC3 source-cite marker 3640 AC2", test)

    # ── AC4: no invent ───────────────────────────────────────────────────
    if (ROOT / "tests" / "compiler" / "test_issue_3640.cpp").is_file():
        fails.append("AC4: test_issue_3640.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3640.cpp").is_file():
        fails.append("AC4: tests/core/test_issue_3640.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3640-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    # ── AC5: wiring ──────────────────────────────────────────────────────
    must("check_unpack_spine_gate_3640", "AC5 build.py wiring", build)
    must("check_unpack_spine_gate_3640.py", "AC5 root allowlist entry", allow)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3640 add_mutate gate single spine — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
