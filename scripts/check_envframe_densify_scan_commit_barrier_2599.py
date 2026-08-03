#!/usr/bin/env python3
"""Issue #2599: EnvFrame densify ownership scan fail enters outermost
commit barrier (production-only gating).

Coverage gate: presence-checks for the production-only envframe scan block
in Phase 5 driver (src/compiler/evaluator_mutation_boundary.cpp) + test
additions + build.py wiring. Mirrors
`check_panic_residual_densify_hard_2598.py` /
`check_general_object_pin_auto_wire_2597.py` style.

Contract:
  AC16 Phase 5 driver gates scan_fail_delta on production_defaults_active
       (only envframe_block = prod && scan_fail_delta forces envframe_ok=false)
  AC17 EnvFrameDensifyOwnership deny reason cited (force_rollback authority)
  AC18 Source-cite for #2599 + #2340 / #2361 + force_linear_rollback (#2545)
  AC19 build.py wires cmd_envframe_densify_scan_commit_barrier_2599_coverage +
       gate script present

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

    driver = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed_2495.cpp")
    build = _read("build.py")

    # AC16: production-only envframe scan block in Phase 5 driver.
    must("Issue #2599", "AC16", driver)
    must("production_defaults_active()", "AC16", driver)
    must("envframe_block = prod_for_densify && scan_fail_delta", "AC16", driver)
    must(
        "pairing.envframe_ok && linear_type_ok && !envframe_block",
        "AC16",
        driver,
    )
    must("densify_ownership_scan_fail_total", "AC16", driver)

    # AC17: EnvFrameDensifyOwnership deny reason.
    must(
        "EnvFrameDensifyOwnership",
        "AC17",
        driver,
    )

    # AC18: source-cite for #2599 + related refs.
    must("Issue #2599", "AC18", driver)
    must("force_linear_rollback", "AC18", driver)
    must("#2545", "AC18", driver)

    # AC19: test additions + build.py wiring.
    must("Issue #2599", "test", test)
    must(
        "EnvFrame densify ownership scan fail enters outermost commit barrier (extends #2495 test file per #81967)",
        "test",
        test,
    )
    must("ac16_production_only_envframe_scan_block", "test", test)
    must("ac17_envframe_densify_ownership_deny_reason", "test", test)
    must("ac18_source_cite_2599", "test", test)
    must("ac19_build_gate_wiring_source_cite", "test", test)
    must(
        "envframe_block = prod_for_densify && scan_fail_delta",
        "test",
        test,
    )
    must("EnvFrameDensifyOwnership", "test", test)
    must(
        "cmd_envframe_densify_scan_commit_barrier_2599_coverage",
        "build",
        build,
    )
    must(
        "check_envframe_densify_scan_commit_barrier_2599",
        "build",
        build,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} envframe-densify-scan-commit-barrier (#2599) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2599 envframe densify scan commit barrier — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
