#!/usr/bin/env python3
"""Issue #3062: macro_expand_all pass/depth limit refuse-partial without boundary.

Production (Restricted/Strict) installs a lightweight expand checkpoint
(reuse panic-checkpoint save/restore/commit) so a limit hit never returns
a half-expanded tree. Soft/Off remains zero-cost. No new query keys.

  AC1  production no-boundary pass-limit → original_root + reason 3
  AC2  MutationBoundary still restore_macro_expand_checkpoint
  AC3  Soft/Off still half-expands (zero-cost)
  AC4  install_macro_expand_checkpoint + save/commit C ABI
  AC5  extend test_macro_hygiene_limits; no test_issue_3062.cpp; no docs/design/

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

    mx = _read("src/compiler/macro_expansion.cpp")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    br = _read("src/compiler/aura_jit_bridge.h")
    lim_t = _read("tests/compiler/test_macro_hygiene_limits.cpp")
    build = _read("build.py")

    bpos = mx.find("Issue #2023: body of macro_expand_all")
    if bpos < 0:
        bpos = mx.rfind("macro_expand_all_body")
    bwin = mx[bpos : bpos + 12000] if bpos >= 0 else ""

    # AC1 production refuse-partial
    must("Issue #3062", "AC1 cite", mx)
    must("production_surface", "AC1 gate", bwin)
    must("is_sandbox_active", "AC1 Restricted/Strict", mx)
    must("original_root", "AC1 return", bwin)
    must("g_macro_hygiene_last_limit_reason", "AC1 reason", bwin)
    must("3062 AC1", "AC1 test", lim_t)

    # AC2 boundary restore
    must("aura_evaluator_try_restore_macro_expand_checkpoint", "AC2 restore", bwin)
    must("install_macro_expand_checkpoint", "AC2 install", mx)
    must("3062 AC2", "AC2 test", lim_t)

    # AC3 Soft/Off
    must("Soft/Off", "AC3 cite", mx)
    must("3062 AC3 Soft/Off still half-expands", "AC3 test", lim_t)

    # AC4 ABI + helper
    must("aura_evaluator_try_save_macro_expand_checkpoint", "AC4 save", fib)
    must("aura_evaluator_commit_macro_expand_checkpoint", "AC4 commit", fib)
    must("save_panic_checkpoint", "AC4 reuse save", fib)
    must("commit_panic_checkpoint", "AC4 reuse commit", fib)
    must("aura_evaluator_try_save_macro_expand_checkpoint", "AC4 hdr save", br)
    must("NameMapCheckpoint", "AC4 steal/name map unchanged", mx)
    must("3062 AC4", "AC4 test", lim_t)

    # AC5 wiring
    must("check_macro_expand_noboundary_limit_3062", "AC5 build", build)
    must("cmd_macro_expand_noboundary_limit_3062", "AC5 cmd", build)
    must("3062 AC5", "AC5 test", lim_t)
    if (ROOT / "tests" / "compiler" / "test_issue_3062.cpp").is_file():
        fails.append("AC5: test_issue_3062.cpp present (forbidden per #81967)")
    if _read("docs/design/3062-macro-expand-noboundary-limit.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    if "query:macro-expand-checkpoint" in _read("src/compiler/evaluator_primitives_query_obs_mid.cpp"):
        fails.append("AC5: new top-level query key (forbidden)")

    if fails:
        print(f"Issue #3062 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3062 no-boundary expand limit refuse-partial — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
