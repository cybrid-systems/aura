#!/usr/bin/env python3
"""Issue #3313: production_defaults arms Soft-observe+Harden for NDEBUG OFF.

Residual of #2435/#3043/#3106: the runtime probe already consulted
production_defaults_active() (#3139) but NDEBUG macros stayed ((void)0).
Under production_defaults the same binary now fail-closes (sampled RECORD
+ observe + trap + abort) without -DAURA_HOT_SOFT_OBSERVE_HARDEN.

Contract:
  AC1  production_defaults + false AURA_HOT_CHECK → observe + trap + abort
  AC2  Soft / sandbox=off / unit: macros skip (armed()==0, expr not evaluated)
  AC3  sample period still applies under Harden (sampled RECORD)
  AC4  linter after #3106; test_hot_contract_placement; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/core/cpp26_contract_stats.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_contract_placement.cpp")
    build = _read("build.py")
    lint3106 = _read("scripts/coverage/checks/check_hot_contract_harden_3106.py")

    must("Issue #3313", "AC1 header", hh)
    must("kHotContractProductionHardenIssue = 3313", "AC1 stamp", hh)
    must("hot_contract_harden_armed()", "AC1 OFF macros consult probe", hh)
    must("observe_hot_contract_false()", "AC1 observe", hh)
    must("record_hotpath_contract_harden_trap()", "AC1 trap", hh)
    must("std::abort()", "AC1 abort", hh)
    must("3313 AC1", "AC1 test", test)
    must("schema-3313", "AC1 query stamp", q)
    must("hot-contract-harden-armed", "AC1 reuse armed key", q)
    must("hot-contract-harden-trap-total", "AC1 reuse trap-total", q)

    must("3313 AC2", "AC2 test", test)
    must("expr not evaluated", "AC2 Soft skip", hh)
    must("AURA_HOT_MODE_OFF", "AC2 compile-time OFF kept", hh)

    must("kHotSoftObserveRecordSample = 256", "AC3 sample period", hh)
    must("record_hotpath_invariant_hit_sampled", "AC3 sampled RECORD", hh)
    must("3313 AC3", "AC3 test", test)

    must("check_hot_contract_production_harden_3313", "AC4 build.py", build)
    must("3313 AC4", "AC4 test", test)
    must("3313", "AC4 3106 lineage", lint3106)
    prev = build.find("check_hot_contract_harden_3106")
    ours = build.find("check_hot_contract_production_harden_3313")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3106")
    if "schema-3313" not in q:
        fails.append("AC4: additive schema-3313 missing")
    if (ROOT / "tests" / "compiler" / "test_issue_3313.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3313.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3313.cpp").is_file():
        fails.append("AC4: forbidden tests/core/test_issue_3313.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3313-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")
    must("#define AURA_COLD_CONTRACT(expr) ((void)0)", "AC4 cold off unchanged", hh)

    if fails:
        print("FAIL #3313 hot_contract_production_harden:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3313 hot_contract_production_harden: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
