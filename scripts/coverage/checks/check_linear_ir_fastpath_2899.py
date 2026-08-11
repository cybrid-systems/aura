#!/usr/bin/env python3
"""Issue #2899: proven Move/Drop IR fast-path after TypeLinearCommitProof.

Contract:
  AC1 proof linear_ok + no escape → skip_total
  AC2 escape active / Reject → skip-blocked
  AC3 no proof / mid-boundary → full check (quiet or blocked)
  AC4 additive query keys schema-2899; preserve #2263/#2854
  AC5 source-cite + extend test_escape_move_elision_gate; no docs/design/
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
    ir = _read("src/compiler/ir_executor_impl.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    build = _read("build.py")

    must("2899", "AC1", aud)
    must("linear_ir_fastpath_try_skip", "AC1", aud)
    must("g_linear_ir_fastpath_skip_total", "AC1", aud)
    must("publish_last_proof_face", "AC1", aud)
    must("linear_ir_fastpath_try_skip", "AC1", ir)
    must("2899", "AC1", ir)

    must("g_linear_ir_fastpath_skip_blocked_total", "AC2", aud)
    must("aura_escape_move_gate_active", "AC2", aud)
    must("Reject", "AC2", aud)

    must("boundary_depth", "AC3", aud)
    must("g_last_type_linear_commit_proof_stamp", "AC3", aud)

    must("schema-2899", "AC4", q)
    must("linear-ir-fastpath-skip-total", "AC4", q)
    must("linear-ir-fastpath-wired", "AC4", q)
    must("schema-2263", "AC4", q)

    must("ac2899_1_proof_fresh_skips", "AC5", t)
    must("ac2899_2_escape_or_reject_blocks", "AC5", t)
    must("ac2899_3_no_proof_or_mid_boundary", "AC5", t)
    must("ac2899_4_additive_query", "AC5", t)
    must("ac2899_5_source_cite", "AC5", t)
    must("check_linear_ir_fastpath_2899", "AC5", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2899-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2899.cpp").is_file():
        fails.append("tests/compiler/test_issue_2899.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2899 linear IR fast-path — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
