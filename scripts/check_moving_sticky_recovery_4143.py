#!/usr/bin/env python3
"""Issue #4143 source-cite gate: sticky densify-off recovery clear ordering.

`Evaluator::recover_moving_sticky_densify_off` used to clear the sticky
densify-off trap at entry (step b) and only then optionally run the one-shot
densify retry (step c). Between the clear and the unified-green publish (or
on the #3884 LCP-deny re-arm path) `moving_compact_enabled()` read true while
pin / LCP cover could still be incomplete, so a concurrent Phase-5 / auto-arm
Moving or a peer `try_acquire` (which also enters this recover via the
densify throttle) could admit relocate under incomplete cover — a UAF /
miss-remap race window. #4143: the trap stays armed through the whole
recovery; the recovery entrant alone gates its one-shot on the feature flag
(`moving_compact_feature_enabled()`, mirrors live_compact(Moving) #3123 AC3),
and the sticky clears only after the retry publishes a unified-green window
(pin held ∧ ¬incomplete ∧ untracked==0 ∧ root_fail==0 — same surface as
`compute_moving_unified_success`). Recovery entry is no longer an automatic
green: deny / incomplete / blocked recoveries keep the trap armed and Moving
admit closed.

ACs:
  AC1  the recovery body performs no sticky clear before the densify retry:
       the first `clear_moving_incomplete_remap_sticky_densify_off_reason`
       occurrence in the recovery window follows
       `arena_group_->compact_all_moving_pinned()` (no pre-clear window).
  AC2  the recovery retry gates on the feature flag (sticky does not hide it
       from the authorized entrant) and the comment-stripped recovery body no
       longer consults the sticky-gated `moving_compact_enabled()`, while
       arena.ixx keeps `moving_compact_enabled()` sticky-gated for agents /
       auto-arm.
  AC3  the LCP-deny path keeps the fail-closed contract with no clear→re-arm
       cycle: no `g_moving_incomplete_remap_sticky_densify_off.exchange` in
       the whole TU, the deny branch keeps `out.sticky_cleared = false`, and
       the blocked publish (had=true, pin=false, objects_moved=0) remains.
  AC4  sticky clears only after a unified-green publish: the recovery window
       gates the recovery-reason clear on `compute_moving_unified_success`
       and resolves `sticky_cleared` post-publish (true only when the trap
       actually lifted).
  AC5  tests cite #4143 (runtime ACs in test_moving_densify_fail_closed.cpp,
       wired into the dispatched runner); no tests/core/test_issue_4143.cpp;
       no docs/design/4143-*; build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MUT = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
ARENA = ROOT / "src" / "core" / "arena.ixx"
HEALTH = ROOT / "src" / "core" / "moving_densify_health.hh"
TST = ROOT / "tests" / "core" / "test_moving_densify_fail_closed.cpp"

FN = "Evaluator::recover_moving_sticky_densify_off"
CLEAR = "clear_moving_incomplete_remap_sticky_densify_off_reason"
EXCHANGE = "g_moving_incomplete_remap_sticky_densify_off.exchange"
COMPACT = "arena_group_->compact_all_moving_pinned()"


def _strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        out.append(line[:cut] if cut >= 0 else line)
    return "\n".join(out)


def main() -> int:
    mut = MUT.read_text() if MUT.exists() else ""
    arena = ARENA.read_text() if ARENA.exists() else ""
    HEALTH.read_text() if HEALTH.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    fn = mut.find(FN)
    # 8500 chars covers the full recovery body through the post-publish
    # sticky_cleared resolution and the success computation.
    win = mut[fn : fn + 8500] if fn >= 0 else ""
    code_only = _strip_comments(win)

    compact = win.find(COMPACT)
    clear = win.find(CLEAR)
    good = fn >= 0 and compact >= 0 and clear >= 0 and compact < clear
    report(
        "AC1",
        good,
        "first sticky clear in the recovery body follows the retry compact (no pre-clear window)",
    )

    good = (
        "moving_compact_feature_enabled()" in code_only
        and "moving_compact_enabled()" not in code_only
        and "g_moving_incomplete_remap_sticky_densify_off.load" in arena
        and "return moving_compact_feature_enabled();" in arena
    )
    report(
        "AC2",
        good,
        "recovery retry gates on the feature flag; agents/auto-arm keep the sticky-gated enabled()",
    )

    deny = win.find("if (densify_entry_lcp_blocked)")
    deny_body = win[deny : win.find("} else {", deny)] if deny >= 0 else ""
    good = (
        EXCHANGE not in mut
        and deny >= 0
        and "out.sticky_cleared = false;" in deny_body
        and "/*pin_contract_held=*/false" in deny_body
        and "/*objects_moved=*/0" in deny_body
    )
    report(
        "AC3",
        good,
        "LCP-deny keeps blocked publish + sticky_cleared=false with no clear→re-arm exchange",
    )

    tail = win.find("sticky_cleared = !aura::ast::moving_incomplete_remap_sticky_densify_off()")
    good = (
        "compute_moving_unified_success" in win
        and "kStickyClearRecovery" in win
        and compact >= 0
        and clear >= 0
        and compact < clear
        and tail >= 0
        and tail > compact
    )
    report(
        "AC4",
        good,
        "recovery-reason clear is unified-green gated and sticky_cleared resolves post-publish",
    )

    no_doc_4287 = not any((ROOT / "docs" / "design").glob("4287-*")) if (ROOT / "docs" / "design").exists() else True
    good = (
        "Issue #4287" in win
        and "const bool pin_for_publish = compact_r.pin_contract_held && !blocked;" in code_only
        and "/*had_moving_densify=*/true, pin_for_publish," in code_only
        and "/*pin_contract_held=*/false" in deny_body
        and "4287: blocked publish closes would_allow" in tst
        and not (ROOT / "tests" / "core" / "test_issue_4287.cpp").exists()
        and no_doc_4287
    )
    report(
        "AC6",
        good,
        "recovery retry publish folds blocked into pin (#4287); LCP-deny pin=false stays",
    )

    no_doc = not any((ROOT / "docs" / "design").glob("4143-*")) if (ROOT / "docs" / "design").exists() else True
    good = (
        "ac4143_1_deny_recover_never_drops_sticky" in tst
        and "ac4143_2_green_retry_clears_post_publish" in tst
        and "ac4143_3_source_cite_clear_after_retry" in tst
        and "ac4143_4_wiring_no_invent" in tst
        and tst.count("ac4143_1_deny_recover_never_drops_sticky();") == 1
        and not (ROOT / "tests" / "core" / "test_issue_4143.cpp").exists()
        and no_doc
        and "check_moving_sticky_recovery_4143.py" in build
        and "check_moving_sticky_recovery_4143.py" in allow
    )
    report("AC5", good, "tests cite #4143 and are dispatched; no issue-file/doc; build.py + allowlist wired")

    print(f"check_moving_sticky_recovery_4143: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
