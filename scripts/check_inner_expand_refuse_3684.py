#!/usr/bin/env python3
"""Issue #3684 source-cite gate: no half-expanded MacroIntroduced tree.

Production expand left two bypasses that committed a spliced
MacroIntroduced tree: (1) expand_inner_macros spliced a cloned body even
when the recursive inner expand hit a deny (depth ceiling) and eval_flat
evaluated it; (2) inner_expand_production_limit_deny matched only
2/3/6/7/1, so a later pass refusing via 8/9/10 (same-flat / name-map /
concurrent-top-level) kept the pass-0 tree instead of original_root.

ACs:
  AC1  deny helper ORs 8/9/10 (ConcurrentCloneGuard refuse codes).
  AC2  expand_inner_macros refuses to splice a half-expanded clone
       (try_restore + return root before the parent set_child).
  AC3  eval_flat hygienic path refuses to eval a half-expanded body
       (deny consult right after expand_inner_macros).
  AC4  Soft/Off: production_surface / is_sandbox_active gates kept
       (historical half-expand contract unchanged).
  AC5  tests extended (limits ac3684 + closed-loop ac3684); no
       test_issue_3684.cpp; no docs/design/3684-*; no new query key.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ME = ROOT / "src" / "compiler" / "macro_expansion.cpp"
IXX = ROOT / "src" / "compiler" / "macro_expansion.ixx"
FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
LIM = ROOT / "tests" / "compiler" / "test_macro_hygiene_limits.cpp"
CLOSED = ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp"


def main() -> int:
    me = ME.read_text() if ME.exists() else ""
    IXX.read_text() if IXX.exists() else ""
    flat = FLAT.read_text() if FLAT.exists() else ""
    lim = LIM.read_text() if LIM.exists() else ""
    closed = CLOSED.read_text() if CLOSED.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    good = (
        "kHygieneLimitReasonSameFlatReject" in me
        and "kHygieneLimitReasonNameMapShared" in me
        and "kHygieneLimitReasonConcurrentTopLevel" in me
        and "Issue #3684" in me
    )
    report("AC1", good, "deny helper ORs 8/9/10 (+ cites #3684)")

    good = "half-expanded clone" in me and "aura_evaluator_try_restore_macro_expand_checkpoint" in me
    report("AC2", good, "clone-path splice refused (try_restore + return root)")

    good = (
        "half-expanded body" in flat
        and "macro_exp::inner_expand_production_limit_deny()" in flat
        and "aura::core::sandbox::is_sandbox_active()" in flat
    )
    report("AC3", good, "eval_flat skips eval on deny (production-gated)")

    good = "Soft/Off keeps the splice (contract)" in me and "Soft/Off keeps the historical" in flat
    report("AC4", good, "Soft/Off half-expand contract unchanged")

    good = (
        "ac3684_deny_codes" in lim
        and "ac3684_inner_depth_restore" in lim
        and "ac3684_inner_expand_refuse" in closed
        and not (ROOT / "tests" / "compiler" / "test_issue_3684.cpp").exists()
        and not (ROOT / "docs" / "design" / "3684-inner-expand-refuse.md").exists()
        and "query:macro-hygiene-provenance-stats"
        in (ROOT / "src" / "compiler" / "evaluator_primitives_obs_jit.cpp").read_text()
    )
    report("AC5", good, "tests extended; no new artifacts; query key unchanged")

    print("Issue #3684 inner expand refuse linter: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
