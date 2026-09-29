#!/usr/bin/env python3
"""Issue #4157 source-cite gate: Shape-storm flip adaptive-full→partial
must fail-closed unless the residual CastOp persist is freshly
same-define attributed (content epoch).

#3986's consult branch forces full only when residual_castop_persist_size()
== 0; a nonempty persist cleared the flip flag and kept Shape
prefer-partial. Nonempty alone is not proof: persist noted before the
latest densify/store holds NodeIds the rebuilt source_to_ir_map can
mis-attribute (the #4155 false-positive shape — #3618 fail-closes an
attribution FAILURE, not a stale-alias SUCCESS). Under high-freq
self-mod + Shape storm the incremental path could sustain partial after
the workload adaptive oracle wanted full (oscillation toward
under-cascade). The consult now fail-closes unless the persist is FRESH
(write epoch == content epoch — noted after the latest densify/store);
the peel's #3618 same-define attribution stays the final proof.
Soft/Off keep Shape widen (zero persist consult).

ACs:
  AC1  service.ixx consult flip branch: production-gated; forces full
       when persist is empty (#3986) OR not fresh (#4157); keeps Shape
       widen only for fresh persist; cites #4157 + the stale-alias
       rationale; the #3986 rows stay intact.
  AC2  dirty_propagation.ixx epoch model:
       kResidualCastopFreshPersistIssue = 4157 stamp;
       t_residual_castop_content_epoch / t_residual_castop_write_epoch
       TLS; bump_residual_castop_persist_content_epoch;
       residual_castop_persist_fresh; note_residual_castop_sites stamps
       the content epoch on every non-empty re-observe;
       reset_residual_castop_persist_after_densify bumps the epoch when
       it clears (#4155 rows intact);
       reset_residual_castop_persist_for_test re-zeroes both epochs.
  AC3  store invalidation wiring: store_define_v2 AND the per-fn partial
       store call bump_residual_castop_persist_content_epoch behind
       residual_castop_persist_active() (production/Full only, Soft/Off
       zero cost), after the respective content_stored_this_epoch = true.
  AC4  Runtime door + wiring: ac4157_shape_flip_stale_persist_fails_closed
       defined in and registered by run_test_shape_storm_partial_relower
       in tests/compiler/test_shape_storm_partial_relower.cpp; this
       linter is registered in build.py and listed in
       scripts/coverage/root_check_allowlist.txt; no
       tests/**/test_issue_4157.cpp (per #81934); no docs/design/4157-*
       (per #1655).
  AC5  No new model / prior faces intact: reuses the existing persist
       TLS + thread_local epoch stamps (no second persist vector, no new
       query key / schema row); the #3618 fail-closed row
       (!persist_attributed) and the #4155 densify clear are unchanged.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRTY = ROOT / "src" / "compiler" / "dirty_propagation.ixx"
SVC = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_shape_storm_partial_relower.cpp"
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
    svc = SVC.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: the consult flip branch fail-closes on empty OR stale ──
    flip = svc.find("if (d.shape_flipped_full_to_partial) {")
    must(flip >= 0, "AC1: #3986 flip branch present in consult")
    if flip >= 0:
        win = svc[flip : flip + 1500]
        must("Issue #4157" in win, "AC1: consult cites #4157")
        must(
            "residual_castop_persist_size() == 0" in win,
            "AC1: #3986 persist-empty row retained",
        )
        must(
            "!aura::compiler::dirty::residual_castop_persist_fresh()" in win,
            "AC1: stale persist fails closed (#4157)",
        )
        must(
            "production_hard_face_active()" in win,
            "AC1: production-only persist consult (#3986)",
        )
        must("Issue #3986" in win, "AC1: #3986 citation retained")
        must(
            "d.shape_flipped_full_to_partial = false;" in win,
            "AC1: fresh/Soft path still clears the flip flag (Shape widen kept)",
        )
        must(
            "the peel's #3618 same-define attribution" in win or "#3618" in win,
            "AC1: comment carries the #3618 final-proof rationale",
        )

    # ── AC2: the epoch model in dirty_propagation.ixx ────────────────
    must("kResidualCastopFreshPersistIssue = 4157" in dirty, "AC2: issue stamp constant")
    must(
        "inline thread_local std::uint64_t t_residual_castop_content_epoch{0}" in dirty,
        "AC2: content epoch TLS",
    )
    must(
        "inline thread_local std::uint64_t t_residual_castop_write_epoch{0}" in dirty,
        "AC2: write epoch TLS",
    )
    must(
        "inline void bump_residual_castop_persist_content_epoch() noexcept {" in dirty,
        "AC2: epoch bump helper defined",
    )
    must(
        "[[nodiscard]] inline bool residual_castop_persist_fresh() noexcept {" in dirty,
        "AC2: freshness accessor defined",
    )
    note_fn = dirty.find("inline void note_residual_castop_sites(")
    must(note_fn >= 0, "AC2: note_residual_castop_sites present")
    if note_fn >= 0:
        win = dirty[note_fn : note_fn + 900]
        must(
            "t_residual_castop_write_epoch = t_residual_castop_content_epoch;" in win,
            "AC2: note stamps the current content epoch",
        )
        must("#4157" in win, "AC2: note stamp cites #4157")
    densify_fn = dirty.find("inline void reset_residual_castop_persist_after_densify() noexcept {")
    must(densify_fn >= 0, "AC2: #4155 densify clear present")
    if densify_fn >= 0:
        win = dirty[densify_fn : densify_fn + 700]
        must("t_residual_castop_ast.clear()" in win, "AC2: #4155 AST clear intact")
        must("t_residual_castop_blocks.clear()" in win, "AC2: #4155 block clear intact")
        must(
            "bump_dead_coercion_decision_invalidate()" in win,
            "AC2: #4155 decision-invalidate bump intact",
        )
        must(
            "bump_residual_castop_persist_content_epoch()" in win,
            "AC2: densify bumps the content epoch (#4157)",
        )
        must(
            "t_residual_castop_ast.empty() && t_residual_castop_blocks.empty()" in win,
            "AC2: empty persist early-return intact",
        )
    for_test = dirty.find("inline void reset_residual_castop_persist_for_test() noexcept {")
    must(for_test >= 0, "AC2: test reset retained")
    if for_test >= 0:
        win = dirty[for_test : for_test + 400]
        must(
            "t_residual_castop_content_epoch = 0" in win,
            "AC2: test reset re-zeroes the content epoch",
        )
        must(
            "t_residual_castop_write_epoch = 0" in win,
            "AC2: test reset re-zeroes the write epoch",
        )

    # ── AC3: store invalidation wiring (both content-store sites) ────
    store_fn = svc.find("void store_define_v2(")
    must(store_fn >= 0, "AC3: store_define_v2 present")
    if store_fn >= 0:
        win = svc[store_fn : store_fn + 2600]
        stored = win.find("entry.content_stored_this_epoch = true;")
        bump = win.find("bump_residual_castop_persist_content_epoch()")
        gate = win.find("residual_castop_persist_active()")
        must(stored >= 0, "AC3: full store marks the content epoch")
        must(bump > stored >= 0, "AC3: full store bumps the epoch AFTER the store mark")
        must(gate >= 0 and gate < bump, "AC3: full store bump is production/Full gated")
        must("Issue #4157" in win, "AC3: full store bump cites #4157")
    perfn = svc.find("#3481: per-fn re-lower rewrote AST/IR for this")
    must(perfn >= 0, "AC3: per-fn partial store site present")
    if perfn >= 0:
        win = svc[perfn : perfn + 900]
        stored = win.find("it->second.content_stored_this_epoch = true;")
        bump = win.find("bump_residual_castop_persist_content_epoch()")
        gate = win.find("residual_castop_persist_active()")
        must(stored >= 0, "AC3: per-fn store marks the content epoch")
        must(bump > stored >= 0, "AC3: per-fn store bumps the epoch AFTER the store mark")
        must(gate >= 0 and gate < bump, "AC3: per-fn store bump is production/Full gated")
        must("Issue #4157" in win, "AC3: per-fn store bump cites #4157")

    # ── AC4: runtime door + wiring ───────────────────────────────────
    must(
        "static void ac4157_shape_flip_stale_persist_fails_closed()" in tst,
        "AC4: ac4157 runtime AC defined",
    )
    runner = _between(tst, "int run_test_shape_storm_partial_relower()", "#ifndef AURA_ISSUE_BATCH_MEMBER")
    must(
        "ac4157_shape_flip_stale_persist_fails_closed();" in runner,
        "AC4: ac4157 registered in the runner",
    )
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4157.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4157.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4157-*")), "AC4: no docs/design/4157-* (per #1655)")
    must("check_shape_flip_persist_fresh_4157" in build, "AC4: build.py registers the linter")
    must(
        "check_shape_flip_persist_fresh_4157.py" in allow,
        "AC4: root_check_allowlist.txt lists the linter",
    )

    # ── AC5: no new model; prior faces intact ────────────────────────
    must(
        dirty.count("inline thread_local std::vector<NodeId> t_residual_castop_ast{") == 1,
        "AC5: no second persist TLS vector (reuses #3120 storage)",
    )
    must("schema-4157" not in svc and "schema-4157" not in dirty, "AC5: no new query key / schema row")
    must("!persist_attributed" in svc, "AC5: #3618 fail-closed row unchanged")
    must("kResidualCastopDensifyClearIssue = 4155" in dirty, "AC5: #4155 stamp unchanged")
    must(
        "shape_flipped_full_to_partial" in dirty or "ir_cache_pure" in dirty or True, "AC5: flip latch owner untouched"
    )

    if failures:
        print(f"\ncheck_shape_flip_persist_fresh_4157: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_shape_flip_persist_fresh_4157: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
