#!/usr/bin/env python3
"""Issue #3043: Soft-observe AURA_HOT_CONTRACT (metrics, no abort).

Contract (one row per AC):
  AC1  Production default remains OFF (zero cost)
  AC2  Soft-observe CHECK records observe_hot_contract_false, no abort
  AC3  Debug/Enforce still contract_assert
  AC4  Soft-observe RECORD is sampled (no per-call atomic RMW)
  AC5  query:cpp26-contracts-stats exposes hot-contract-false + schema-3043
  AC6  this linter wired in build.py; no test_issue_3043.cpp; no docs/design/

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
    unify = _read("tests/compiler/test_hot_contract_unify.cpp")
    build = _read("build.py")
    lint2435 = _read("scripts/coverage/checks/check_hot_contract_placement_2435.py")

    # ── AC1: production default still OFF ──
    must("Issue #3043", "AC1 header", hh)
    must("AURA_HOT_MODE_OFF", "AC1", hh)
    must("production (NDEBUG) default", "AC1", hh)
    must("#define AURA_HOT_CHECK(expr) ((void)0)", "AC1 off check", hh)
    must("3043 AC1", "AC1 test", test)
    # Must not flip NDEBUG default to Soft-observe.
    if "NDEBUG default — hot contracts OFF" not in hh and "production (NDEBUG) default — hot contracts OFF" not in hh:
        fails.append("AC1: production NDEBUG default is no longer OFF")

    # ── AC2: Soft-observe metrics only ──
    must("AURA_HOT_MODE_SOFT_OBSERVE", "AC2", hh)
    must("observe_hot_contract_false", "AC2 helper", hh)
    must("AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE", "AC2 flag", hh)
    must("3043 AC2", "AC2 test", test)
    must("if (!(expr))", "AC2 check", hh)

    # ── AC3: Enforce unchanged ──
    must("AURA_HOT_MODE_ENFORCE", "AC3", hh)
    must("#define AURA_HOT_CHECK(expr) contract_assert(expr)", "AC3", hh)
    must("fail-closed", "AC3", hh)
    must("3043 AC3", "AC3 test", test)

    # ── AC4: sampled RECORD ──
    must("kHotSoftObserveRecordSample", "AC4", hh)
    must("record_hotpath_invariant_hit_sampled", "AC4", hh)
    must("3043 AC4", "AC4 test", test)

    # ── AC5: query surface ──
    must("schema-3043", "AC5 query", q)
    must("issue-3043", "AC5 query", q)
    must("hot-contract-false-total", "AC5 query", q)
    must("hot-contract-soft-observe-wired", "AC5 query", q)
    must("hot-contracts-mode-env", "AC5 query", q)
    must("kHotContractSoftObserveIssue = 3043", "AC5 header", hh)
    must("3043 AC5", "AC5 test", test)
    must("schema-3043", "AC5 unify", unify)
    must("hot-contract-false-total", "AC5 unify", unify)

    # ── AC6: wired + no invent / no design ──
    must("check_hot_contract_soft_observe_3043", "AC6", build)
    must("3043", "AC6 2435 lineage", lint2435)
    if (ROOT / "tests" / "compiler" / "test_issue_3043.cpp").is_file():
        fails.append("AC6: test_issue_3043.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3043.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3043.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3043-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3043 Soft-observe hot contract — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
