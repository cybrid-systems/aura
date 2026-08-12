#!/usr/bin/env python3
"""Issue #2871: named-let true TCO in tree-walker (no early recursion cap).

TW Call/LetRec/Let/inline-lambda tails used `return eval_flat(body)` which
grew the C stack; named-let (letrec→lambda) never hit IR TCOPass and failed
at MAX_C_STACK_DEPTH (~700). Self-rec define often lowered to IR PendingCall
and appeared TCO-safe. Fix: continue the eval_flat TCO loop; pin parent Envs
when rebinding tail_env.

Contract:
  AC1 eval_flat Call TW closure path continues (true TCO) + #2871
  AC2 LetRec / Let / inline-lambda tail continue; pin_if_current_in_tail
  AC3 suite named_let_tco_2871.aura
  AC4 linter wired in build.py; no docs/design/2871-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    suite = _read("tests/suite/named_let_tco_2871.aura")
    build = _read("build.py")

    # AC1 — TW closure Call continues (not return eval_flat body alone)
    must("#2871", "AC1", ev)
    must("true TCO for TW closure calls", "AC1", ev)
    must("current_id = cl.body_id", "AC1", ev)
    must("pin_if_current_in_tail", "AC1", ev)

    # AC2 — LetRec / Let / inline-lambda
    must("TCO into body — named-let desugars", "AC2", ev)
    must("TCO into let body", "AC2", ev)
    must("true TCO — stay in this eval_flat frame", "AC2", ev)
    must("tco_pinned", "AC2", ev)

    # AC3 — suite
    must("2871", "AC3", suite)
    must("named-let", "AC3", suite)
    must("OK-2871", "AC3", suite)
    must("deep 2000", "AC3", suite)

    # AC4 — wire + no docs
    must("check_named_let_tco_2871", "AC4", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2871.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_2871.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2871-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2871 named-let TCO — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
