#!/usr/bin/env python3
"""Issue #2982: Staging/Dlopen reload-fail ops recovery surface.

Agents get last_ops_fail_kind + path/detail hashes + Staging retry
eligibility without log scrape. Dlopen never auto-retries. Soft is
last_reason only.

Contract:
  AC1 Production Staging fail → ops fields + one scheduled retry
  AC2 Dlopen fail → path/errno visible; max_reemit=0
  AC3 Version/Env/Linear + #2927 bit map unchanged
  AC4 Soft / no fail → no extra path stores
  AC5 Additive schema-2982; #2093/#2249/#2927 preserved
  AC6 Extend test_reload_recovery_query; source-cite; no docs/design

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

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    brh = _read("src/compiler/aura_jit_bridge.h")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qeval = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2982", "AC1", hh)
    must("note_ops_fail_staging", "AC1", hh)
    must("staging_retry_eligible", "AC1", hh)
    must("staging_retry_scheduled_total", "AC1", cpp)
    must("AotReloadFail::Staging", "AC1", cpp)
    must("ac2982_1_staging_eligible_retry", "AC1", test)

    # AC2
    must("note_ops_fail_dlopen", "AC2", hh)
    must("note_ops_fail_dlopen", "AC2", br)
    must("last_dlopen_path_hash", "AC2", hh)
    must("last_dlopen_errno_class", "AC2", hh)
    must("max_reemit=*/0", "AC2", brh)
    must("ac2982_2_dlopen_no_auto_retry", "AC2", test)

    # AC3
    must("aot_reload_fail_to_force_jit_mask", "AC3", brh)
    must("AotReloadFail::Env", "AC3", brh)
    must("ac2982_3_version_env_unchanged", "AC3", test)

    # AC4
    must("zero extra stores", "AC4", cpp)
    must("aura_production_defaults_active_probe() == 0", "AC4", cpp)
    must("ac2982_4_soft_zero_extra", "AC4", test)

    # AC5
    must("schema-2982", "AC5", mut)
    must("schema-2927", "AC5", mut)
    must("schema-2249", "AC5", qeval)
    must("schema_2982", "AC5 snap", hh)
    must("ac2982_5_query_keys", "AC5", test)

    # AC6
    must("Issue #2982", "AC6 cpp", cpp)
    must("check_staging_dlopen_ops_recovery_2982", "AC6", build)
    must("ac2982_6_source_and_linter", "AC6", test)
    if (ROOT / "tests" / "compiler" / "test_issue_2982.cpp").is_file():
        fails.append("AC6: test_issue_2982.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2982*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2982 Staging/Dlopen ops recovery surface — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
