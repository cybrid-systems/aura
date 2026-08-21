#!/usr/bin/env python3
"""Issue #3221: production dirty / invalidate pass Cascade, not ResidualForceHeal.

Every production mark_define_dirty / invalidate_function used to look
like a residual-force heal to decide_and_reemit. That skews cascade vs
residual coverage and #3096 auto-heal.

Contract (one row per AC):
  AC1  mark_define_dirty / invalidate_function pass ReemitReason::Cascade;
       those function windows do not contain ResidualForceHeal
  AC2  observe_residual_force_stale auto-heal uses ResidualForceHeal;
       maybe_coverage_verify default remains CoverageVerify
  AC3  dual-track / #3112 facade ownership preserved; this linter in
       build.py; extend test_compiler_hot_update_facade; no invent /
       docs/design/3221-*; no new query:*
  AC4  last_reemit_reason hook on decide_and_reemit; Soft facade still
       returns false (zero extra)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _fn_win(src: str, sig: str) -> str:
    pos = src.find(sig)
    if pos < 0:
        return ""
    nxt = src.find("\nvoid CompilerService::", pos + 1)
    return src[pos:nxt] if nxt > pos else src[pos : pos + 16000]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    svc = _read("src/compiler/service_dirty.cpp")
    hur = _read("src/compiler/hot_update_registry.cpp")
    hh = _read("src/compiler/hot_update_registry.hh")
    t = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    build = _read("build.py")

    md = _fn_win(svc, "void CompilerService::mark_define_dirty")
    inv = _fn_win(svc, "void CompilerService::invalidate_function")
    must("ReemitReason::Cascade", "AC1 mark_define_dirty Cascade", md)
    must("Issue #3221", "AC1 mark_define_dirty cite", md)
    if "ReemitReason::ResidualForceHeal" in md:
        fails.append("AC1: mark_define_dirty still passes ResidualForceHeal")
    must("ReemitReason::Cascade", "AC1 invalidate_function Cascade", inv)
    must("Issue #3221", "AC1 invalidate_function cite", inv)
    if "ReemitReason::ResidualForceHeal" in inv:
        fails.append("AC1: invalidate_function still passes ResidualForceHeal")

    must("ReemitReason::ResidualForceHeal", "AC2 auto-heal reason", hur)
    must("maybe_coverage_verify_min_dirty(ReemitReason::ResidualForceHeal)", "AC2 #3096 call", hur)
    must("ReemitReason::CoverageVerify", "AC2 CoverageVerify default", hh)
    must("last_reemit_reason", "AC4 hook", hh)
    must("last_reemit_reason_.store", "AC4 decide stores reason", hur)

    must("hard_invalidate_via_facade", "AC3 facade still used", md)
    must("g_dual_track_bypass_total", "AC3 dual-track counter", md)
    must("ac3221_cascade_reason_not_residual_force_heal", "AC3 test fn", t)
    must("check_cascade_reason_not_residual_force_heal_3221", "AC3 build.py", build)
    if "query:last-reemit-reason" in hur or "query:cascade-reason" in hur:
        fails.append("AC3: new query:* name (reuse last_reemit_reason hook)")

    if (ROOT / "tests" / "compiler" / "test_issue_3221.cpp").is_file():
        fails.append("AC3: tests/compiler/test_issue_3221.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3221.cpp").is_file():
        fails.append("AC3: tests/issues/test_issue_3221.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3221-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3221 cascade_reason_not_residual_force_heal:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3221 cascade_reason_not_residual_force_heal: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
