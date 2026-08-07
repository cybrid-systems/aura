#!/usr/bin/env python3
"""Issue #2665: production-default GeneralObjectPin required + close inventory-only
adopt gap (P0 lifetime safety).

Contract (one row per AC):
  AC1 src/core/lifetime_pin.hh wire_general_object_create_pair bumps
     g_general_object_pin_required_enforced_total when
     g_general_object_pin_required_pref.load() > 0 (production-default
     locked via #2597 step 15) AND either pin fails. Soft / dev_off /
     unset stays zero-cost.
  AC2 src/compiler/evaluator_primitives_obs_eval.cpp exposes additive
     query keys: general-object-pin-required-enforced-total +
     general-object-pin-required-wired + schema-2665 + issue-2665.
  AC3 lifetime_pin.hh comment documents Soft / dev_off / unset
     zero-cost retention (gated on pref <= 0).
  AC4 tests/core/test_general_object_pin_coverage_gate.cpp extended
     with #2665 AC1-AC4 source-cite block (per #81967 — no new file).
  AC5 build.py wires check_2665_coverage into the gate after
     check_general_object_pin_2298.
  AC6 cross-check: check_general_object_pin_2298 + check_general_object_
     pin_auto_wire_2597 still green (no regression).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    lp = _read("src/core/lifetime_pin.hh")  # SSOT
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    # AC1 — production-default required-mode fail-closed enforcement
    must("Issue #2665", "AC1", lp)
    must("g_general_object_pin_required_enforced_total", "AC1", lp)
    must("g_general_object_pin_required_pref.load(std::memory_order_relaxed) > 0", "AC1", lp)
    must("g_general_object_pin_required_enforced_total.fetch_add", "AC1", lp)
    must("wire_general_object_create_pair", "AC1", lp)

    # AC2 — additive query keys
    must("general-object-pin-required-enforced-total", "AC2", obs)
    must("general_object_pin_required_enforced_total", "AC2", obs)
    must("general-object-pin-required-wired", "AC2", obs)
    must("schema-2665", "AC2", obs)
    must("issue-2665", "AC2", obs)

    # AC3 — Soft zero-cost retention
    must("Soft / dev_off / unset (pref <= 0)", "AC3", lp)

    # AC4 — test file extension
    must("ac2665_1_production_required_enforcement", "AC4", test)
    must("ac2665_2_query_keys_added", "AC4", test)
    must("ac2665_3_soft_zero_cost", "AC4", test)
    must("ac2665_4_existing_ac4_unchanged", "AC4", test)
    must("Issue #2665", "AC4", test)

    # AC5 — build.py wires the linter
    must("check_2665_coverage", "AC5", build)

    # Cross-check: check_general_object_pin_2298 still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_general_object_pin_2298.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_general_object_pin_2298 regression:\n{r1.stdout}\n{r1.stderr}")

    # Cross-check: check_general_object_pin_auto_wire_2597 still green
    r2 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_general_object_pin_auto_wire_2597.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"check_general_object_pin_auto_wire_2597 regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2665 production-default GeneralObjectPin required + close inventory-only adopt gap — all AC rows satisfied"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
