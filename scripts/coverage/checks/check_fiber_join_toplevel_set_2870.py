#!/usr/bin/env python3
"""Issue #2870: top-level free-var set! from named-let / after fiber:join.

lookup_cell_index / lookup_cell_ptr only consulted live top_ when
parent_id_==0. Named-let/letrec materialize with parent_id_ = let frame
(non-zero); free-var set! of a top-level define was unbound while Variable
read still worked. Unify residual: join then fanout set! spawn-count.

Contract:
  AC1 lookup_cell_index consults live top_ after SoA miss (any parent_id)
  AC2 lookup_cell_index / ptr consult live top_ when walk reaches cur==0
  AC3 suite fiber_join_toplevel_set_2870.aura
  AC4 linter wired in build.py; no docs/design/*

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

    env = _read("src/compiler/evaluator_env.cpp")
    suite = _read("tests/suite/fiber_join_toplevel_set_2870.aura")
    build = _read("build.py")

    # AC1 — live top_ free-var cells after parent miss (not only parent_id_==0)
    must("#2870", "AC1", env)
    must("live top_ free-var cells after parent miss", "AC1", env)
    must("Fall through to parent_ walk + live top_", "AC1", env)
    must("std::optional<std::uint64_t> Env::lookup_cell_index", "AC1", env)

    # AC2 — parent_ before live top_ (let* shadows top_)
    must("Prefer parent_ above so", "AC2", env)
    must("let* locals shadow", "AC2", env)
    # ptr path also falls through
    must("let* free-var cells may live only on the parent_ Env chain", "AC2", env)

    # AC3 — suite
    must("2870", "AC3", suite)
    must("spawn-count", "AC3", suite)
    must("named-let", "AC3", suite)
    must("OK-2870", "AC3", suite)

    # AC4 — wire + no docs
    must("check_fiber_join_toplevel_set_2870", "AC4", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2870.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_2870.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2870-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2870 fiber join toplevel set! — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
