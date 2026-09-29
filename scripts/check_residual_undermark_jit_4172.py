#!/usr/bin/env python3
"""Issue #4172 source-cite gate: residual CastOp undermark pre-JIT verification.

The DeadCoercion dual-layer (AST apply_coercion_map identity/Dynamic elision +
IR DeadCoercionEliminationPass) plus the #3228/#3347 undermark persist can
still leave a residual CastOp that lowers into interpreter/JIT hot functions:
the #3689 guard skips the #3007 production residual sweep exactly when DCE
was cone-limited, and nothing re-runs the elimination on the remirrored
persist sites before the relowered body reaches JIT. Under a hard density
face the leftover must fail the mutate commit, not silently JIT.

ACs:
  AC1  optimization_passes.ixx hosts verify_residual_undermark_post_peel_4172:
       gated on production_hard_face_active + the #3347 undermark latch; a
       full peel re-runs DeadCoercionEliminationPass over the fresh body; a
       partial peel counts survivors only (no #3689 mixed-IR reopen); only
       unannotated (narrow_evidence==0) survivors drive the reject; cites
       #4172.
  AC2  castop_density_policy.hh arms the EXISTING density gate reject
       (#3699 g_density_gate_reject_pending + g_gate_reject_total +
       mutate_type_gate hard reject counter) — no second reject model, no
       streak hop (the undermark latch already proves the cone miss);
       reset-for-test hygiene present.
  AC3  service.ixx relower arms the #4172 face
       (production_hard_face_active && residual_castop_undermark_pending),
       forces the persist sites into each entry's peel mask
       (mark_entry_from_dead_coercion_persist_ under the face), and runs
       the post-peel verification after BOTH peel branches
       (want_partial true + false).
  AC4  the reject fires only under a hard density face
       (force_jit_path_enabled) — Soft / Off observe counters only and the
       commit may succeed; no second pending latch is invented (the #3699
       g_density_gate_reject_pending stays the single reject SSOT).
  AC5  tests cite #4172 runtime ACs in test_dead_coercion_dirty_cone.cpp
       (batch member of test_occurrence_coercion_batch); no
       tests/compiler/test_issue_4172.cpp; no docs/design/4172-*;
       build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POL = ROOT / "src" / "compiler" / "castop_density_policy.hh"
OPT = ROOT / "src" / "compiler" / "optimization_passes.ixx"
SVC = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_dead_coercion_dirty_cone.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def fail(msg: str) -> int:
    print(f"check_residual_undermark_jit_4172: FAIL {msg}")
    return 1


def _body(text: str, start_marker: str, end_marker: str) -> str:
    """Region from start_marker up to the next end_marker occurrence."""
    idx = text.find(start_marker)
    if idx < 0:
        return ""
    end = text.find(end_marker, idx + len(start_marker))
    return text[idx:end] if end > 0 else text[idx:]


def main() -> int:
    pol = POL.read_text()
    opt = OPT.read_text()
    svc = SVC.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: verifier shape in optimization_passes.ixx ──
    if "verify_residual_undermark_post_peel_4172" not in opt:
        return fail("AC1 verifier missing from optimization_passes.ixx")
    ver = _body(
        opt,
        "Issue #4172: post-peel residual-undermark verification",
        "Issue #2611: re-export dce deopt-meta counter names",
    )
    if not ver:
        return fail("AC1 verifier region not found")
    if "#4172" not in ver:
        return fail("AC1 verifier must cite #4172")
    if "production_hard_face_active()" not in ver:
        return fail("AC1 verifier must gate on production_hard_face_active()")
    if "residual_castop_undermark_pending()" not in ver:
        return fail("AC1 verifier must gate on the #3347 undermark latch")
    if "DeadCoercionEliminationPass dce(reg);" not in ver:
        return fail("AC1 full peel must re-run DeadCoercionEliminationPass")
    if "want_partial" not in ver or "++unannotated_leftover;" not in ver:
        return fail("AC1 verifier must branch full/partial and count survivors")
    if "narrow_evidence == 0" not in ver:
        return fail("AC1 only unannotated (narrow_evidence==0) survivors count")

    # ── AC2: reject reuses the existing density gate (no second model) ───
    if "note_residual_undermark_hard_reject" not in pol:
        return fail("AC2 policy reject helper missing")
    if "kResidualUndermarkRejectIssue = 4172" not in pol:
        return fail("AC2 issue constant missing")
    rej = _body(
        pol, "inline void note_residual_undermark_hard_reject", "inline void reset_residual_undermark_reject_for_test"
    )
    if not rej:
        return fail("AC2 reject helper region not found")
    if "g_density_gate_reject_pending.store(1, std::memory_order_release);" not in rej:
        return fail("AC2 reject must arm the existing #3699 gate pending")
    if "g_gate_reject_total().fetch_add(1" not in rej:
        return fail("AC2 reject must bump the existing gate reject total")
    if "mutate_type_gate::g_hard_type_error_reject_total" not in rej:
        return fail("AC2 reject must bump the MutateTypeGate hard reject counter")
    if "g_density_streak" in rej:
        return fail("AC2 reject must not streak-hop (undermark latch already proves the miss)")
    if "reset_residual_undermark_reject_for_test" not in pol:
        return fail("AC2 reset-for-test hygiene missing")

    # ── AC3: relower arms the face, forces the mask, verifies both branches ──
    if svc.count("residual_undermark_face_4172") < 4:
        return fail("AC3 face arm + per-entry force + two peel arms expected")
    arm = _body(svc, "Issue #4172: pre-JIT residual CastOp verification face", "Snapshot names first")
    if "production_hard_face_active()" not in arm or "residual_castop_undermark_pending()" not in arm:
        return fail("AC3 face must gate on hard face AND undermark latch")
    force = _body(svc, "Issue #4172: force the persisted CastOp sites", "Issue #3484:")
    if "mark_entry_from_dead_coercion_persist_(it->second);" not in force:
        return fail("AC3 undermark face must force the persist into the peel mask")
    if "force_residual_castop_undermark_into_cone" not in force:
        return fail("AC3 undermark face must re-mirror into the type∪IR cone")
    if svc.count("verify_residual_undermark_post_peel_4172") < 2:
        return fail("AC3 post-peel verification must run on BOTH peel branches")
    if "/*want_partial=*/true" not in svc or "/*want_partial=*/false" not in svc:
        return fail("AC3 partial + full verification arms missing")

    # ── AC4: density hard gates the reject; Soft observes only ──
    if "force_jit_path_enabled()" not in ver:
        return fail("AC4 reject must be gated on the hard density face")
    if "Reuse the existing #3699 density gate reject plumbing" not in pol:
        return fail("AC4 must document the reuse (single reject SSOT)")
    if "g_residual_undermark_pending" in pol:
        return fail("AC4 must not invent a second pending latch")

    # ── AC5: runtime ACs + no invent + build/allowlist wiring ──
    for row in (
        "ac4172_1_production_hard_reject",
        "ac4172_2_full_peel_reelides_identity",
        "ac4172_3_soft_disarmed",
        "ac4172_4_annotated_leftover_keeps",
        "ac4172_5_partial_counts_without_reelide",
        "ac4172_6_schema_and_source",
        "verify_residual_undermark_post_peel_4172",
    ):
        if row not in tst:
            return fail(f"AC5 runtime door missing: {row}")
    if (ROOT / "tests" / "compiler" / "test_issue_4172.cpp").exists():
        return fail("AC5 test_issue_4172.cpp must not exist (#81934)")
    if list(ROOT.glob("docs/design/4172-*")):
        return fail("AC5 docs/design/4172-* must not exist (#1655)")
    if "check_residual_undermark_jit_4172" not in build:
        return fail("AC5 build.py must wire this linter")
    if "check_residual_undermark_jit_4172.py" not in allow:
        return fail("AC5 root_check_allowlist.txt must list this linter")

    print("check_residual_undermark_jit_4172: OK (5 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
