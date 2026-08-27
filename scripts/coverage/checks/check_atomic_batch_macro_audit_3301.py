#!/usr/bin/env python3
"""Issue #3301: atomic-batch batch-level MacroIntroduced fail-closed audit.

Production residual from Macro + Hygiene + Self-Evo review (2026-08-23):
`mutate:atomic-batch` relied on each lockless sub-op carrying its own
hygiene gate; the dispatcher had no independent batch-level
MacroIntroduced audit. A future helper appended to
kAtomicBatchLocklessOps without its own gate inherited a default-deny
hole under production defaults (Restricted + Strict).

Contract (one row per AC):
  AC1  Batch-entry target walk: dispatcher checks each sub-op's target
       node-id arg (table target_arg metadata) BEFORE the sub-op loop;
       MacroIntroduced target + no opt-out => whole-batch deny
  AC2  Batch-form `:allow-macro? #t` keyword + per-sub-op
       :allow-macro? (#3213) + global allow all opt out
  AC3  Deny path stamps kHygieneLimitReasonMacroIntroduced
       (hygiene_last_limit_reason_string == "hygiene-macro-introduced")
       + typed audit + bumps atomic_batch hygiene-violations counter
  AC4  Soft/Off zero-cost: the walk is gated to production sandbox
       (is_sandbox_active || effect_sandbox_mode != 0); per-op gates
       keep Soft semantics (no change to Soft/Off contract)
  AC5  Lockless :rebind gets its own hygiene gate (name-based parity —
       the batch walk cannot see its define); new rebind_hygiene_reject
       counter on FlatAST
  AC6  query:atomic-batch-stats-hash exposes hygiene-violations +
       schema-3301; no invent test_issue_3301.cpp; no docs/design/3301-*

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    ast = _read("src/core/ast.ixx")
    ev = _read("src/compiler/evaluator.ixx")
    mutq = _read("src/compiler/evaluator_primitives_mutation.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")
    lint3213 = _read("scripts/coverage/checks/check_atomic_batch_allow_macro_3213.py")

    # ── AC1 batch-entry target walk ────────────────────────────────────
    must("Issue #3301", "AC1 cite", mut)
    must("target_arg", "AC1 table target-arg metadata", mut)
    must("is_macro_introduced(node)", "AC1 batch walk checks marker", mut)
    must("3301 AC1", "AC1 test marker", test)

    # ── AC2 opt-out paths ──────────────────────────────────────────────
    must(":allow-macro?", "AC2 batch-form keyword", mut)
    must("batch_allow_macro", "AC2 batch-form flag", mut)
    must("parse_allow_macro_opt_out(ev, op_args)", "AC2 per-sub-op opt-out", mut)
    must("get_allow_macro_mutate()", "AC2 global allow", mut)
    must("3301 AC2", "AC2 test marker", test)

    # ── AC3 reason stamp + counters ────────────────────────────────────
    must("kHygieneLimitReasonMacroIntroduced", "AC3 reason stamp", mut)
    must("capture_macro_hygiene_audit", "AC3 typed audit trail", mut)
    must("bump_atomic_batch_hygiene_violation", "AC3 batch hygiene counter", mut)
    must("3301 AC3", "AC3 test marker", test)

    # ── AC4 Soft/Off production gate ───────────────────────────────────
    must("is_sandbox_active()", "AC4 production gate", mut)
    must("effect_sandbox_mode() != 0", "AC4 production gate 2", mut)
    must("3301 AC4", "AC4 test marker", test)

    # ── AC5 lockless rebind parity gate ────────────────────────────────
    must("batch :rebind: cannot rebind MacroIntroduced define", "AC5 rebind gate", efl)
    must("note_rebind_hygiene_reject", "AC5 rebind reject counter call", efl)
    must("rebind_hygiene_reject_total", "AC5 counter decl", ast)
    must("3301 AC5", "AC5 test marker", test)

    # ── AC6 observability + lineage + no-invent ────────────────────────
    must('insert_kv("hygiene-violations"', "AC6 stats-hash key", mutq)
    must('insert_kv("schema-3301"', "AC6 schema-3301", mutq)
    must("atomic_batch_hygiene_violations_total", "AC6 getter", ev)
    must("check_atomic_batch_macro_audit_3301", "AC6 build.py wiring", build)
    must("check_atomic_batch_allow_macro_3213", "AC6 3213 linter still wired", build)
    must("3213", "AC6 3213 lineage", lint3213)
    must("3301 AC6", "AC6 test marker", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3301.cpp").is_file():
        fails.append("AC6: test_issue_3301.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3301.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3301.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3301-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3301 atomic-batch batch-level macro audit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
