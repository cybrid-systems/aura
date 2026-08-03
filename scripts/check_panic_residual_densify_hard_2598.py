#!/usr/bin/env python3
"""Issue #2598: production densify-after panic residual → hard
(align with steal residual hard-AND).

Coverage gate: presence-checks for the production-lock wiring in
audit_panic_defer_after_densify (src/core/gc_hooks.h) + soft override
helper + test additions + build.py wiring. Mirrors
`check_moving_untracked_production_hard_2596.py` /
`check_general_object_pin_auto_wire_2597.py` style.

Contract:
  AC6 gc_defer_production_locked() callable + gc_hooks.h declares it
  AC7 panic_contract_soft_override() recognizes soft / off env
  AC8 audit_panic_defer_after_densify modified to read production_lock +
      soft_override (hard_from_env OR (production_lock && !soft_override))
  AC9 build.py wires cmd_panic_residual_densify_hard_2598_coverage +
      scripts/check_panic_residual_densify_hard_2598.py present

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

    gh = _read("src/core/gc_hooks.h")
    test = _read("tests/compiler/test_panic_defer_after_densify_2364.cpp")
    build = _read("build.py")

    # AC6: gc_defer_production_locked helper source-cite.
    must("gc_defer_production_locked", "AC6", gh)
    must("set_gc_defer_production_locked", "AC6", gh)
    must("g_production_locked", "AC6", gh)

    # AC7: panic_contract_soft_override helper.
    must("panic_contract_soft_override", "AC7", gh)
    must('"soft"', "AC7", gh)
    must('"off"', "AC7", gh)

    # AC8: audit_panic_defer_after_densify modified to read production lock.
    must("Issue #2598", "AC8", gh)
    must("production_lock && !soft_override", "AC8", gh)
    must("hard_from_env", "AC8", gh)
    must("hard_from_env || (production_lock && !soft_override)", "AC8", gh)

    # AC9: test additions + build.py wiring.
    must("Issue #2598", "test", test)
    must("ac6_production_lock_helper_source_cite", "test", test)
    must("ac7_soft_override_helper", "test", test)
    must("ac8_production_lock_source_cite", "test", test)
    must("ac9_build_gate_wiring_source_cite", "test", test)
    must("panic_contract_soft_override", "test", test)
    must("production_lock && !soft_override", "test", test)
    must("cmd_panic_residual_densify_hard_2598_coverage", "build", build)
    must("check_panic_residual_densify_hard_2598", "build", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} panic-residual-densify-hard (#2598) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2598 panic residual densify hard — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
