#!/usr/bin/env python3
"""Issue #3040: residual compile/verify/syntax NodeId entries gate first.

Contract (one row per AC):
  AC1  Production path: residual Agent-reachable compile:/verify:/syntax:
       NodeId writers call gate_compile_node_effect (for_node_id / on_ref)
       BEFORE Guard / topology write. No 2-arg require_effect_for_node_id
       with implicit ref_tenant=0.
  AC2  Stamped foreign tenant uses require_effect_on_ref (not re-stamp).
  AC3  Soft / Off: sandbox_mode==0 && effect_sandbox_mode==0 short-circuit
       (zero extra stores).
  AC4  nodeid_only_entry_prevented_total + schema-3040 posture keys.
  AC5  Extends test_tenant_isolation_enforcement.cpp; this linter.
  AC6  build.py wires this check; no test_issue_3040.cpp; no docs/design/.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Agent-reachable NodeId writers that must gate before mutate body.
RESIDUAL_SITES = (
    "compile:mark-dirty-upward-fast",
    "verify:assertion-failed",
    "verify:report-coverage",
    "compile:mark-narrowing-dirty!",
    "compile:per-defuse-index-add",
    "compile:subtree-bump",
    "syntax:set-marker",
    "syntax:propagate-marker",
    "syntax:set-provenance",
)

MUTATE_MARKERS = (
    "run_under_mutation_guard",
    "run_compile_dirty_under_guard",
    "apply_verify_dirty_bits",
    "begin_metadata_mutation",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _prim_body(text: str, name: str) -> str:
    # Issue #3172: fine-grained compile: dirty writers are sink_compile_prim
    # (not public add()). Prefer add() then the sunk registrar.
    needles = (f'add("{name}"', f'sink_compile_prim("{name}"')
    start = -1
    for needle in needles:
        start = text.find(needle)
        if start >= 0:
            break
    if start < 0:
        return ""
    # Body runs until the next add(" / sink / ObservabilityPrims / register_ end.
    rest = text[start:]
    nxt = len(rest)
    for pat in (
        '\n    add("',
        "\n    sink_compile_prim(",
        "\n    ObservabilityPrims::",
        "\n}\n",
    ):
        i = rest.find(pat, 8)
        if 0 <= i < nxt:
            nxt = i
    return rest[:nxt]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    iso = _read("src/core/workspace_isolation.hh")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    qws = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")
    lint2942 = _read("scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py")

    # ── AC1: residual sites gate before mutate body ──
    must("gate_compile_node_effect", "AC1 helper", compile_cpp)
    must("parse_compile_node_arg", "AC1 helper", compile_cpp)
    must("Issue #3040", "AC1 compile", compile_cpp)
    must("require_effect_for_node_id", "AC1 helper", compile_cpp)
    must("require_effect_on_ref", "AC1 helper", compile_cpp)
    # No 2-arg require_effect_for_node_id(id, …) overload.
    if re.search(
        r"require_effect_for_node_id\s*\([^;]{0,80}?ref_tenant\s*=\s*0",
        compile_cpp + sec + ixx,
    ):
        fails.append("AC1: 2-arg require_effect_for_node_id default ref_tenant=0 present")
    must("no 2-arg default ref_tenant=0", "AC1 comment", sec)
    must("Issue #3040", "AC1 security", sec)

    for name in RESIDUAL_SITES:
        body = _prim_body(compile_cpp, name)
        if not body:
            fails.append(f"AC1: residual site {name!r} not found")
            continue
        gate_p = body.find("gate_compile_node_effect")
        if gate_p < 0:
            fails.append(f"AC1: {name}: missing gate_compile_node_effect")
            continue
        mutate_positions = [body.find(m) for m in MUTATE_MARKERS if body.find(m) >= 0]
        if mutate_positions and gate_p > min(mutate_positions):
            fails.append(f"AC1: {name}: gate_compile_node_effect must precede mutate body")

    # Canonical #2839 site still gated (lineage).
    fb = _prim_body(compile_cpp, "mutate:from-verification-feedback")
    if "require_effect_for_node_id" not in fb:
        fails.append("AC1: mutate:from-verification-feedback lost for_node_id")

    # ── AC2: stamped path uses on_ref ──
    must("arg.tenant != 0", "AC2", compile_cpp)
    must("require_effect_on_ref", "AC2 helper", compile_cpp)
    must("require_effect_on_ref", "AC2 test", test_iso)

    # ── AC3: Soft/Off zero-cost ──
    must("sandbox_mode() == 0 && ev.effect_sandbox_mode() == 0", "AC3", compile_cpp)
    must("zero extra stores", "AC3", compile_cpp)
    must("ac3040_3_soft_off", "AC3 test", test_iso)

    # ── AC4: counter + schema ──
    must("nodeid_only_entry_prevented_total", "AC4 metrics", iso)
    must("nodeid_only_entry_prevented", "AC4 snapshot", iso)
    must("schema-3040", "AC4 posture", posture)
    must("issue-3040", "AC4 posture", posture)
    must("nodeid-only-entry-prevented-wired", "AC4 posture", posture)
    must("nodeid-only-entry-prevented-total", "AC4 posture", posture)
    must("kNodeIdOnlyEntryPreventedWired", "AC4 ixx", ixx)
    must("kNodeIdOnlyEntryIssue = 3040", "AC4 ixx", ixx)
    must("schema-3040", "AC4 isolation-stats", qws)
    must("nodeid_only_entry_prevented_total.fetch_add", "AC4 bump", sec)

    # ── AC5: existing isolation suite ──
    must("#3040", "AC5 test", test_iso)
    must("ac3040_1_", "AC5 test AC1", test_iso)
    must("ac3040_2_", "AC5 test AC2", test_iso)
    must("ac3040_5_", "AC5 test AC5", test_iso)
    must("3040", "AC5 2942 lineage", lint2942)

    # ── AC6: linter wired + no invent / no design ──
    must("check_compile_node_id_entry_3040", "AC6", build)
    if (ROOT / "tests" / "core" / "test_issue_3040.cpp").is_file():
        fails.append("AC6: test_issue_3040.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3040.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3040.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3040-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3040 compile NodeId-only entry gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
