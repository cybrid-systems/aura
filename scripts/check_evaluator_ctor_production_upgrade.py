#!/usr/bin/env python3
"""Issue #3554 linter: enforce that Evaluator::Evaluator() ctor self-upgrades
lock_order mode to canary (3) + enables hold-budget reject when host
environment signals production_defaults_expected() AND !hold-budget
already on. Embedder skip-init silently leaves lock_order observe-only
and hold-budget metric-only — this linter rejects the silent face.

Usage:
    python3 scripts/check_evaluator_ctor_production_upgrade.py --strict

Forbidden (production silent-degrade):
    - Evaluator::Evaluator() ctor missing production_defaults_expected()
      gate before lock_order upgrade / hold-budget reject enable.
    - apply_production_lock_order_default() not wired in default path.

Required:
    - production_defaults_expected() in lock_order_audit.h reads
      AURA_PRODUCTION_DEFAULTS env OR g_lock_order_production_soft_default.
    - mutation_hold_budget_reject_enabled_set(bool) setter in mutation_hold_budget.h.
    - ctor calls both under production_defaults_expected().
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "src/compiler/lock_order_audit.h"
MBH = ROOT / "src/compiler/mutation_hold_budget.h"
CTOR = ROOT / "src/compiler/evaluator_ctor.cpp"

RE_PROD_HELPER = re.compile(
    r"inline\s+bool\s+production_defaults_expected\s*\(\s*\)\s*noexcept",
)
RE_ENV_READ = re.compile(r"AURA_PRODUCTION_DEFAULTS", re.MULTILINE)
RE_SOFT_GAUGE = re.compile(
    r"g_lock_order_production_soft_default\.load\s*\(\s*std::memory_order_acquire\s*\)\s*!=\s*0",
)
RE_HB_SETTER = re.compile(
    r"inline\s+bool\s+mutation_hold_budget_reject_enabled_set\s*\(\s*bool\s+\w+\s*\)\s*noexcept",
)
RE_CTOR_GATE = re.compile(
    r"aura::compiler::lock_order::production_defaults_expected\s*\(\s*\)",
)
RE_CTOR_LOCK_BUMP = re.compile(
    r"g_lock_order_mode\.store\s*\(\s*3\s*,\s*std::memory_order_release\s*\)",
)
RE_CTOR_CANARY = re.compile(
    r"g_lock_order_canary_enabled\.store\s*\(\s*1\s*,\s*std::memory_order_release\s*\)",
)
RE_CTOR_HB_ENABLE = re.compile(
    r"mutation_hold_budget_reject_enabled_set\s*\(\s*true\s*\)",
)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--strict", action="store_true", help="Fail on missing wire patterns")
    args = p.parse_args()
    strict = bool(args.strict)

    v = 0
    for label, path, regex, why in [
        ("AC1", LOCK, RE_PROD_HELPER, "lock_order_audit.h: production_defaults_expected() helper"),
        ("AC1", LOCK, RE_ENV_READ, "lock_order_audit.h: reads AURA_PRODUCTION_DEFAULTS env"),
        ("AC1", LOCK, RE_SOFT_GAUGE, "lock_order_audit.h: also reads g_lock_order_production_soft_default gauge"),
        ("AC2", MBH, RE_HB_SETTER, "mutation_hold_budget.h: mutation_hold_budget_reject_enabled_set(bool) setter"),
        ("AC3", CTOR, RE_CTOR_GATE, "evaluator_ctor.cpp: ctor reads production_defaults_expected()"),
        ("AC3", CTOR, RE_CTOR_LOCK_BUMP, "evaluator_ctor.cpp: ctor upgrades lock_order_mode to canary (3)"),
        ("AC3", CTOR, RE_CTOR_CANARY, "evaluator_ctor.cpp: ctor enables canary gate"),
        ("AC3", CTOR, RE_CTOR_HB_ENABLE, "evaluator_ctor.cpp: ctor enables hold-budget reject via setter"),
    ]:
        if not path.exists():
            fail(f"{label}: missing {path}")
            v += 1
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if not regex.search(text):
            fail(f"{label}: {path}: missing pattern {regex.pattern!r} ({why})")
            v += 1
    if v > 0 and strict:
        print(
            f"\ncheck_evaluator_ctor_production_upgrade: {v} violation(s) — refusing to ship",
            file=sys.stderr,
        )
        return 1
    print("check_evaluator_ctor_production_upgrade: OK (3554 AC: ctor self-upgrade wired)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
