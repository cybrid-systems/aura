#!/usr/bin/env python3
"""Issue #3447: JIT dual-fresh observes facade C-bridge AND table epoch.

Owner-scoped hard invalidate bumps g_current_bridge_epoch but does not
fetch_add g_aot_table_epoch. Dual-fresh used to sample only table, so
owner live closures stayed green.

Contract:
  AC1 single-eval: miss on either C-bridge or table clock
  AC2 owner-scoped: table frozen, C-bridge miss until remount restamp
  AC3 no table force-bump; peer name soft-stale (#3300) kept
  AC4 captured==0 + tracking stale on both clocks (#2930); LEGACY_TRUST kept
  AC5 #3410 still fires on visible miss (including C-bridge); not sole path
  AC6 non-duplicative vs #3410/#3412/#3377/#3300/#2841/#2951; no new query key

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    br = (ROOT / "src" / "compiler" / "aura_jit_bridge.cpp").read_text()
    hh = (ROOT / "src" / "compiler" / "aura_jit_bridge.h").read_text()
    rt = (ROOT / "src" / "compiler" / "aura_jit_runtime.cpp").read_text()
    stub = (ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp").read_text()
    hot = (ROOT / "src" / "compiler" / "hot_update_registry.cpp").read_text()
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    cow = (ROOT / "tests" / "compiler" / "test_cross_cow_soft_migrate.cpp").read_text()
    mutate = (ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    fn = br.find("bool aura_is_jit_closure_fresh")
    body = br[fn : fn + 2800] if fn >= 0 else ""
    must("Issue #3447", "AC1 marker", br)
    must("aura_get_current_bridge_epoch()", "AC1 C-bridge sample", body)
    must("g_aot_table_epoch.load", "AC1 table sample kept", body)
    must("cur_c_bridge", "AC1 C-bridge local", body)
    must("c_ok && table_ok", "AC1 AND both clocks", body)
    must("3447 AC1", "AC1 test", test)

    must("jit_closure_bridge_stamp_now", "AC2 remount stamp", rt)
    must("Issue #3447", "AC2 runtime cite", rt)
    must("3447 AC2", "AC2 test", test)

    must("Issue #3300", "AC3 peer name", rt)
    must("aura_aot_peer_jit_name_is_soft_stale", "AC3 name gate", rt)
    bump = br.find("aura_aot_bump_func_table_epoch")
    bump_body = br[bump : bump + 2200] if bump >= 0 else ""
    must("g_aot_table_epoch", "AC3 table atom", bump_body)
    must("3447 AC3", "AC3 test", test)

    must("note_observed()", "AC4 #2930 observe", body)
    must("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "AC4 legacy trust", body)
    must("3447 AC4", "AC4 test", test)

    must("Issue #3410", "AC5 #3410 kept", rt)
    must("cur_c_bridge", "AC5 #3410 sees C-bridge miss", rt)
    must("3447 AC5", "AC5 test", test)
    must("Issue #3410", "AC5 cow suite", cow)

    for marker in ("#3412", "#3377", "#3300", "#2841", "#2951"):
        if marker not in hot and marker not in rt and marker not in br:
            fails.append(f"AC6: {marker} upstream marker missing")
    must("3447 AC6", "AC6 test", test)
    must("check_jit_dual_fresh_c_bridge_3447", "AC6 build.py", build)
    must("Issue #3447", "AC6 header", hh)
    must("cur_c", "AC6 stub C-bridge", stub)
    must_not("schema-3447", "AC6 no new query key", mutate)
    if (ROOT / "tests" / "compiler" / "test_issue_3447.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3447.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3447.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3447.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3447-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3447 jit_dual_fresh_c_bridge:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3447 jit_dual_fresh_c_bridge: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
