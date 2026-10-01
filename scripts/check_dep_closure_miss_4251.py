#!/usr/bin/env python3
"""Issue #4251: dep-closure reverse-map miss is not a truncate.

reverify_clean_constraints_for_touched treated a var_to_constraints_ miss as
an empty closure node (bare `continue`, no pending_full_solve_roots_ insert,
no last_reverify_truncated_), so a later local / empty-dirty solve_delta could
return SOLVED without ever rechecking the unmapped constraint. Production
truncated-BFS escalate (#3511/#3557) never fired because a miss is not a cap
hit.

Contract (one row per AC):
  AC1 Production/Full miss takes the cap-hit fail-closed arm (pending insert +
      last_reverify_truncated_) instead of the bare observe-only continue
  AC2 Cap-hit arm unchanged (closure_cap_hit + enqueue_residual_frontier)
  AC3 Soft/Off observe-only continue preserved; #2939 empty-seed zero cost kept
  AC4 UF merge retargets pending_full_solve_roots_ r2->r1 (occurrence/let-poly
      sibling shape) so a seed never names a dead rep
  AC5 Stray 3820dbg fprintf removed; no new query key / no docs/design
  AC6 Runtime ACs ac4251_1..5 dispatched before the Results line; linter wired
      in build.py + coverage root allowlist; no test_issue_4251 tree

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _squash(text: str) -> str:
    return " ".join(text.split())


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if _squash(n) not in _squash(hay):
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if _squash(n) in _squash(hay):
            fails.append(f"{label}: forbidden {n!r}")

    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    start = impl.find("bool ConstraintSystem::reverify_clean_constraints_for_touched(")
    end = impl.find("void ConstraintSystem::add_delta(", start) if start >= 0 else -1
    if start < 0 or end < 0:
        fails.append("AC1: reverify_clean_constraints_for_touched window not found")
        win = ""
    else:
        win = impl[start:end]

    # ── AC1: fail-closed miss arm ──
    must("Issue #4251", "AC1 cite", win)
    must("a reverse-map miss is NOT an empty closure node", "AC1 comment", win)
    must("hard_miss", "AC1 hard-face probe", win)
    must("pending_full_solve_roots_.insert(root)", "AC1 pending insert", win)
    must("last_reverify_truncated_ = true", "AC1 truncate latch", win)
    must("last_reverify_unscanned_ = 1", "AC1 unscanned latch", win)
    must("production_defaults_active()", "AC1 prod face", win)
    must("AuditStrategy::Full", "AC1 full face", win)
    # The unguarded miss (no pending insert, no latch) must be gone.
    must_not(
        "auto it = var_to_constraints_.find(root); if (it == var_to_constraints_.end()) continue;",
        "AC1 bare miss continue",
        impl,
    )

    # ── AC2: cap-hit arm unchanged ──
    must("closure_cap_hit = true", "AC2 cap hit", win)
    must("enqueue_residual_frontier", "AC2 residual frontier", win)
    must("const bool truncated = closure_cap_hit;", "AC2 truncation is cap hit", win)

    # ── AC3: soft observe + #2939 empty-seed zero cost ──
    must("zero-cost observe-only continue", "AC3 soft observe doc", win)
    must("if (touched_roots_.empty() && occurrence_priority_roots_.empty() &&", "AC3 empty seed", win)
    must("return true;", "AC3 empty seed returns clean", win)
    must_not("schema-4251", "AC3 no new query key", impl)

    # ── AC4: UF merge retargets pending r2->r1 ──
    ustart = impl.find("Issue #745: preserve Occurrence-narrow priority across merges.")
    uend = impl.find("note_touched_var(TypeId{static_cast<std::uint32_t>(r1), 1});", ustart)
    if ustart < 0 or uend < 0:
        fails.append("AC4: UF merge retarget window not found")
        uwin = ""
    else:
        uwin = impl[ustart:uend]
    must("Issue #4251: a pending full-solve seed must follow the merge", "AC4 cite", uwin)
    must("pending_full_solve_roots_.count(static_cast<std::uint32_t>(r2)) > 0", "AC4 r2 probe", uwin)
    must("pending_full_solve_roots_.insert(static_cast<std::uint32_t>(r1));", "AC4 insert r1", uwin)
    must("pending_full_solve_roots_.erase(static_cast<std::uint32_t>(r2));", "AC4 erase r2", uwin)

    # ── AC5: debug probe removed / no design doc ──
    must_not("3820dbg", "AC5 debug fprintf removed", impl)
    must("pending_full_solve_root_present_for_test", "AC5 read-only probe", ixx)
    if (ROOT / "tests" / "compiler" / "test_issue_4251.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_4251.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_4251.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_4251.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4251-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # ── AC6: runtime ACs + wiring ──
    for name in (
        "ac4251_1_prod_miss_not_silent_solved",
        "ac4251_2_soft_miss_observe_only",
        "ac4251_3_uf_merge_retargets_pending",
        "ac4251_4_source_cite_and_debug_removed",
        "ac4251_5_source_and_linter",
    ):
        must(name, f"AC6 {name}", t)
    must("4251 AC1: prod miss latches truncation", "AC6 CHECK row", t)
    must("4251 AC3: pending follows the surviving rep", "AC6 merge CHECK row", t)
    must("check_dep_closure_miss_4251", "AC6 build.py wiring", build)
    must("check_dep_closure_miss_4251.py", "AC6 allowlist", allow)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4251 dep-closure reverse-map miss fail-closed + UF pending retarget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
