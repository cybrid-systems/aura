#!/usr/bin/env python3
"""Issue #3469: last_object_remap_ folds previous-window keys.

#3421 last-window refuse is closed. Two Moving windows wiped A after
A→B then B→C, so apply_closure proceeded with densify-old flat==A.
Fold previous keys into the same map (not a second registry).

Contract:
  AC1 relocate saves prev_remap and folds keys that are not live
  AC2 apply_closure helper still refuses on resolve hit (no new predicate)
  AC3 Soft / objects_moved==0 keep #2569 recover (helper early-outs)
  AC4 no g_3469_* / no new query key / no second pin registry
  AC5 tests in test_moving_compact + test_setcode_rebind_survive;
      no test_issue_3469.cpp / no docs/design/3469-*

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    arena = _read("src/core/arena.ixx")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    moving = _read("tests/core/test_moving_compact.cpp")
    survive = _read("tests/compiler/test_setcode_rebind_survive.cpp")

    pos = arena.find("relocate_tracked_objects_for_moving_")
    end = arena.find("void note_dtor_entry_", pos) if pos >= 0 else -1
    if pos < 0:
        fails.append("AC1: relocate_tracked_objects_for_moving_ missing")
        win = ""
    else:
        win = arena[pos:end] if end > pos else arena[pos : pos + 16000]
    must("Issue #3469", "AC1 cite", win)
    must("prev_remap", "AC1 save previous map", win)
    must("live_new", "AC1 skip live identities", win)
    must_not("class DensifyClosurePinRegistry", "AC4 no second registry", arena)

    helper = flat.find("static bool production_apply_closure_densify_hard_refuse")
    helper_end = flat.find("static void note_apply_closure_densify_hard_refuse", helper)
    helper_win = flat[helper:helper_end] if helper >= 0 and helper_end > helper else flat[helper : helper + 800]
    must("resolve_object_remap", "AC2 refuse still resolve-hit", helper_win)
    must("g_last_objects_moved", "AC3 last-window moved load", helper_win)
    if "LifetimePin::pin" in helper_win or ".pin(" in helper_win:
        fails.append("AC3: extra pin walk on densify-refuse helper")

    must("3469: A still a remap key after window 2", "AC5 two-window", moving)
    must("ac6_3469_two_window_stale_flat_refuse", "AC5 apply test", survive)
    must("3469: apply_closure hard-refuses stale A", "AC5 apply refuse", survive)

    if "g_3469_" in arena or "g_3469_" in flat:
        fails.append("AC4: invented g_3469_* counter")
    if "schema-3469" in arena or "schema-3469" in flat:
        fails.append("AC4: new schema-3469 query key")

    if (ROOT / "tests" / "compiler" / "test_issue_3469.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3469.cpp")
    if (ROOT / "tests" / "core" / "test_issue_3469.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3469.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3469-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3469 apply_closure_densify_multi_window:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3469 apply_closure_densify_multi_window: fold previous keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
