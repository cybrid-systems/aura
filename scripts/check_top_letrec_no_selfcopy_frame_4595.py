#!/usr/bin/env python3
"""Issue #4595 source-cite gate: a top-level named let must not freeze
top_ behind a self-copy frame, and set! cell lookups must resolve the
NEWEST binding of a shadowed name.

Mechanism (5-line repro in the issue):
  (define x 0)
  (define (call th) (th))
  (let loop ((r 0)) (if (< r 0) (loop (+ r 1)) #t))
  (display (let ((x "local")) (call (lambda () x))))   ; -> 0, want local

The letrec lowering of the top-level named let registered the live top
env as a frozen SoA frame copy and repointed top_.parent_id() AT that
copy (#232-style registration). A lambda created directly in top_ then
captured an EMPTY live-safe frame parented at top_.parent_id()
(#4350/#4474) — its free-var walk stopped at the stale copy, so any
name that existed at named-let time resolved to the cell frozen there,
shadowing every later top-level let/define rebind of the same name
(the aura-rocks corrupted-merge check read the global `bad` counter
instead of the let*-bound corrupted string). Same family as #4353 /
#4352, on the closure-capture path.

ACs:
  AC1  evaluator_eval_flat.cpp (letrec arm): the #232 parent registration
       and repoints are KEPT verbatim (both branches register; the steal /
       panic-checkpoint / reclaim machinery walks the SoA frame graph —
       removing it crashed the chaos release blocker: steal-snapshot
       hard-fail, resume-fence surplus, invalid free); the #4595 comment
       documents that the frozen-self-copy shadow is fixed lookup-side.
  AC2  evaluator_env.cpp: live-safe EMPTY captures consult the LIVE top_
       before their SoA walk, FIBER-GATED (host-thread only — under
       concurrent fibers a live consult switches the resolved cell
       mid-flight and hung the chaos release blocker); the SoA walk keeps
       HEAD's #2566 root-frame order (frame scan first, live top after —
       reordering broke module-closure scoping, CI #4373 face);
       lookup_cell_ptr / lookup_cell_index match frame string
       bindings in REVERSE (SoA walk, legacy parent_ walk, live top_
       consult) so set! writes the newest cell after a top-level rebind;
       cites #4595.
  AC2  evaluator_env.cpp: lookup_cell_ptr and lookup_cell_index match
       frame string bindings in REVERSE — SoA walk (f.bindings_.rbegin()),
       legacy parent_ walk (p->bindings_.rbegin()), and live top_
       consult (auto tb = owner_->top_env().bindings(); reverse) — so
       set! writes the newest (shadowing) cell after a top-level let/
       define rebind leaves [old binding, fresh binding] in one env;
       cites #4595. The symid scans were already reverse; the string
       scans matched Env::lookup's #4353 reverse semantics.
  AC3  Runtime door extends the existing #4353 family test (per #81934):
       tests/compiler/test_env_lookup_soa_shadow_reverse.cpp defines
       ac3_4595_top_named_let_shadow / ac4_4595_letstar_variant /
       ac5_4595_setbang_consistency / ac6_4595_source_cite_gates and
       registers all four in run_test_env_lookup_soa_shadow_reverse;
       no tests/**/test_issue_4595.cpp; no docs/design/4595-* (per
       #1655).
  AC4  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
ENV = ROOT / "src" / "compiler" / "evaluator_env.cpp"
TST = ROOT / "tests" / "compiler" / "test_env_lookup_soa_shadow_reverse.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def main() -> int:
    flat = FLAT.read_text()
    env = ENV.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: registration kept verbatim; fix documented lookup-side ─
    must("#4595" in flat, "AC1: evaluator_eval_flat.cpp cites #4595")
    letrec_region = _between(
        flat,
        "// Issue #4595: this registration (and the repoint",
        "auto ci = alloc_cell(make_void());",
    )
    must(bool(letrec_region), "AC1: letrec fix window found")
    must(
        letrec_region.count("alloc_env_frame_from_env(letrec_parent)") == 2,
        "AC1: both branches still register the parent frame (chaos SoA graph)",
    )
    must(
        letrec_region.count("set_parent_id(eval_id)") == 1,
        "AC1: #232 repoint unchanged (first branch)",
    )
    must(
        letrec_region.count("set_parent_id(new_id)") == 1,
        "AC1: #232 repoint unchanged (else branch)",
    )
    must(
        "fixed lookup-side in Env::lookup /" in letrec_region,
        "AC1: the frozen-copy shadow is documented as fixed lookup-side",
    )

    # ── AC2: fiber-gated consults + walk order + reverse scans ─────
    must("#4595" in env, "AC2: evaluator_env.cpp cites #4595")
    must(
        env.count("aura::serve::g_current_fiber == nullptr && bindings_.empty()") == 3,
        "AC2: three fiber-gated empty-capture consults (lookup + both cell lookups)",
    )
    must(
        "Fiber-gated (#4595)" in env,
        "AC2: fiber-gate rationale documented",
    )
    must(
        env.count("auto tb4595 = owner_->top_env().bindings();") == 2,
        "AC2: cell-lookup empty-capture consults use tb4595",
    )
    must(
        env.count("(cap_snap_->bindings_.empty() && cap_snap_->bindings_symid_.empty())") == 3,
        "AC2: consults fire only for truly-empty captures (symid-only keeps capture order)",
    )

    # ── AC3: runtime door extends the #4353 family test ─────────────
    for fn in (
        "ac3_4595_top_named_let_shadow",
        "ac4_4595_letstar_variant",
        "ac5_4595_setbang_consistency",
        "ac6_4595_source_cite_gates",
    ):
        must(f"static void {fn}()" in tst, f"AC3: {fn} defined")
        must(
            f"{fn}();"
            in _between(tst, "int run_test_env_lookup_soa_shadow_reverse()", "#ifndef AURA_ISSUE_BATCH_MEMBER"),
            f"AC3: {fn} registered in run_test_env_lookup_soa_shadow_reverse",
        )
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4595.cpp"), recursive=True),
        "AC3: no tests/**/test_issue_4595.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4595-*")), "AC3: no docs/design/4595-* (per #1655)")

    # ── AC4: wiring ─────────────────────────────────────────────────
    must("check_top_letrec_no_selfcopy_frame_4595.py" in build, "AC4: build.py registers the linter")
    must("check_top_letrec_no_selfcopy_frame_4595.py" in allow, "AC4: root_check_allowlist.txt lists the linter")

    if failures:
        print(f"\ncheck_top_letrec_no_selfcopy_frame_4595: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_top_letrec_no_selfcopy_frame_4595: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
