#!/usr/bin/env python3
"""Issue #3666: AURA_PRODUCTION_PACK compiles Harden as a constant.

#3490 cached the probe; #3501 folded RECORD+CHECK to one armed() load.
Production pack still loaded hot_contract_harden_armed_cache on every
as_int / view_at. Defaults never disarm Harden in that binary.

Contract (one row per AC):
  AC1  pack OFF redefine: CHECK/CONTRACT do not call armed() / load cache;
       as_int call sites unchanged; OOB still abort + trap
  AC2  Soft/unit keep cache; unarmed does not evaluate expr
  AC3  #3501 non-pack OFF CONTRACT still one armed() load; #3428 view_at
  AC4  apply_production flip is non-pack; pack is compile-armed
  AC5  extend placement + pack-pipeline suites; linter AFTER #3665;
       no invent; no docs/design; no new query key

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    hh = _read("src/core/cpp26_contract_stats.h")
    val = _read("src/compiler/value.ixx")
    soa = _read("src/compiler/ir_soa.ixx")
    place = _read("tests/compiler/test_hot_contract_placement.cpp")
    pack = _read("tests/compiler/test_pack_pipeline_strict.cpp")
    unify = _read("tests/compiler/test_hot_contract_unify.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    must("kHotContractProductionPackIssue = 3666", "AC1 stamp", hh)
    must("kHotContractProductionPackCompileArmed", "AC1 constexpr", hh)
    guard = hh.find("#if defined(AURA_HOT_MODE_OFF) && defined(AURA_PRODUCTION_PACK)")
    if guard < 0:
        fails.append("AC1: missing pack OFF redefine guard")
        pwin = ""
    else:
        end = hh.find("#endif", guard)
        pwin = hh[guard:end] if end > guard else ""
    must("std::abort()", "AC1 pack abort", pwin)
    must("record_hotpath_contract_harden_trap", "AC1 pack trap", pwin)
    must_not("hot_contract_harden_armed()", "AC1 pack no armed()", pwin)
    must_not("hot_contract_harden_armed_cache", "AC1 pack no cache load", pwin)
    must("AURA_HOT_CONTRACT(is_int(v))", "AC1 as_int CONTRACT", val)
    must("AURA_HOT_CHECK((v.val & 1) == 0)", "AC1 as_int CHECK", val)
    must("ac3666_pack_hot_check_compile_armed", "AC1 pack test", pack)

    must("3666 AC2: Soft unarmed", "AC2 test", place)
    must("expr not evaluated", "AC2 Soft skip kept", hh)
    must("hot_contract_harden_armed_cache", "AC2 cache kept", hh)

    d1 = hh.find("#define AURA_HOT_CONTRACT")
    d2 = hh.find("#define AURA_HOT_CONTRACT", d1 + 1 if d1 >= 0 else 0)
    off_body = hh[d1:d2] if d1 >= 0 and d2 > d1 else ""
    armed_n = off_body.count("hot_contract_harden_armed()")
    if armed_n != 1:
        fails.append(f"AC3: #3501 OFF CONTRACT armed() count {armed_n} != 1")
    must("AURA_HOT_CONTRACT", "AC3 view_at", soa)
    must("3501 AC1: single hot_contract_harden_armed() in OFF CONTRACT", "AC3 3501 test", unify)
    must("3666 AC3: view_at CONTRACT kept", "AC3 3428", place)

    fn = hh.find("[[nodiscard]] inline bool hot_contract_harden_armed()")
    nxt = hh.find("peek_hot_contracts_mode_env", fn) if fn >= 0 else -1
    awin = hh[fn:nxt] if fn >= 0 and nxt > fn else ""
    must("AURA_PRODUCTION_PACK", "AC4 armed() pack", awin)
    must("return true", "AC4 compile true", awin)
    must("3666 AC4: apply_production arms", "AC4 test", place)
    must("3666 AC4: pack stays armed after note(false)", "AC4 pack test", pack)

    must("check_insert_remove_child_locked_dense_splice_3665", "AC5 prev linter", build)
    must("check_hot_contract_production_pack_3666", "AC5 build.py", build)
    prev = build.find("check_insert_remove_child_locked_dense_splice_3665")
    ours = build.find("check_hot_contract_production_pack_3666")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3665")
    must_not("schema-3666", "AC5 no new query key", q + hh)
    must_not("g_hotpath_3666", "AC5 no new series", hh)
    must("hot-contract-harden-trap-total", "AC5 reuse trap-total", q)
    if _read("tests/compiler/test_issue_3666.cpp") or _read("tests/core/test_issue_3666.cpp"):
        fails.append("AC5: test_issue_3666.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3666-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3666 hot_contract_production_pack:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3666 hot_contract_production_pack")
    return 0


if __name__ == "__main__":
    sys.exit(main())
