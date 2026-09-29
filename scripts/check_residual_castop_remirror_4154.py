#!/usr/bin/env python3
"""Issue #4154 source-cite gate: DeadCoercion / residual CastOp remirror
must not let an added==0 short-circuit leave the type∪IR union cone empty
for persisted sites (miss stamp → miss revalidate under production).

Production remutate re-enters typecheck because #3065/#3120 keep eliminated
CastOp / identity sites inside the type∪IR dirty cone. When the persist span
is nonempty but the AST arm adds nothing (seen-set hit after a SOA columnar
dirty rebuild / IR-only CastOp site without a durable AST or column mark),
the union cone can stay empty for those sites: incremental typecheck skips,
and IR may still execute the prior lowering. The remirror must
durability-probe the persisted sites and latch the existing #3031
pending_full_solve readiness face when nothing is durably marked (no new
model); commit / typed-entry refuse until a successful remirror or full
re-infer. Soft / Off stay return-0 with no permanent bits.

ACs:
  AC1  dirty_propagation.ixx remirror_persisted_residual_castops: after the
       #3065/#3120 force arms, a nonempty persist span with n_ast+n_blk==0
       durability-probes the sites (residual_persist_sites_durable) and
       latches note_pending_full_solve_residual(n, /*hard=*/true) when
       nothing is durably marked; cites #4154.
  AC2  Benign keep-alive composes: the probe scans persisted AST dep marks
       (encode_ast_dep_node) and IR block marks (is_dirty) and returns true
       when any survives, so the #3347 steady state (sites already in cone +
       durably marked) stays latch-free and the #3120/#3228 wipe → re-add
       flow is untouched (latch comment cites the #3347 AC3 compose).
  AC3  typed_mutation_audit.h commit_readiness_live_policy: the C ABI
       remirror runs before the pending_full_solve_residual face read so the
       same fill observes the #4154 latch (comment cites #4154); the #3031
       hard-deny arm (reason pending_full_solve_residual, bp 700) and the
       Soft observe-allow arm (bp 7200) are unchanged.
  AC4  No new model / no invent: the latch reuses
       note_pending_full_solve_residual (no new face atomic / query key /
       schema-4154 row); the runtime door extends
       tests/compiler/test_dead_coercion_dirty_cone.cpp (ac4154_1..ac4154_4
       registered in run_test_dead_coercion_dirty_cone); no
       tests/**/test_issue_4154.cpp (per #81934); no docs/design/4154-*
       (per #1655); build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
  AC5  Soft / Off unchanged: the latch sits behind the
       residual_castop_persist_active() gate and the nonempty-persist early
       return (Soft never reaches it — return 0, no permanent dirty bits);
       the #3228 Soft zero-cost shape is preserved.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRTY = ROOT / "src" / "compiler" / "dirty_propagation.ixx"
TMH = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
TST = ROOT / "tests" / "compiler" / "test_dead_coercion_dirty_cone.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, row: str) -> None:
    if not cond:
        failures.append(row)


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i:j] if j > i else text[i:]


def main() -> int:
    dirty = DIRTY.read_text()
    tmh = TMH.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: remirror durability probe + existing #3031 face latch ──
    remirror = _between(
        dirty,
        "inline std::size_t remirror_persisted_residual_castops() noexcept",
        "inline constexpr int kResidualCastopUndermarkConeIssue",
    )
    must("Issue #4154" in remirror, "AC1: remirror cites #4154")
    must("n_ast + n_blk == 0" in remirror, "AC1: latch gated on added==0 with nonempty span")
    must("residual_persist_sites_durable()" in remirror, "AC1: durability probe consulted")
    must(
        "note_pending_full_solve_residual(" in remirror and "/*hard=*/true" in remirror,
        "AC1: latches existing #3031 face with production hard=true",
    )
    must("return n_ast + n_blk;" in remirror, "AC1: remirror return shape unchanged")
    probe = _between(
        dirty,
        "// Issue #4154: durability probe",
        "// Issue #3120: after type-txn wipe",
    )
    must("Issue #4154" in probe, "AC1: durability probe cites #4154")
    must(
        "encode_ast_dep_node(nid)" in probe and "g_global_dirty.is_dirty" in probe,
        "AC1: probe scans persisted AST dep marks",
    )
    must("is_dirty(enc)" in probe, "AC1: probe scans persisted IR block marks")

    # ── AC2: benign keep-alive composes (#3347 AC3 / #3120 / #3228) ──
    must(probe.count("return true;") == 2, "AC2: any surviving AST or block mark is durable")
    must("#3347" in remirror, "AC2: latch comment cites the #3347 keep-alive compose")

    # ── AC3: live policy observes the latch in-fill; arms unchanged ──
    lp_start = tmh.find("[[nodiscard]] inline CommitReadinessInput commit_readiness_live_policy() noexcept {")
    must(lp_start >= 0, "AC3: live policy found")
    lp = tmh[lp_start : lp_start + 6000]
    remirror_call = lp.find("(void)aura_force_residual_castop_undermark_into_cone();")
    face_read = lp.find("pending_full_solve_residual_face_hit()")
    must(0 <= remirror_call < face_read, "AC3: C ABI remirror runs before the face read")
    must("#4154:" in lp, "AC3: live policy cites #4154 ordering rationale")
    must(
        'set("pending_full_solve_residual", false, 700)' in tmh,
        "AC3: #3031 production hard-deny arm unchanged (bp 700)",
    )
    must(
        'set("pending_full_solve_residual", true, 7200)' in tmh,
        "AC3: #3031 Soft observe-allow arm unchanged (bp 7200)",
    )

    # ── AC4: no new model / no invent / wiring ──────────────────────
    must(
        "note_pending_full_solve_residual" in remirror,
        "AC4: latch reuses the existing #3031 face setter (no new model)",
    )
    must(
        "static void ac4154_1_miss_column_latches_face()" in tst,
        "AC4: ac4154_1 defined in test_dead_coercion_dirty_cone.cpp",
    )
    must(
        "static void ac4154_2_keep_alive_stays_clear()" in tst,
        "AC4: ac4154_2 defined",
    )
    must(
        "static void ac4154_3_readiness_deny_and_authority()" in tst,
        "AC4: ac4154_3 defined",
    )
    must(
        "static void ac4154_4_quiet_soft_source_cite()" in tst,
        "AC4: ac4154_4 defined",
    )
    runner = _between(tst, "int run_test_dead_coercion_dirty_cone()", "#ifndef AURA_ISSUE_BATCH_MEMBER")
    must("ac4154_1_miss_column_latches_face();" in runner, "AC4: ac4154_1 registered in runner")
    must("ac4154_2_keep_alive_stays_clear();" in runner, "AC4: ac4154_2 registered in runner")
    must("ac4154_3_readiness_deny_and_authority();" in runner, "AC4: ac4154_3 registered in runner")
    must("ac4154_4_quiet_soft_source_cite();" in runner, "AC4: ac4154_4 registered in runner")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4154.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4154.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4154-*")), "AC4: no docs/design/4154-* (per #1655)")
    must("check_residual_castop_remirror_4154.py" in build, "AC4: build.py registers the linter")
    must(
        "check_residual_castop_remirror_4154.py" in allow,
        "AC4: root_check_allowlist.txt lists the linter",
    )

    # ── AC5: Soft / Off unchanged (return 0, no permanent bits) ─────
    soft_arm = _between(dirty, "inline std::size_t force_dead_coercion_elim_into_cone", "Issue #4154: durability probe")
    must(
        "if (!residual_castop_persist_active())" in remirror
        and remirror.find("return 0;") < remirror.find("note_pending_full_solve_residual("),
        "AC5: latch sits behind the production/Full persist gate",
    )
    must(
        "if (!production_defaults_active() && get_strategy() != AuditStrategy::Full)" in soft_arm,
        "AC5: #3065 force keeps its Soft observe-only gate (no permanent bits)",
    )

    if failures:
        print(f"\ncheck_residual_castop_remirror_4154: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_residual_castop_remirror_4154: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
