#!/usr/bin/env python3
"""Issue #3321: ConcurrentCloneGuard production zero half-tree + stable reason.

Residual after #3303: 16-slot same-flat / name-map reject was counter-only
(Agent saw last_reject_reason 2/4 overlapping depth-limit / macro-introduced).
Nested steal kept cloning siblings into a half-tree until function-exit
restore. Production must restore + stamp a stable hygiene_last_limit_reason
string; Soft/Off may keep historical half-write (zero extra on quiet path).

Contract:
  AC1 steal mid top-level → try_restore + StealAbort reason
  AC2 nested steal under claimed top-level → production fail-fast sibling abort
  AC3 Soft/Off: no extra claim / no hard restore mandate on quiet path
  AC4 16-slot same-flat reject stamps last_limit_reason code 8 (string);
      last_reject_reason 2/4 preserved
  AC5 extend test_concurrent_clone_steal_abort_visibility; no invent / docs
  AC6 cross-FlatAST intern(resolve) — no raw ID capture

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

    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    test = _read("tests/compiler/test_concurrent_clone_steal_abort_visibility.cpp")
    build = _read("build.py")

    must("kConcurrentCloneProdZeroHalfTreeIssue = 3321", "AC1 stamp", ixx)
    must("expand_ckpt.try_restore()", "AC1 restore", me)
    must("note_hygiene_last_limit_reason(kHygieneLimitReasonStealAbort)", "AC1 steal reason", me)
    must("#3321 AC1", "AC1 test", test)

    must("Issue #3321: production fail-fast after nested steal-abort", "AC2 fail-fast", me)
    must("cloned == NULL_NODE && production_surface", "AC2 sibling abort", me)
    must("#3321 AC2", "AC2 test", test)

    must("Soft/Off: continue (historical", "AC3 soft", me)
    must("hygiene_depth == 0 && production_surface", "AC3 ckpt gate", me)
    must("#3321 AC3", "AC3 test", test)

    must("kHygieneLimitReasonSameFlatReject = 8", "AC4 code 8", ixx)
    must("kHygieneLimitReasonNameMapShared = 9", "AC4 code 9", ixx)
    must("note_hygiene_last_limit_reason(kHygieneLimitReasonSameFlatReject)", "AC4 same-flat", me)
    must("note_hygiene_last_limit_reason(kHygieneLimitReasonNameMapShared)", "AC4 name-map", me)
    must('return "same-flat-clone-reject"', "AC4 string same-flat", me)
    must('return "name-map-shared"', "AC4 string name-map", me)
    must("g_macro_clone_last_reject_reason.store(2", "AC4 reason 2 kept", me)
    must("g_macro_clone_last_reject_reason.store(4", "AC4 reason 4 kept", me)
    must("#3321 AC4", "AC4 test", test)

    must("check_concurrent_clone_prod_zero_half_tree_3321", "AC5 build.py", build)
    must("#3321 AC5", "AC5 test", test)
    must("target_pool.intern(std::string(source_pool.resolve(sid)))", "AC6 intern", me)
    if "g_3321_" in me or "g_3321_" in ixx:
        fails.append("AC5: new g_3321_* counter")
    if "schema-3321" in me:
        fails.append("AC5: schema-3321")
    if (ROOT / "tests" / "compiler" / "test_issue_3321.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3321.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3321.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3321.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3321-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3321 concurrent_clone_prod_zero_half_tree:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3321 concurrent_clone_prod_zero_half_tree: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
