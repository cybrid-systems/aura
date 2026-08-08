#!/usr/bin/env python3
"""Issue #2766: require-before-export free-var capture of module-private cells.

When (require …) appeared textually before (export …), multi-define Begin
treated require/export as body expressions (define_after_expr), forcing
sequential eval. Export filtering then stripped private cells that closures
free-ref → unbound-at-call (*cell*, agent-register, std/orchestrator).

Fix: treat export/require/import as module prologue (do not set
define_after_expr); run prologue Phase 0 before letrec multi-define so
require injects before lambda free-var capture.

Contract (one row per AC):
  AC1 is_module_prologue + define_after_expr skip for require/export
  AC2 Phase 0 prologue before letrec cell pre-allocate / lambda install
  AC3 Phase 3 skips already-run prologue forms
  AC4 ac2766_* tests in test_module_require_freevar.cpp
  AC5 this linter wired in build.py; no docs/design/*

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

    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    t = _read("tests/compiler/test_module_require_freevar.cpp")
    build = _read("build.py")

    # AC1 — prologue skip.
    must("#2766", "AC1", efl)
    must("is_module_prologue", "AC1", efl)
    must("NodeTag::Export", "AC1", efl)
    must("require", "AC1", efl)
    must("import", "AC1", efl)
    must("saw_non_define", "AC1", efl)

    # AC2 — Phase 0 before letrec + module pool for dual-path bind.
    must("Phase 0", "AC2", efl)
    must("is_module_prologue", "AC2", efl)
    # Phase 0 loop must precede pre-allocate cells comment/block.
    p0 = efl.find("Phase 0")
    p1 = efl.find("pre-allocate cells for all defines")
    if p0 < 0 or p1 < 0 or p0 > p1:
        fails.append("AC2: Phase 0 must appear before pre-allocate cells")
    must("#2766", "AC2", loader)
    must("set_pool", "AC2", loader)

    # AC3 — phase 3 skips prologue.
    must("Phase 0 already ran", "AC3", efl)

    # AC4 — tests.
    must("ac2766_1_require_before_export_private_cell", "AC4", t)
    must("ac2766_2_export_before_require_parity", "AC4", t)
    must("ac2766_3_orchestrator_agent_registry", "AC4", t)
    must("ac2766_4_source_cite", "AC4", t)
    must("ac2766_5_linter", "AC4", t)
    must("bad-order", "AC4", t)
    must("agent:spawn", "AC4", t)

    # AC5 — linter wire + no docs.
    must("check_module_require_export_order_2766", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2766.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2766.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2766-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2766 require-before-export free-var capture — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
