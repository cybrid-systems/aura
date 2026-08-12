#!/usr/bin/env python3
"""Issue #2931: promote steal×mutate×GC×mailbox chaos soak to nightly hard gate.

Contract (one row per AC):
  AC1 Nightly (or equivalent scheduled) job runs the chaos harness with
      duration ≥ 600s and workers ≥ 8 under production-like defaults.
  AC2 Fail-closed on unbounded residual_defer_after_exit growth without
      matching clears; resume_fence_fail / ticket-mismatch hard surplus
      == 0 under production defaults (Soft override documented).
  AC3 Default PR / ./build.py test unaffected (EXCLUDE_FROM_ALL + env gate).
  AC4 End-of-run snapshot still surfaces schema-2310…2314 + schema-2846.
  AC5 Source-cite + coverage linter wired in build.py; tests stay
      src-aligned (test_chaos_steal_mutation_gc.cpp).
  AC6 No docs/design/* per #1655.

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

    chaos = _read("tests/serve/test_chaos_steal_mutation_gc.cpp")
    build = _read("build.py")
    nightly = _read(".github/workflows/nightly.yml")
    cmake = _read("CMakeLists.txt")

    # AC1 — nightly duration ≥ 600, workers ≥ 8, env gate.
    must("AURA_CHAOS_STEAL_GC", "AC1", nightly)
    must("AURA_CHAOS_DURATION_S", "AC1", nightly)
    must("AURA_CHAOS_WORKERS", "AC1", nightly)
    must('"600"', "AC1 duration", nightly)
    must('"8"', "AC1 workers", nightly)
    must("test_chaos_steal_mutation_gc", "AC1", nightly)
    must("chaos-steal-gc-nightly", "AC1 job", nightly)
    must("cmd_chaos_steal_gc_nightly_2931", "AC1", build)
    must("chaos-steal-gc-nightly-2931", "AC1", build)

    # AC2 — hard-fail keys in harness.
    must("residual_defer_after_exit", "AC2", chaos)
    must("residual_defer_after_exit_total", "AC2", chaos)
    must("matching_clears", "AC2", chaos)
    must("resume_fence_fail_total", "AC2", chaos)
    must("steal_safety_ticket_mismatch", "AC2", chaos)
    must("residual_defer_steal_hard_fail", "AC2", chaos)
    must("defer_reasons_snapshot", "AC2", chaos)
    must("#2931: residual_defer_after_exit explained by matching clears", "AC2", chaos)
    must("#2931: steal_safety_ticket_mismatch delta == 0", "AC2", chaos)
    must("#2931: resume_fence hard/ticket surplus == 0", "AC2", chaos)
    must("#2931: residual_defer_steal_hard_fail delta == 0", "AC2", chaos)
    must("soft_steal_override", "AC2 Soft", chaos)
    must("AURA_STEAL_SNAPSHOT_SOFT", "AC2 Soft", chaos)
    must("reset_steal_snapshot_soft_for_test", "AC2 production Soft off", chaos)
    must("production-like defaults", "AC2 production-like", chaos)

    # AC3 — EXCLUDE_FROM_ALL + env gate preserved.
    must("EXCLUDE_FROM_ALL", "AC3", cmake)
    must("test_chaos_steal_mutation_gc", "AC3", cmake)
    must("AURA_CHAOS_STEAL_GC", "AC3", chaos)
    must("chaos_enabled", "AC3", chaos)
    # CMake wires EXCLUDE_FROM_ALL on the steal-gc target specifically.
    if "set_target_properties(test_chaos_steal_mutation_gc PROPERTIES EXCLUDE_FROM_ALL TRUE)" not in cmake:
        fails.append("AC3: EXCLUDE_FROM_ALL not set on test_chaos_steal_mutation_gc")

    # AC4 — schema snapshots including 2846.
    must("schema-2310", "AC4", chaos)
    must("schema-2311", "AC4", chaos)
    must("schema-2312", "AC4", chaos)
    must("schema-2313", "AC4", chaos)
    must("schema-2314", "AC4", chaos)
    must("schema-2846", "AC4", chaos)
    must("residual-defer-after-exit-total", "AC4", chaos)
    must("residual-defer-after-exit-wired", "AC4", chaos)

    # AC5 — source-cite + linter wire.
    must("ac2931_1_nightly_duration_workers", "AC5", chaos)
    must("ac2931_2_residual_resume_fail_closed", "AC5", chaos)
    must("ac2931_3_exclude_from_all_env_gate", "AC5", chaos)
    must("ac2931_4_schema_2846_snapshot", "AC5", chaos)
    must("ac2931_5_source_and_linter", "AC5", chaos)
    must("check_chaos_steal_gc_nightly_2931", "AC5", build)
    must("cmd_chaos_steal_gc_nightly_2931_coverage", "AC5", build)
    must("cmd_chaos_steal_gc_nightly_2931", "AC5", build)
    must('"chaos-steal-gc-nightly-2931": cmd_chaos_steal_gc_nightly_2931,', "AC5", build)
    must(
        '"chaos-steal-gc-nightly-2931-coverage": cmd_chaos_steal_gc_nightly_2931_coverage,',
        "AC5",
        build,
    )

    # AC6 — no docs/design invent.
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2931-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2931.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2931.cpp present (forbidden invent)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2931 chaos steal-gc nightly hard gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
