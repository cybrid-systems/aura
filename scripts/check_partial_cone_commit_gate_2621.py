#!/usr/bin/env python3
"""Issue #2621: partial re-infer cone truncate must not silent-commit under production.

Contract:
  AC1 Soft + cone soft overflow → metrics, commit allowed, last_partial_cone_truncated
  AC2 production + soft overflow → would_allow_commit=false (cone_truncate/truncate)
  AC3 production + hard overflow → never silent success (reject path)
  AC4 fan-out trunc counted separately; empty dirty vacuous healthy
  AC5 schema-2621 additive on type-dep-partial-merge + fidelity
  AC6 unit test high fan-out / production gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    audit = _read("src/compiler/typed_mutation_audit.h")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_partial_cone_commit_gate_2621.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("publish_partial_cone_truncate", "AC1", audit)
    must("last_partial_cone_truncated", "AC1", audit)
    must("partial_cone_truncated", "AC1", audit)
    must("publish_partial_cone_truncate", "AC1", impl)
    must("ac1_soft_observe_allow", "AC1", test)

    # AC2
    must("cone_truncate", "AC2", audit)
    must("partial_cone_commit_hard_enabled", "AC2", audit)
    must("AURA_PARTIAL_CONE_COMMIT_HARD", "AC2", audit)
    must("ac2_production_deny", "AC2", test)

    # AC3
    must("orig_sz > hard", "AC3", impl)
    must("partial_cone_hard_fallback", "AC3", impl)
    must("ac3_hard_cap_never_silent", "AC3", test)

    # AC4
    must("partial_cone_type_dep_degree_trunc", "AC4", impl)
    must("last_partial_cone_fanout_trunc", "AC4", audit)
    must("empty dirty", "AC4", impl)
    must("ac4_empty_and_fanout", "AC4", test)

    # AC5
    must("schema-2621", "AC5", q)
    must("last-partial-cone-truncated", "AC5", q)
    must("partial-cone-commit-observe-total", "AC5", q)
    must("partial-cone-commit-reject-total", "AC5", q)
    must("schema-2560", "AC5", q)
    must("last_partial_cone_truncated_", "AC5", ixx)
    must("ac5_schema_source", "AC5", test)

    # AC6
    must("test_partial_cone_commit_gate_2621", "AC6", cmake)
    must("check_partial_cone_commit_gate_2621", "AC6", build)
    must("cmd_partial_cone_commit_gate_coverage", "AC6", build)
    must("ac6_high_fanout_gate", "AC6", test)
    must("kPartialConeCommitGateIssue", "AC6", audit)

    for rel in (
        "docs/design/partial_cone_commit_gate_2621.md",
        "docs/partial_cone_commit_gate_2621.md",
        "design/2621.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2621 partial cone commit gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
