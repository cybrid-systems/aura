#!/usr/bin/env python3
"""check_stable_func_id_eval_namespace_coverage.py — Issue #2670 source gate.

stable_func_id map namespace by (eval_owner, name) for multi-eval safety
(refine single-workspace #2550). Two Evaluator instances sharing a process
get distinct sids per eval for the same Define name (no map collision);
legacy callers without eval owner registered still see identical single-
workspace behavior (legacy C funcs dispatch via aura_aot_get_reemit_owner_
eval() ?: aura_aot_get_register_owner_eval() ?: nullptr).

AC1: two evals, same Define name → distinct stable_func_ids; each preserves
     across reemit
AC2: single-workspace (nullptr / default) behavior identical to pre-change
AC3: named set_name under eval A does not overwrite eval B map entry
AC4: clear_for_eval(A) leaves B entries intact
AC5: query size + preserve/assign counters still advance
AC6: src-aligned test (extend test_named_closure_stable_id_at_create.cpp
     #2550 suite per #81967) + coverage gate (this linter + build.py
     cmd_stable_func_id_eval_namespace_coverage).

Default: non-strict (exit 0, prints coverage summary). Use --strict to
enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BRIDGE_CPP = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_H = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRIDGE_STUB = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
BUILD = ROOT / "build.py"
TEST_2550 = ROOT / "tests" / "compiler" / "test_named_closure_stable_id_at_create.cpp"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    def must_present(path: Path, needle: str, label: str) -> None:
        if not path.exists():
            failures.append(f"{label}: {path} not found")
            return
        text = path.read_text(encoding="utf-8", errors="replace")
        if needle not in text:
            failures.append(f"{label}: missing {needle!r} in {path.name}")

    cpp_text = _read("src/compiler/aura_jit_bridge.cpp")
    hh_text = _read("src/compiler/aura_jit_bridge.h")
    _stub_text = _read("src/compiler/aura_jit_bridge_stub.cpp")  # noqa: F841 — used via must_present

    # AC1+AC3+AC4: nested map keyed by eval_owner + for_eval helpers + C funcs.
    must_present(
        BRIDGE_CPP,
        "Issue #2670: namespace by eval_owner",
        "AC1: cpp cites #2670 namespace-by-eval comment",
    )
    must_present(
        BRIDGE_CPP,
        "g_eval_to_stable_func_id",
        "AC1: cpp uses nested map keyed by eval_owner (was g_name_to_stable_func_id)",
    )
    must_present(
        BRIDGE_CPP,
        "preserve_stable_func_id_for_eval_locked",
        "AC1: cpp defines preserve helper that takes eval_ptr",
    )
    must_present(
        BRIDGE_CPP,
        "lookup_stable_func_id_for_eval_locked",
        "AC3: cpp defines lookup helper that takes eval_ptr",
    )
    must_present(
        BRIDGE_CPP,
        "stable_func_id_map_size_locked",
        "AC5: cpp defines size helper that sums inner sizes",
    )
    must_present(
        BRIDGE_CPP,
        "clear_stable_func_id_map_for_eval_locked",
        "AC4: cpp defines clear-for-eval helper",
    )
    must_present(
        BRIDGE_CPP,
        "clear_stable_func_id_map_all_locked",
        "AC4: cpp defines full-clear helper (process teardown / tests)",
    )

    # AC1: legacy callers dispatch via owner TLS (reemit ?: register ?: nullptr).
    if cpp_text:
        legacy_dispatch = (
            "aura_aot_get_reemit_owner_eval()" in cpp_text and "aura_aot_get_register_owner_eval()" in cpp_text
        )
        if not legacy_dispatch:
            failures.append("AC2: legacy C funcs must dispatch via owner TLS")

    # AC2: single-workspace (nullptr) behavior preserved.
    if cpp_text:
        # The legacy aura_get_or_preserve_stable_func_id must exist and call
        # the for_eval variant with owner TLS (or nullptr fallback).
        if "aura_get_or_preserve_stable_func_id(const char* name, int* out_preserved)" not in cpp_text:
            failures.append("AC2: legacy aura_get_or_preserve_stable_func_id(name, out) missing")
        # for_eval with nullptr must be reachable as the default-key path.
        if "aura_get_or_preserve_stable_func_id_for_eval(eval_owner" not in cpp_text:
            failures.append("AC2: for_eval dispatch must pass owner (or nullptr fallback)")

    # Bridge.h declarations for the 3 new for_eval variants.
    must_present(
        BRIDGE_H,
        "aura_get_or_preserve_stable_func_id_for_eval",
        "AC1: bridge.h declares aura_get_or_preserve_stable_func_id_for_eval",
    )
    must_present(
        BRIDGE_H,
        "aura_lookup_stable_func_id_for_eval",
        "AC3: bridge.h declares aura_lookup_stable_func_id_for_eval",
    )
    must_present(
        BRIDGE_H,
        "aura_clear_stable_func_id_map_for_eval",
        "AC4: bridge.h declares aura_clear_stable_func_id_map_for_eval",
    )
    must_present(
        BRIDGE_H,
        "Issue #2670",
        "AC6: bridge.h cites #2670 in for_eval declarations block",
    )

    # Stub file: weak fallbacks for the 3 new for_eval variants (light tests).
    must_present(
        BRIDGE_STUB,
        "aura_get_or_preserve_stable_func_id_for_eval",
        "AC6: stub provides weak fallback for get_or_preserve_for_eval",
    )
    must_present(
        BRIDGE_STUB,
        "aura_lookup_stable_func_id_for_eval",
        "AC6: stub provides weak fallback for lookup_for_eval",
    )
    must_present(
        BRIDGE_STUB,
        "aura_clear_stable_func_id_map_for_eval",
        "AC6: stub provides weak fallback for clear_for_eval",
    )

    # #2550 surface preserved (additive, not replaced).
    if hh_text and "Issue #1930 / #2550" not in hh_text:
        failures.append("AC6: bridge.h missing #2550 surface (additive)")
    if cpp_text and "aura_get_or_preserve_stable_func_id_locked" in cpp_text:
        # Old internal helper name should be GONE (replaced by _for_eval variant).
        failures.append(
            "AC6: cpp still references old preserve_stable_func_id_locked "
            "(replaced by preserve_stable_func_id_for_eval_locked)"
        )

    # AC6: test file coverage — #2670 ACs present + main() invokes them.
    test_text = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    for ac_fn in (
        "ac2670_distinct_sids_per_eval",
        "ac2670_single_workspace_unchanged",
        "ac2670_named_set_name_isolates",
        "ac2670_clear_for_eval_isolates",
        "ac2670_query_counters_advance",
        "ac2670_schema_and_source",
    ):
        if ac_fn not in test_text:
            failures.append(f"AC6: test missing {ac_fn} function")
    if "run_test_named_closure_stable_id_at_create" in test_text:
        for ac_fn in (
            "ac2670_distinct_sids_per_eval",
            "ac2670_single_workspace_unchanged",
            "ac2670_named_set_name_isolates",
            "ac2670_clear_for_eval_isolates",
            "ac2670_query_counters_advance",
            "ac2670_schema_and_source",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: main() does not call {ac_fn}()")

    # AC6: build.py wiring.
    build_text = _read("build.py")
    if "check_stable_func_id_eval_namespace_coverage" not in build_text:
        failures.append("AC6: build.py does not reference check_stable_func_id_eval_namespace_coverage linter")
    if "cmd_stable_func_id_eval_namespace_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_stable_func_id_eval_namespace_coverage function")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if strict:
            return 1
        print(
            f"\nNON-STRICT: {len(failures)} issue(s) above (--strict to enforce)",
            file=sys.stderr,
        )
        return 0

    print(
        "OK: all #2670 ACs satisfied (stable_func_id map namespaced by eval_owner — "
        "distinct sids per eval, single-workspace legacy preserved, "
        "clear_for_eval isolation, query counters additive, "
        "additive to #1930/#2550)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
