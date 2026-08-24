#!/usr/bin/env python3
"""Issue #3290: residual hard-AND invariant table must be machine-checkable
at every call site (I3 residual @ e2ac485).

The StealInvariant table (#2929) is runtime-only: steal_safety_transaction +
evaluate_residual_hard_and_bits hard-AND the 7 arms under the per-Fiber
decision window, and production multi-worker refuses weak ABI stubs. #3072
already proves every stolen-fiber Ready enqueue is dominated by
steal_safety_transaction Ok. The residual gap: no compile-time / linter
guarantee that (a) the resume path enters the invariant check, (b) every
arm appears in BOTH the evaluate path and the last_reject_invariant_bits
publish, (c) mailbox delivery uses the shared residual evaluator.

Gate rows:
  G1  resume path: Fiber::resume (fiber.cpp) calls
      check_and_enforce_resume_invariants BEFORE ::swapcontext — a resume
      bypass can run inconsistent snapshot / stale-ticket code.
  G2  every StealInvariant arm appears in steal_safety.cpp's evaluate path
      (evaluate_residual_hard_and_bits body; SnapshotConsistent is the
      transaction's inconsistency branch) AND in note_steal_invariant_fail
      (dedicated fail counter per arm).
  G3  last_reject_invariant_bits publish exists in the RejectHard path and
      carries the arm bit-set (fail_bits from evaluate / SnapshotConsistent
      explicit mask) — no silent RejectHard without arm attribution.
  G4  mailbox delivery uses the shared evaluator:
      mailbox_delivery_safety_transaction → evaluate_residual_hard_and_bits.
  G5  Soft / quiet path unchanged: no new runtime atomics / counters
      (linter is offline; no g_3290_*).
  G6  test ACs in src-aligned suite (#81967); no test_issue_3290.cpp;
      no docs/design/ (#1655).
  G7  build.py wires this linter.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _function_window(text: str, fn_name: str, size: int = 12000) -> str:
    pos = text.find(fn_name)
    if pos < 0:
        return ""
    return text[pos : pos + size]


def _enum_arms(hdr: str) -> list[str]:
    """Parse StealInvariant enum member names (skip Count)."""
    # Underlying type is std::uint8_t — allow [: \w:]+ between the name
    # and the opening brace.
    m = re.search(r"enum class StealInvariant\s*(?::\s*[\w:]+)?\s*\{(.*?)\}", hdr, re.S)
    if not m:
        return []
    arms: list[str] = []
    for line in m.group(1).splitlines():
        line = line.split("//", 1)[0].strip()
        if not line or line.startswith("Count"):
            continue
        for name in re.findall(r"([A-Za-z][A-Za-z0-9_]*)\s*=", line):
            if name != "Count":
                arms.append(name)
    return arms


def main() -> int:
    hdr = _read("src/serve/steal_safety.h")
    ss = _read("src/serve/steal_safety.cpp")
    fc = _read("src/serve/fiber.cpp")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")
    lint3072 = _read("scripts/coverage/checks/check_steal_enqueue_sole_gate_3072.py")

    # ── G1: resume path enters the invariant check before swapcontext ──
    resume = _function_window(fc, "void Fiber::resume()", 6000)
    must("check_and_enforce_resume_invariants" in resume, "G1: Fiber::resume calls the consolidated invariant check")
    sw = resume.find("::swapcontext")
    chk = resume.find("check_and_enforce_resume_invariants()")
    must(chk >= 0 and sw >= 0 and chk < sw, "G1: invariant check precedes swapcontext (no resume bypass)")
    # The #2677 consolidation comment sits immediately BEFORE the
    # function declaration — anchor the window back to cover it.
    decl = fc.find("check_and_enforce_resume_invariants() noexcept")
    cite_win = fc[decl - 1500 : decl + 500] if decl >= 0 else ""
    must(
        "Issue #2677" in cite_win and "single call site" in cite_win,
        "G1: consolidated check cites #2677 single call site",
    )

    # ── G2: every arm in evaluate path + note_steal_invariant_fail ──
    eval_win = _function_window(ss, "evaluate_residual_hard_and_bits", 12000)
    note_win = _function_window(ss, "note_steal_invariant_fail", 4000)
    arms = _enum_arms(hdr)
    must(len(arms) >= 7, f"G2: parsed {len(arms)} StealInvariant arms (expect >= 7)")
    for arm in arms:
        # SnapshotConsistent is the transaction's inconsistency branch
        # (mutation_safety_snapshot_inconsistent), not the evaluate body.
        if arm == "SnapshotConsistent":
            must(
                "mutation_safety_snapshot_inconsistent" in ss,
                f"G2: {arm} evaluated via transaction inconsistency branch",
            )
            must(f"StealInvariant::{arm}" in ss, f"G2: {arm} present in steal_safety.cpp")
        else:
            must(f"StealInvariant::{arm}" in eval_win, f"G2: {arm} in evaluate_residual_hard_and_bits")
        must(f"StealInvariant::{arm}" in note_win, f"G2: {arm} has dedicated fail counter (note_steal_invariant_fail)")

    # ── G3: last_reject publish carries the arm bit-set ──
    txn = _function_window(ss, "StealSafetyDecision steal_safety_transaction(", 12000)
    must(
        "g_steal_safety_last_reject_invariant_bits.store" in txn, "G3: RejectHard publishes last_reject_invariant_bits"
    )
    must("fail_bits" in txn, "G3: residual RejectHard stores evaluate fail_bits")
    must(
        "steal_invariant_mask(StealInvariant::SnapshotConsistent)" in txn,
        "G3: SnapshotConsistent publishes explicit mask",
    )
    for arm in arms:
        if arm != "SnapshotConsistent":
            must(
                f"steal_invariant_mask(StealInvariant::{arm})" in eval_win,
                f"G3: {arm} bit-set built in evaluate (mask)",
            )
    must("g_steal_safety_last_reject_invariant_bits" in hdr, "G3: last_reject_invariant_bits declared in header")

    # ── G4: mailbox delivery uses the shared residual evaluator ──
    mb_win = _function_window(ss, "mailbox_delivery_safety_transaction", 6000)
    must("evaluate_residual_hard_and_bits" in mb_win, "G4: mailbox delivery shares the residual evaluator")
    must(
        "mailbox never bumps steal counters" in mb_win or "/*bump_counters=*/false" in mb_win,
        "G4: mailbox uses quiet evaluator (no steal counter bumps)",
    )

    # ── G5: Soft / quiet path unchanged (linter offline, no new counters) ──
    must(
        "g_3290_" not in hdr and "g_3290_" not in ss and "g_3290_" not in fc, "G5: no new g_3290_* counter in serve TUs"
    )
    must("evaluate_residual_hard_and_bits" in eval_win, "G5: shared evaluator intact (no new runtime framework)")

    # ── G6: src-aligned suite home (#81967) ──
    must("ac3290_1_resume_no_bypass" in test, "G6: AC1 test present")
    must("ac3290_2_arms_evaluate_and_publish" in test, "G6: AC2 test present")
    must("ac3290_3_mailbox_shared_evaluator" in test, "G6: AC3 test present")
    must("ac3290_4_soft_quiet_unchanged" in test, "G6: AC4 test present")
    must("ac3290_5_source_and_linter" in test, "G6: AC5 test present")
    must("ac3290_6_no_invent" in test, "G6: AC6 test present")
    must(not _read("tests/serve/test_issue_3290.cpp"), "G6: no tests/serve/test_issue_3290.cpp per #81967")
    must(not _read("tests/issues/test_issue_3290.cpp"), "G6: no tests/issues/test_issue_3290.cpp per #81967")

    # ── G7: build.py wiring + #3072 lineage preserved ──
    must("check_steal_invariant_call_site_coverage_3290" in build, "G7: build.py wires linter")
    must("Issue #3072" in lint3072, "G7: #3072 sole-enqueue linter preserved")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        bad = [f.name for f in sorted(docs.glob("3290-*"))]
        must(not bad, "G7: no docs/design/3290-* per #1655")
    else:
        must(True, "G7: no docs/design/3290-* per #1655")

    if failures:
        print(f"\n#3290 linter: {len(failures)} gate(s) FAILED")
        return 1
    print("\n#3290 linter: all gates OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
