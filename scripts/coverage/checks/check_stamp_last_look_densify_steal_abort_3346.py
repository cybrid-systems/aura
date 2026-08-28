#!/usr/bin/env python3
"""Issue #3346: last-look fingerprint / goals / linear_root before stamp.

Concurrent densify × steal × mid-abort can tear TypeLinearCommitProof
gauges vs live Occurrence + linear roots. Production success stamps
(outermost + densify Phase-5 + steal) re-read live fingerprint +
live_goal_count + linear_root_count immediately before the atomic
last_proof stores and reject (clear persist + invalidate_gen + no
green) on mismatch. densify/steal refuse if mid_abort_authority is
outstanding. last_proof_* publish with release; IR consult acquires.
Soft/Off: last-look early-returns. Reuses mismatch + invalidate_gen;
no new query keys.

Contract:
  AC1 last-look in both stamp builders; mismatch reject
  AC2 densify Phase-5 + steal refuse outstanding (or last-look)
  AC3 publish_last_proof_face release; linear_fast_path_ok acquire
  AC4 Soft early-return; after #3225; no invent / docs/design /
      g_3346_* / schema-3346

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    stubs = _read("src/compiler/test_concurrent_stubs.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("kStampLastLookIssue = 3346", "AC1 stamp", tma)
    must("stamp_last_look_live_matches", "AC1 helper", tma)
    must("reject_stamp_last_look_mismatch", "AC1 reject", tma)
    must("stamp_last_look_rejected", "AC1 outermost flag", tma)
    must("aura_stamp_last_look_cs_matches", "AC1 CS ABI", tma)
    must("aura_stamp_last_look_cs_matches", "AC1 light-link last-look stub", stubs)
    must("aura_clear_occurrence_persist_snapshot_tc", "AC1 light-link persist stub", stubs)
    must("note_stamp_last_look_tc", "AC1 TLS", tma)
    must("Issue #3346 AC1/AC2: last-look", "AC1 builder cite", tma)
    if tma.count("stamp_last_look_live_matches(") < 3:
        fails.append("AC1: expected last-look in both stamp builders + helper def")

    must("densify_abort_outstanding_3346", "AC2 densify", mb)
    must("mid_abort_authority_outstanding", "AC2 densify consult", mb)
    must("steal_abort_outstanding_3346", "AC2 steal", steal)
    must("note_stamp_last_look_tc", "AC2 steal TLS", steal)
    must("note_stamp_last_look_tc(tc_handle)", "AC2 densify notes TLS", mb)
    must("note_stamp_last_look_tc(tc)", "AC1 outermost notes TLS", mb)
    must("stamp_last_look_rejected()", "AC1 outermost skip-grant", mb)

    must(
        "g_last_proof_would_allow_commit.store(would_allow ? 1 : 0, std::memory_order_release)",
        "AC3 publish release",
        tma,
    )
    must(
        "g_last_proof_would_allow_commit.load(std::memory_order_acquire)",
        "AC3 IR acquire",
        tma,
    )
    must("g_rehydrate_miss_green_bind_gen.load(std::memory_order_acquire)", "AC3 bind acquire", tma)

    must("if (!stamp_last_look_hard())", "AC4 Soft skip", tma)
    must("ac3346_1_last_look_fingerprint_mismatch_rejects", "AC4 test AC1", t)
    must("ac3346_2_outstanding_authority_refuses_stamp", "AC4 test AC2", t)
    must("ac3346_3_stamp_matches_live_under_acquire", "AC4 test AC3", t)
    must("ac3346_4_soft_zero_extra_and_linter", "AC4 test AC4", t)
    must("check_stamp_last_look_densify_steal_abort_3346", "AC4 build.py", build)
    pos3225 = build.find("check_occurrence_persist_seq_3225")
    pos3346 = build.find("check_stamp_last_look_densify_steal_abort_3346")
    if pos3225 < 0 or pos3346 < 0 or pos3346 < pos3225:
        fails.append("AC4: linter must be wired after #3225")

    if "g_3346_" in tma or "g_3346_" in mb or "g_3346_" in steal:
        fails.append("AC4: new g_3346_* counter")
    if "schema-3346" in tma:
        fails.append("AC4: new schema-3346 query key")
    if (ROOT / "tests" / "issues" / "test_issue_3346.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3346.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3346.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3346.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3346-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3346 stamp_last_look_densify_steal_abort:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3346 stamp_last_look_densify_steal_abort: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
