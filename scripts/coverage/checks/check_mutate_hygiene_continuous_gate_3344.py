#!/usr/bin/env python3
"""Issue #3344: continuous hygiene gate on mutate:* / lockless helpers.

Closed-loop coverage (#3191 and siblings) already gates today's structural
set. Residual is continuous: any future lockless / scalar / sv-* / refactor
prim that bypasses reject_structural_macro_hygiene /
enforce_macro_hygiene_mutate_hotpath / hygiene_protected_error re-opens the
MacroIntroduced default-deny hole.

This linter DISCOVERS every add_mutate("mutate:…") and every
eval_flat_apply_mutate_* — it does not keep a stale allowlist. A new prim
without a hygiene call (or a documented HYGIENE_EXEMPT / GUARD_EXEMPT
reason) fails the gate.

Contract:
  AC1  every add_mutate mutate:* body hits a hygiene helper, or carries
       HYGIENE_EXEMPT / GUARD_EXEMPT with reason
  AC2  every eval_flat_apply_mutate_* hits a hygiene helper (or lockless
       is_macro_introduced + record_hygiene_violation_attempt) and is
       listed in kAtomicBatchLocklessOps
  AC3  test_hygiene_mutate_closed_loop names every non-exempt mutate:*
       (golden list for MacroIntroduced default-deny / :allow-macro?)
  AC4  Soft / Off: helpers still short-circuit on !is_macro_introduced
       (zero extra parse on non-macro)
  AC5  extend test_hygiene_mutate_closed_loop; linter AFTER #3191; no
       test_issue_3344.cpp; no docs/design/; no schema-3344 / g_3344_*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

HYG_HELPERS = (
    "reject_structural_macro_hygiene",
    "enforce_macro_hygiene_mutate_hotpath",
    "hygiene_protected_error",
    "record_hygiene_violation_attempt",
)

ADD_MUTATE_RE = re.compile(r'add_mutate\(\s*"([^"]+)"', re.MULTILINE)
FLAT_FN_RE = re.compile(r"EvalResult Evaluator::(eval_flat_apply_mutate_\w+)\(")
LOCKLESS_ROW_RE = re.compile(r'\{\s*"mutate:([^"]+)"\s*,\s*&Evaluator::eval_flat_apply_mutate_(\w+)')


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _lambda_body(hay: str, after: int) -> str:
    bracket = hay.find("[", after)
    if bracket < 0:
        return ""
    brace = hay.find("{", bracket)
    if brace < 0:
        return ""
    depth = 1
    i = brace + 1
    while i < len(hay) and depth > 0:
        c = hay[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return hay[brace:i]


def _has_hygiene(body: str) -> bool:
    return any(h in body for h in HYG_HELPERS)


def _is_exempt(pre: str) -> bool:
    return "HYGIENE_EXEMPT" in pre or "GUARD_EXEMPT" in pre


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    # AC1 — discover every add_mutate mutate:* .
    names: list[str] = []
    for m in ADD_MUTATE_RE.finditer(mut):
        name = m.group(1)
        names.append(name)
        pre = mut[max(0, m.start() - 900) : m.start()]
        body = _lambda_body(mut, m.end())
        if _has_hygiene(body):
            continue
        if _is_exempt(pre):
            continue
        fails.append(
            f"AC1: {name} missing hygiene helper "
            "(reject_structural_macro_hygiene / enforce_macro_hygiene_mutate_hotpath / "
            "hygiene_protected_error / record_hygiene_violation_attempt) and no "
            "HYGIENE_EXEMPT / GUARD_EXEMPT reason"
        )
    if len(names) < 10:
        fails.append(f"AC1: discovered too few add_mutate entries ({len(names)})")
    must("Issue #3344", "AC1 mutate cite", mut)
    must("check_mutate_hygiene_continuous_gate_3344", "AC1 linter name", Path(__file__).name)

    # AC2 — lockless helpers.
    flat_fns = list(FLAT_FN_RE.finditer(flat))
    table_pairs = {(f"mutate:{a}", b) for a, b in LOCKLESS_ROW_RE.findall(mut)}
    table_names = {n for n, _ in table_pairs}
    table_fns = {f"eval_flat_apply_mutate_{fn}" for _, fn in table_pairs}
    if not table_pairs:
        fails.append("AC2: kAtomicBatchLocklessOps rows not found")
    for i, m in enumerate(flat_fns):
        fn = m.group(1)
        nxt = flat_fns[i + 1].start() if i + 1 < len(flat_fns) else m.start() + 16000
        body = flat[m.start() : nxt]
        lockless_ok = "is_macro_introduced" in body and (
            "record_hygiene_violation_attempt" in body or "note_hygiene_last_limit_reason" in body
        )
        if not (_has_hygiene(body) or lockless_ok):
            fails.append(
                f"AC2: {fn} missing lockless hygiene gate (is_macro_introduced + record_hygiene_violation_attempt)"
            )
        if fn not in table_fns:
            fails.append(f"AC2: {fn} not listed in kAtomicBatchLocklessOps")
    for n, fn in sorted(table_pairs):
        full = f"eval_flat_apply_mutate_{fn}"
        if not any(m.group(1) == full for m in flat_fns):
            fails.append(f"AC2: table row {n} helper {full} missing from eval_flat")

    # AC3 — closed-loop golden list names every non-exempt mutate:*.
    for m in ADD_MUTATE_RE.finditer(mut):
        name = m.group(1)
        pre = mut[max(0, m.start() - 900) : m.start()]
        body = _lambda_body(mut, m.end())
        if _is_exempt(pre) and not _has_hygiene(body):
            continue
        if name not in test:
            fails.append(f"AC3: test_hygiene_mutate_closed_loop missing {name}")
    must("mutate:rename-symbol", "AC3 golden", test)
    must("mutate:replace-pattern", "AC3 golden", test)
    must("mutate:atomic-batch", "AC3 golden", test)

    # AC4 — Soft / Off short-circuit still present.
    must("is_macro_introduced(id)", "AC4 helper short-circuit", mut)
    must("get_allow_macro_mutate() || parse_allow_macro_opt_out", "AC4 lockless ||", flat)
    must("3344 AC4", "AC4 test", test)

    # AC5
    must("check_mutate_hygiene_continuous_gate_3344", "AC5 build.py", build)
    must("ac3344_1_discovers_mutate_entries", "AC5 test AC1", test)
    must("ac3344_5_source_and_linter", "AC5 test AC5", test)
    prev = build.find("check_macro_hygiene_default_deny_3191")
    ours = build.find("check_mutate_hygiene_continuous_gate_3344")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3191")
    if "schema-3344" in mut or "schema-3344" in flat:
        fails.append("AC5: new schema-3344 query key")
    if "g_3344_" in mut or "g_3344_" in flat:
        fails.append("AC5: new g_3344_* counter")
    if _read("tests/compiler/test_issue_3344.cpp"):
        fails.append("AC5: test_issue_3344.cpp present (forbidden #81967)")
    if _read("tests/issues/test_issue_3344.cpp"):
        fails.append("AC5: tests/issues/test_issue_3344.cpp present (forbidden #81967)")
    if _read("docs/design/3344-mutate-hygiene-continuous-gate.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3344 mutate_hygiene_continuous_gate:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(
        f"OK #3344 mutate_hygiene_continuous_gate: "
        f"{len(names)} add_mutate, {len(flat_fns)} lockless helpers, "
        f"{len(table_names)} batch-table rows"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
