#!/usr/bin/env python3
"""Issue #3470: expand_inner_macros qq-unwrap + depth-limit refuse-partial.

#3062 closed top-level macro_expand_all half-tree. Inner expander still
committed unwrap set_child before recurse and stamped PassLimit on depth.

Contract:
  AC1 production rolls back unwrap set_child on depth/pass/steal/cap deny
  AC2 Soft/Off keeps historical half-write (no extra Soft checkpoint)
  AC3 inner depth ceiling stamps DepthLimit (2), not PassLimit (3)
  AC4 no new query key / metric; reuse last_limit_reason + expand ckpt ABI
  AC5 extend qq-unwrap + hygiene_limits; no test_issue_3470.cpp

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

    me = _read("src/compiler/macro_expansion.cpp")
    qq = _read("tests/compiler/test_qq_unwrap_targeted_restamp.cpp")
    lim = _read("tests/compiler/test_macro_hygiene_limits.cpp")

    eim = me.find("aura::ast::NodeId expand_inner_macros")
    eim_win = me[eim : eim + 4200] if eim >= 0 else ""
    must("Issue #3470", "AC1 cite", eim_win)
    must("kHygieneLimitReasonDepthLimit", "AC3 DepthLimit", eim_win)
    must_not(
        "note_hygiene_last_limit_reason(kHygieneLimitReasonPassLimit)",
        "AC3 not PassLimit on inner depth",
        eim_win,
    )
    must("inner_expand_production_limit_deny", "AC1 deny helper", eim_win)
    must("set_child(parent_id, unwrap_ci, root)", "AC1 belt restore", eim_win)
    must("aura_evaluator_try_restore_macro_expand_checkpoint", "AC1 reuse ckpt", eim_win)
    must("restamp_after_qq_unwrap", "AC1 #2809 restamp kept", eim_win)
    must("is_sandbox_active", "AC2 production gate", eim_win)
    must("Soft/Off", "AC2 Soft contract", eim_win)

    # No second ExpandCheckpointGuard in inner (clone/expand_all own it).
    if eim_win.count("struct ExpandCheckpointGuard") != 0:
        fails.append("AC1: inner must not install a second ExpandCheckpointGuard")

    must("3470: production returns pre-unwrap NodeId", "AC5 qq restore", qq)
    must("3470: reason hygiene-depth-limit", "AC5 depth string", qq)
    must("3470: Soft/Off keeps unwrap (historical half-write)", "AC5 Soft", qq)
    must("3470: inner stamps DepthLimit", "AC5 limits", lim)

    if "g_3470_" in me:
        fails.append("AC4: invented g_3470_* atomic")
    if "schema-3470" in me:
        fails.append("AC4: new schema-3470 query key")

    if (ROOT / "tests" / "compiler" / "test_issue_3470.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3470.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3470-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3470 inner_expand_unwrap_depth:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3470 inner_expand_unwrap_depth: DepthLimit + unwrap restore")
    return 0


if __name__ == "__main__":
    sys.exit(main())
