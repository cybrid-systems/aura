#!/usr/bin/env python3
"""Issue #3490: cache Harden arm; CONSTEXPR column checks honor it.

#3313 / #3428 landed production_defaults arm + view_at AURA_HOT_CHECK.
Residual: hot_contract_harden_armed() re-entered the C ABI probe on every
HOT_CHECK (probe-true returned before touching cached). AURA_HOT_CHECK_CONSTEXPR
stayed ((void)0) so column accessors were Quiet OOB if a caller skipped view_at.

Contract:
  AC1  cache load precedes probe; apply_production/apply_dev store the cache
  AC2  Soft / unarmed: expr not evaluated (armed()==false)
  AC3  CONSTEXPR runtime arm (if !consteval → AURA_HOT_CHECK); column accessors
  AC4  view_at AURA_HOT_CONTRACT + trap helper retained (#3428 / #3106 / #3501)
  AC5  no new query key / g_3490_* / docs/design / test_issue_3490.cpp;
       linter AFTER #3313

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
    soa = _read("src/compiler/ir_soa.ixx")
    tma = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_contract_placement.cpp")
    build = _read("build.py")
    lint3313 = _read("scripts/coverage/checks/check_hot_contract_production_harden_3313.py")
    lint3428 = _read("scripts/coverage/checks/check_hot_contract_view_at_harden_3428.py")

    must("kHotContractHardenCacheIssue = 3490", "AC1 stamp", hh)
    must("hot_contract_harden_armed_cache", "AC1 cache", hh)
    must("note_hot_contract_harden_armed", "AC1 note", hh)
    fn = hh.find("[[nodiscard]] inline bool hot_contract_harden_armed()")
    nxt = hh.find("peek_hot_contracts_mode_env", fn) if fn >= 0 else -1
    win = hh[fn:nxt] if fn >= 0 and nxt > fn else ""
    load = win.find("hot_contract_harden_armed_cache.load")
    probe = win.find("aura_production_defaults_active_probe() != 0")
    if load < 0 or probe < 0 or load >= probe:
        fails.append("AC1: cache load must precede C ABI probe")
    must("if (v >= 0)", "AC1 cache-hit return", win)
    must("note_hot_contract_harden_armed(true)", "AC1 apply_production store", tma)
    must("note_hot_contract_harden_armed(false)", "AC1 apply_dev store", tma)
    must("3490 AC1", "AC1 test", test)
    must("if (parsed == 0 && ", "AC1 3139 implicit-arm guard kept", hh)

    must("expr not evaluated", "AC2 Soft skip", hh)
    must("3490 AC2", "AC2 test", test)
    must("AURA_HOT_MODE_OFF", "AC2 compile-time OFF kept", hh)

    must("if !consteval", "AC3 CONSTEXPR runtime arm", hh)
    must("AURA_HOT_CHECK(expr)", "AC3 CONSTEXPR uses CHECK", hh)
    must("AURA_HOT_CHECK_CONSTEXPR", "AC3 column accessors", soa)
    must("Issue #3490", "AC3 ir_soa cite", soa)
    must("3490 AC3", "AC3 test", test)

    start = soa.find("IRInstructionView view_at(")
    addb = soa.find("add_block", start) if start >= 0 else -1
    vwin = soa[start:addb] if start >= 0 and addb > start else ""
    must("AURA_HOT_CONTRACT", "AC4 view_at CONTRACT", vwin)
    must("record_hotpath_contract_harden_trap", "AC4 trap helper", hh)
    must("3490 AC4", "AC4 test", test)
    must("check_hot_contract_view_at_harden_3428", "AC4 3428 linter kept", build)
    must("3428", "AC4 3428 linter body", lint3428)
    must("3313", "AC4 3313 linter body", lint3313)

    must("check_hot_contract_harden_cache_3490", "AC5 build.py", build)
    prev = build.find("check_hot_contract_production_harden_3313")
    ours = build.find("check_hot_contract_harden_cache_3490")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3313")
    must("hot-contract-harden-trap-total", "AC5 reuse trap-total", q)
    must("hot-contract-harden-armed", "AC5 reuse armed", q)
    must("3490 AC5", "AC5 test", test)
    if "schema-3490" in q:
        fails.append("AC5: new schema-3490 query key")
    if "g_3490_" in hh or "g_3490_" in soa:
        fails.append("AC5: new g_3490_* counter")
    if "g_hotpath_3490" in hh:
        fails.append("AC5: new g_hotpath_3490 series")
    must("#define AURA_COLD_CONTRACT(expr) ((void)0)", "AC5 cold off unchanged", hh)
    if (ROOT / "tests" / "compiler" / "test_issue_3490.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3490.cpp present")
    if (ROOT / "tests" / "core" / "test_issue_3490.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3490.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3490-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3490 hot_contract_harden_cache:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3490 hot_contract_harden_cache: armed cached; CONSTEXPR runtime arm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
