#!/usr/bin/env python3
"""Issue #4161 source-cite gate: P1 StealInvariant RejectHard bit coverage
not asserted on every hard soak sample.

Residual: evaluate_residual_hard_and_bits maps residual arms to
StealInvariant bits and stores g_steal_safety_last_reject_invariant_bits
(#2929), and #3001 states the soak must fail-closed if residual counters
grow without matching RejectHard bits — but the hard soak residual check
only asserted delta == 0 (#2755/#2902/#3073). Counter growth without bit
attribution was reported as a number, never proven against the bits:
Agents cannot distinguish which invariant failed and sticky may latch
without a machine-checkable arm proof — weakens I3 deploy confidence
(observe gap, not Soft-as-vuln).

Fix contract (existing mechanisms only — no new counters):
  - tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp: inside the soak
    residual gate (residual_zero_gate — the chaos_soak_hard_gate
    residual check), new #4161 rows assert counter→bits attribution on
    every gated sample: rearm_race growth requires bits != 0 (no
    dedicated bit — the RejectHard store publishes the underlying
    residual arm's bits); envframe growth requires
    mask(StealInvariant::EnvFrameOk); lifetime_proof growth requires
    mask(StealInvariant::LifetimeProofOk). Reads
    steal_safety_last_reject_invariant_bits_v_read only.
  - tests/serve/test_steal_safety_production_residual_zero.cpp: AC19
    source-cites the soak rows + pins the #3001 contract comments.
  - src/serve/steal_safety.{h,cpp}: unchanged SSOT (both RejectHard
    stores + #3001 comments + accessor + test reset) — no second model.

ACs:
  AC1  Soak harness attribution rows: the #4161 block lives inside the
       residual_zero_gate check (bits read after the gate row), cites
       Issue #4161, asserts the three attribution CHECKs (rearm bits!=0,
       envframe mask, lifetime_proof mask) via the existing StealInvariant
       table; #2755/#2902/#3073 delta==0 rows intact (no weakening).
  AC2  Residual-zero face extends with AC19 (source-cite rows + negative
       rows); the batch summary line cites #4161.
  AC3  steal_safety SSOT intact: #3001 contract comments (header +
       steal_safety.cpp), both g_steal_safety_last_reject_invariant_bits
       store sites, the _v_read accessor, the test-reset store(0), the
       StealInvariant enum (Count = 7) and steal_invariant_mask helper.
  AC4  Wiring + no invent: build.py runs this linter, the allowlist row
       is present, no tests/**/test_issue_4161.cpp (per #81934), no
       docs/design/4161-* (per #1655), no g_4161_* counters, no
       schema-4161 query key on the touched surfaces.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
--self-test runs the rows against the real tree (must be clean) AND
against a broken synthetic (must be detected) to prove the checker
fails when the contract is violated.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHAOS = ROOT / "tests" / "serve" / "test_chaos_mutate_steal_gc_mailbox.cpp"
RESIDUAL = ROOT / "tests" / "serve" / "test_steal_safety_production_residual_zero.cpp"
SS_H = ROOT / "src" / "serve" / "steal_safety.h"
SS_CPP = ROOT / "src" / "serve" / "steal_safety.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def run_rows(texts: dict) -> list[str]:
    failures: list[str] = []

    def must(cond: bool, row: str) -> None:
        if not cond:
            failures.append(row)

    chaos = texts["chaos"]
    residual = texts["residual"]
    ss_h = texts["ss_h"]
    ss_cpp = texts["ss_cpp"]
    build = texts["build"]
    allow = texts["allow"]

    # ── AC1: soak harness attribution rows ──
    rz_pos = chaos.find("chaos_soak_hard_gate() || prod_gate")
    bits_pos = chaos.find("steal_safety_last_reject_invariant_bits_v_read")
    must(rz_pos >= 0, "AC1: soak residual gate row present (residual_zero_gate composition)")
    must(bits_pos >= 0, "AC1: soak reads the last RejectHard bit-set SSOT")
    must(rz_pos >= 0 and bits_pos > rz_pos, "AC1: bit-coverage rows live inside the soak residual gate")
    must("Issue #4161" in chaos, "AC1: soak block cites Issue #4161")
    must(
        '"#4161: rearm_race grew without matching RejectHard bits"' in chaos,
        "AC1: rearm attribution row (bits != 0 — no dedicated bit)",
    )
    must(
        '"#4161: envframe grew without matching RejectHard bit"' in chaos,
        "AC1: envframe attribution row (mask(EnvFrameOk))",
    )
    must(
        '"#4161: lifetime_proof grew without matching RejectHard bit"' in chaos,
        "AC1: lifetime_proof attribution row (mask(LifetimeProofOk))",
    )
    must("StealInvariant::EnvFrameOk" in chaos, "AC1: envframe mask via the StealInvariant table")
    must(
        "StealInvariant::LifetimeProofOk" in chaos,
        "AC1: lifetime_proof mask via the StealInvariant table",
    )
    # No weakening — the delta==0 rows stay pinned.
    must(
        '"#2755: residual_boundary_unsafe delta == 0 (SOAK hard / prod)"' in chaos,
        "AC1: #2755 boundary delta row intact",
    )
    must(
        '"#2902: residual_rearm_race delta == 0 (release blocker)"' in chaos,
        "AC1: #2902 rearm delta row intact",
    )
    must('"#3073: residual_envframe_lag delta == 0"' in chaos, "AC1: #3073 envframe row intact")
    must(
        '"#3073: residual_lifetime_proof_reject delta == 0"' in chaos,
        "AC1: #3073 lifetime_proof row intact",
    )

    # ── AC2: residual-zero face AC19 ──
    must(
        "--- AC19: RejectHard bit coverage on hard soak samples (#4161)" in residual,
        "AC2: residual-zero face AC19 header present",
    )
    must("Issue #4161" in residual, "AC2: face cites Issue #4161")
    must(
        '"#4161: rearm_race grew without matching RejectHard bits"' in residual,
        "AC2: face source-cites the rearm attribution row",
    )
    must(
        '"#4161: envframe grew without matching RejectHard bit"' in residual,
        "AC2: face source-cites the envframe attribution row",
    )
    must(
        '"#4161: lifetime_proof grew without matching RejectHard bit"' in residual,
        "AC2: face source-cites the lifetime_proof attribution row",
    )
    must("#4161" in residual and "#4159/#4161" in residual, "AC2: batch summary line cites #4161")

    # ── AC3: steal_safety SSOT intact (no second model) ──
    must(
        "last_reject_invariant_bits covering the arm" in ss_h,
        "AC3: #3001 contract comment intact in steal_safety.h",
    )
    must(
        "Issue #3001: chaos soak fail-closed if this arm grows without RejectHard" in ss_cpp,
        "AC3: #3001 envframe arm comment intact in steal_safety.cpp",
    )
    must(
        ss_cpp.count("g_steal_safety_last_reject_invariant_bits.store") >= 2,
        "AC3: both RejectHard store sites intact in steal_safety.cpp",
    )
    must(
        "steal_safety_last_reject_invariant_bits_v_read" in ss_h,
        "AC3: last-reject-bits accessor intact in steal_safety.h",
    )
    must(
        "g_steal_safety_last_reject_invariant_bits.store(0, std::memory_order_relaxed);" in ss_h,
        "AC3: test reset clears the bit-set (steal_safety.h)",
    )
    must("steal_invariant_mask(StealInvariant inv)" in ss_h, "AC3: mask helper intact")
    must("Count = 7," in ss_h, "AC3: StealInvariant table stays 7 bits (no new bit)")

    # ── AC4: wiring + no invent ──
    must("check_rejecthard_bit_coverage_4161.py" in build, "AC4: build.py runs the linter")
    must("check_rejecthard_bit_coverage_4161.py" in allow, "AC4: root_check_allowlist row present")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4161.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4161.cpp (per #81934)",
    )
    must(
        not glob.glob(str(ROOT / "docs" / "design" / "4161-*")),
        "AC4: no docs/design/4161-* (per #1655)",
    )
    for name, t in (("chaos soak harness", chaos), ("steal_safety.h", ss_h), ("build.py", build)):
        must("g_4161_" not in t, f"AC4: no new counters in {name}")
        must("schema-4161" not in t, f"AC4: no new query key in {name}")

    return failures


def main() -> int:
    texts = {
        "chaos": CHAOS.read_text(),
        "residual": RESIDUAL.read_text(),
        "ss_h": SS_H.read_text(),
        "ss_cpp": SS_CPP.read_text(),
        "build": BUILD.read_text(),
        "allow": ALLOW.read_text(),
    }
    if "--self-test" in sys.argv[1:]:
        real = run_rows(texts)
        broken = dict(texts)
        # Break one row per surface: drop the rearm attribution row (soak
        # harness) and the allowlist wiring row.
        broken["chaos"] = texts["chaos"].replace(
            '"#4161: rearm_race grew without matching RejectHard bits"', '"#4161: rearm row"', 1
        )
        broken["allow"] = texts["allow"].replace("check_rejecthard_bit_coverage_4161.py", "", 1)
        failed = run_rows(broken)
        ok = not real and bool(failed)
        print(
            f"check_rejecthard_bit_coverage_4161 self-test: real-tree rows clean={not real}, "
            f"broken-detection rows={len(failed)}"
        )
        if not failed:
            print("  self-test FAILED: checker did not detect the broken synthetic")
        for row in real:
            print(f"  real-tree FAIL: {row}")
        return 0 if ok else 1
    failures = run_rows(texts)
    if failures:
        print(f"\ncheck_rejecthard_bit_coverage_4161: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_rejecthard_bit_coverage_4161: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
