#!/usr/bin/env python3
"""Issue #3030: abort/restore clears TypeLinearCommitProof + linear_fast_path face.

Contract:
  AC1 abort_restore_dual_topology sites call clear_type_linear_commit_proof_on_abort
  AC2 linear_fast_path_ok false after clear until a fresh stamp
  AC3 production counter + Soft observe; quiet no-face is zero extra
  AC4 schema-3030 on type-linear health + escape-postmutate; no second proof model
  AC5 tests + linter + build.py; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    qh = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    qj = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    th = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3030", "AC1", aud)
    must("clear_type_linear_commit_proof_on_abort", "AC1", aud)
    must("clear_type_linear_commit_proof_on_abort", "AC1", mb)
    if mb.count("abort_restore_dual_topology(") > mb.count("clear_type_linear_commit_proof_on_abort"):
        fails.append("AC1: fewer clears than abort_restore_dual_topology call sites")
    must("ac3030_1_abort_clears_fast_path", "AC1", t)

    # AC2
    must("linear_fast_path_ok", "AC2", t)
    must("linear_ir_fastpath_try_skip", "AC2", t)
    must("ac3030_2_fresh_stamp_restores", "AC2", t)

    # AC3
    must("g_type_linear_proof_cleared_on_abort_total", "AC3", aud)
    must("g_type_linear_proof_cleared_on_abort_observe_total", "AC3", aud)
    must("ac3030_3_soft_observe", "AC3", t)
    must("ac3030_4_quiet_zero_cost", "AC3", t)

    # AC4
    must("schema-3030", "AC4 health", qh)
    must("type-linear-proof-cleared-on-abort-total", "AC4 health", qh)
    must("schema-3030", "AC4 escape-stats", qj)
    must("TypeLinearCommitProof", "AC4 reuse", aud)
    if "struct TypeLinearCommitProofAbort" in aud:
        fails.append("AC4: second proof model (forbidden)")
    must("ac3030_health_schema", "AC4 health test", th)

    # AC5
    must("check_type_linear_proof_clear_on_abort_3030", "AC5 build", build)
    must("cmd_type_linear_proof_clear_on_abort_3030", "AC5 cmd", build)
    must("ac3030_6_linter_no_design", "AC5", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3030.cpp").is_file():
        fails.append("AC5: test_issue_3030.cpp present (forbidden per #81967)")
    if _read("docs/design/3030-type-linear-proof-abort.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3030 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3030 abort clears TypeLinearCommitProof face — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
