#!/usr/bin/env python3
"""Issue #2346: resume MutationSafetySnapshot hard-invariant coverage.

Contract:
  AC1 Soft mismatch → mismatch counter; continue
  AC2 Hard mismatch → hard-fail + mark Done/cancel
  AC3 Happy path → no hard-fail / mismatch bump
  AC4 Query keys schema-2346 additive; decision table in fiber.h
  AC5 Unit test + this linter in pre-push gate

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

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/serve/test_steal_snapshot_hard_invariant_2346.cpp")
    cmake = _read("CMakeLists.txt")

    # AC1 Soft
    must("check_and_enforce_resume_snapshot_invariant", "AC1", fh)
    must("check_and_enforce_resume_snapshot_invariant", "AC1", fc)
    must("bump_mutation_steal_snapshot_mismatch", "AC1", fc)
    must("AC1: Soft", "AC1", test)

    # AC2 Hard
    must("is_steal_snapshot_hard_mode", "AC2", fh)
    must("AURA_STEAL_SNAPSHOT_HARD", "AC2", fh)
    must("bump_steal_snapshot_hard_fail", "AC2", fh)
    must("steal_snapshot_hard_fail_total_", "AC2", fc)
    must("request_cancel", "AC2", fc)
    must("FiberState::Done", "AC2", fc)
    must("AC2: Hard", "AC2", test)
    must("aura_production_defaults_active_probe", "AC2", hooks)

    # AC3 happy path
    must("AC3: happy path", "AC3", test)

    # AC4 query
    must("schema-2346", "AC4", q)
    must("steal-snapshot-mismatch-total", "AC4", q)
    must("steal-snapshot-hard-fail-total", "AC4", q)
    must("steal-snapshot-hard-wired", "AC4", q)
    must("schema-2184", "AC4", q)
    must("schema-2310", "AC4", q)
    must("decision table", "AC4", fh.lower())

    # AC5
    must("AC5: source-cite", "AC5", test)
    must("test_steal_snapshot_hard_invariant_2346", "AC5", cmake)
    must("Issue #2346", "AC5", fc)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2346 resume snapshot hard-invariant — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
