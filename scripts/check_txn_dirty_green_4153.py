#!/usr/bin/env python3
"""Issue #4153 source-cite gate: the nested / atomic_batch txn-dirty
transition must drop the PRIOR outermost TypeLinearCommitProof green face.

Lockless helpers intentionally skip infer (#3686/#3658); typecheck is
deferred to the outermost dtor. While depth>0 and the AST is already
mutated, ir_typed_entry_commit_readiness_ok and linear_move_drop_elision_ok
still consulted the PREVIOUS green stamp + a live TC that may still report
SOLVED (no new constraints yet), so mid-boundary IR/JIT / eval executed
half-typed state under high-frequency lockless mutate. The outermost gates
(#3655/#3658/#3653) close durable persist, but the mid-window was a
half-green EXECUTE residual.

ACs:
  AC1  evaluator.ixx :: note_txn_dirty flips the flag via exchange and
       gates aura::compiler::typed_audit::invalidate_green_face_on_txn_dirty
       on the 0→1 transition (already-dirty re-notes are idempotent — no
       gen churn on nested enters). The comment cites #4153.
  AC2  typed_mutation_audit.h :: invalidate_green_face_on_txn_dirty is
       production/Full-gated (Soft/Off early-out BEFORE any store — no
       extra atomics), then bumps g_txn_dirty_green_invalidate_total,
       advances the #3032 g_rehydrate_miss_invalidate_gen SSOT with
       release, and clears the green face bits (would_allow / linear_ok /
       stamper). No second proof model: the gen SSOT stays single. The
       comment cites #4153 and the #4152 fence coordination.
  AC3  Consult surfaces ride the SSOT: ir_typed_entry_commit_readiness_ok
       keeps the #3379 gen arm, linear_move_drop_elision_ok keeps the #3099
       try_skip re-sample, and the #4152 deferred-green fence keeps its
       gen-at-defer-arm compare (the advance between defer and commit
       refuses the deferred green); publish_last_proof_face remains the
       sole green rebind. The outermost #3658/#3655/#3653 gates in
       evaluator_mutation_boundary.cpp are untouched.
  AC4  The enter-path caller (depth > 1 || bump_suppressed_at_entry) stays
       the only production note_txn_dirty site; txn_dirty()/clear_txn_dirty
       flags unchanged; no new query key, no schema-4153, no new metrics
       bus.
  AC5  Runtime doors extend tests/compiler/test_type_linear_commit_health.cpp
       (ac4153_1..ac4153_6 defined and dispatched in
       run_test_type_linear_commit_health); no tests/**/test_issue_4153.cpp
       (per #81934); no docs/design/4153-* (per #1655); build.py wires this
       linter and scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TMH = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
EVIXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EMB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TEST = ROOT / "tests" / "compiler" / "test_type_linear_commit_health.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

rows: list[tuple[bool, str]] = []


def check(ok: bool, label: str) -> None:
    rows.append((bool(ok), label))


def main() -> int:
    tmh = TMH.read_text()
    ex = EVIXX.read_text()
    emb = EMB.read_text()
    test = TEST.read_text()

    # ── AC1: the note site gates the invalidate on the 0→1 transition ──
    note = ex.find("void note_txn_dirty() noexcept {")
    swap = ex.find("txn_dirty_.exchange(1, std::memory_order_relaxed);", note)
    guard = ex.find("if (!was)", note)
    call = ex.find("(void)aura::compiler::typed_audit::invalidate_green_face_on_txn_dirty();", note)
    cite = ex.find("#4153", note)
    check(
        -1 not in (note, swap, guard, call, cite) and swap < guard < call,
        "AC1: note_txn_dirty exchange-gates the invalidate on the 0→1 transition",
    )
    check(
        ex.find("#2105", note) != -1,
        "AC1: #2105 Agent-visible flag rationale kept",
    )

    # ── AC2: the helper shape — Soft early-out, gen SSOT, face clears ──
    check("kTxnDirtyGreenInvalidateIssue = 4153" in tmh, "AC2: issue constant stamp")
    fn = tmh.find("[[nodiscard]] inline bool invalidate_green_face_on_txn_dirty")
    hard = tmh.find("const bool hard = production_defaults_active()", fn)
    softout = tmh.find("return false;", fn)
    counter = tmh.find("g_txn_dirty_green_invalidate_total.fetch_add(1, std::memory_order_relaxed);", fn)
    gen = tmh.find("g_rehydrate_miss_invalidate_gen.fetch_add(1, std::memory_order_release);", fn)
    face_wa = tmh.find("g_last_proof_would_allow_commit.store(0, std::memory_order_relaxed);", fn)
    face_lo = tmh.find("g_last_proof_linear_ok.store(0, std::memory_order_relaxed);", fn)
    face_st = tmh.find("g_last_proof_stamper_eval.store(0, std::memory_order_relaxed);", fn)
    check(
        -1 not in (fn, hard, softout, counter, gen, face_wa, face_lo, face_st)
        and hard < softout < counter < gen < face_wa < face_lo < face_st,
        "AC2: Soft/Off early-out precedes counter + release gen advance + face clears",
    )
    check(
        tmh.count("inline std::atomic<std::uint64_t> g_rehydrate_miss_invalidate_gen{") == 1,
        "AC2: single invalidate-gen SSOT (no second proof model)",
    )
    check(
        tmh.find("[[nodiscard]] inline std::uint64_t txn_dirty_green_invalidate_total_v_read") != -1,
        "AC2: counter v_read surface present",
    )
    coord = tmh.find("#4152", tmh.find("// Issue #4153: nested / atomic_batch txn-dirty"))
    check(coord != -1, "AC2: helper cites the #4152 fence coordination")

    # ── AC3: consult surfaces + fence + restamp ownership preserved ──
    entry_fn = tmh.find("[[nodiscard]] inline bool ir_typed_entry_commit_readiness_ok")
    entry_gen = tmh.find("g_rehydrate_miss_invalidate_gen.load(std::memory_order_acquire) !=", entry_fn)
    check(entry_fn != -1 and entry_gen != -1, "AC3: ir_typed_entry keeps the #3379 gen arm")
    elide_fn = tmh.find("[[nodiscard]] inline bool linear_move_drop_elision_ok")
    elide_skip = tmh.find("linear_ir_fastpath_try_skip()", elide_fn)
    check(elide_fn != -1 and elide_skip != -1, "AC3: elision keeps the try_skip #3099 re-sample")
    fence = tmh.find(
        "g_rehydrate_miss_invalidate_gen.load(std::memory_order_acquire) !=\n                 g_tls_deferred_outermost_green_invalidate_gen"
    )
    fence_alt = tmh.find(
        "g_tls_deferred_outermost_green_invalidate_gen", tmh.find("commit_deferred_outermost_green_proof")
    )
    check(fence != -1 or fence_alt != -1, "AC3: #4152 fence gen-at-defer-arm compare intact")
    check(
        tmh.find("inline void publish_last_proof_face(bool would_allow, bool linear_ok)") != -1,
        "AC3: publish_last_proof_face remains the sole green rebind",
    )
    for gate in ("run_post_mutate_typecheck_no_lock", "boundary_solve_proof_gate", "finish_mutate_hard_gate"):
        check(gate in emb, f"AC3: outermost gate {gate} untouched in the boundary dtor")

    # ── AC4: single production note site; no invented observability ──
    note_site = emb.find("        note_txn_dirty();")
    check(
        note_site != -1
        and emb.find("if (depth > 1 || bump_suppressed_at_entry)", max(0, note_site - 200), note_site) != -1,
        "AC4: enter path (depth>1 || atomic_batch) stays the production note site",
    )
    check("schema-4153" not in tmh, "AC4: no new query key")
    check(
        "void note_txn_dirty() noexcept {" in ex and "void clear_txn_dirty() noexcept" in ex, "AC4: flag pair unchanged"
    )

    # ── AC5: runtime doors + wiring + allowlist + no invent ──
    acs = [
        "ac4153_1_mid_entry_refuses_stale_green",
        "ac4153_2_elision_blocked_while_txn_dirty",
        "ac4153_3_outermost_restamp_reopens",
        "ac4153_4_idempotent_note_abort_stays_nongreen",
        "ac4153_5_soft_no_extra_atomics",
        "ac4153_6_source_cite_invalidate_shape",
    ]
    runner = test.find("int run_test_type_linear_commit_health()")
    check(runner != -1, "AC5: dispatched runner present")
    for name in acs:
        defined = test.find(f"static void {name}()") != -1
        called = test.find(f"    {name}();", runner) != -1
        check(defined and called, f"AC5: {name} defined and dispatched")
    check(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4153.cpp"), recursive=True),
        "AC5: no tests/**/test_issue_4153.cpp (#81934)",
    )
    check(
        not glob.glob(str(ROOT / "docs" / "design" / "4153-*")),
        "AC5: no docs/design/4153-* (#1655)",
    )
    check(
        "check_txn_dirty_green_4153.py" in BUILD.read_text(),
        "AC5: build.py wires this linter",
    )
    check(
        "check_txn_dirty_green_4153.py" in ALLOWLIST.read_text(),
        "AC5: root_check_allowlist.txt lists this linter",
    )

    failed = [label for ok, label in rows if not ok]
    for ok, label in rows:
        print(("PASS  " if ok else "FAIL  ") + label)
    if failed:
        print(f"\n{len(failed)} row(s) failed")
        return 1
    print(f"\nall {len(rows)} rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
