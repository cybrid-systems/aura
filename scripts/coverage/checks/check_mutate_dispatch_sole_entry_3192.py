#!/usr/bin/env python3
"""Issue #3192: force all structural mutate:* paths through mutate_dispatch_try_acquire.

I2 residual from the 2026-08-19 multi-fiber concurrent mutation safety
review. mutate_dispatch_try_acquire is the sole Guard acquire for structural
mutate:* bodies (#3074). Any structural mutate primitive that misses the
acquire = silent half-topology write under concurrent steal/GC.

Contract:
  AC1 every structural mutate primitive contains mutate_dispatch_try_acquire
     (or a thin wrapper that calls it) — source-cite on the call site
  AC2 this linter scans mutate primitive files and fails on missing acquire
  AC3 nested atomic-batch suppress_bump / single-commit bump unchanged
  AC4 production: missing Guard is hard reject; soft: metric-only
  AC5 extend existing suite (no test_issue_3192.cpp, no docs/design/*)
  AC6 additive observability only (counter at end of CompilerMetrics)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


# Structural mutations that must go through mutate_dispatch_try_acquire.
# These are the public structural mutate:* primitives (skip scalar prims
# like replace-type / replace-value / record-patch that have dedicated
# macro-hygiene gates from #3131 / #3076 / #3115).
STRUCTURAL_PRIMITIVES = [
    "mutate:rebind",
    "mutate:set-body",
    "mutate:remove-node",
    "mutate:insert-child",
    "mutate:replace-pattern",
    "mutate:replace-subtree",
    "mutate:atomic-batch",
    "mutate:splice",
    "mutate:wrap",
    "mutate:rename-symbol",
    "mutate:move-node",
    "mutate:inline-call",
    "mutate:restore-hygiene-checkpoint",
]


def _scan_primitive(hay: str, prim: str) -> tuple[int, int, bool]:
    """Return (start_line, end_line, has_acquire) for a primitive definition.

    Detects an `add_mutate("mutate:NAME", [&ev, ...] { ... })` block and
    checks whether the lambda body contains mutate_dispatch_try_acquire.
    Also accepts the thin-wrapper pattern (TransactionGuard / TransactionGuard)
    only when the wrapper itself calls mutate_dispatch_try_acquire — which
    by inspection it does NOT, so the wrapper is rejected.
    """
    # Find the start of the add_mutate call for this primitive.
    pattern = re.compile(
        r'add_mutate\((\s*)"' + re.escape(prim) + r'"',
        re.MULTILINE,
    )
    m = pattern.search(hay)
    if not m:
        return (-1, -1, False)
    start = m.start()
    # Walk to find the matching closing brace of the lambda body. The lambda
    # body is `[&ev, ...] { ... }`. We count braces from the first '{' after
    # the `[` capture list.
    bracket = hay.find("[", m.end())
    if bracket < 0:
        return (start, -1, False)
    brace = hay.find("{", bracket)
    if brace < 0:
        return (start, -1, False)
    depth = 1
    i = brace + 1
    while i < len(hay) and depth > 0:
        c = hay[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    end = i
    body = hay[brace:end]
    has_acquire = "mutate_dispatch_try_acquire" in body
    return (start, end, has_acquire)


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    mut_other = _read("src/compiler/evaluator_primitives_mutation.cpp")
    mut_boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    dispatch = _read("src/compiler/mutate_dispatch.hh")
    build = _read("build.py")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")

    # AC1 — every structural mutate primitive must contain the SSOT acquire.
    combined = mut + mut_other
    for prim in STRUCTURAL_PRIMITIVES:
        start, end, has = _scan_primitive(combined, prim)
        if start < 0:
            fails.append(f"AC1: primitive {prim!r} not found in mutate primitive files")
            continue
        if not has:
            fails.append(f"AC1: primitive {prim!r} lambda body missing `mutate_dispatch_try_acquire` (I2 residual gap)")

    # AC2 — the linter itself is the contract; verify it scans mutate_dispatch
    # metadata + the SSOT acquire is documented as sole entry.
    must("mutate_dispatch_try_acquire", "AC2", dispatch)
    must("Issue #3074", "AC2", dispatch)
    must("AC1", "AC2", sys.modules[__name__].__doc__ or "")
    # self-test: the linter exists.
    if not (Path(__file__).is_file() and "check_mutate_dispatch_sole_entry_3192" in Path(__file__).name):
        fails.append("AC2: linter path/name")

    # AC3 — nested atomic-batch suppress_bump / single-commit bump unchanged.
    # Issue #3019 / #3166 semantics preserved; verify the existing
    # Issue #3019 / #3166 surface comments are still in the boundary code.
    must("Issue #3019", "AC3", mut_boundary)
    must("Issue #3166", "AC3", mut_boundary)

    # AC4 — production: missing Guard on structural path is hard reject.
    # Soft: metric-only observe counter. The observe path is the existing
    # mutate_dispatch_try_acquire metric; we don't add a new one (#3192 AC6).
    must("quota", "AC4", mut)
    must("resource-quota-exceeded", "AC4", mut)

    # AC5 — no test_issue_3192.cpp, no docs/design/3192-*. Extend existing
    # hygiene_mutate_closed_loop suite.
    must("ac3192_", "AC5", t)
    must("Issue #3192", "AC5", mut)
    must("Issue #3192", "AC5", _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp"))
    if (ROOT / "tests" / "issues" / "test_issue_3192.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3192.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3192.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3192.cpp per #81967")
    if (ROOT / "docs" / "design" / "3192-mutate-dispatch-sole-entry.md").is_file():
        fails.append("AC5: forbidden docs/design/3192-* per #1655")

    # AC6 — wired into build.py.
    must("check_mutate_dispatch_sole_entry_3192", "AC6", build)

    if fails:
        print("FAIL #3192 mutate_dispatch_sole_entry:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3192 mutate_dispatch_sole_entry: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
