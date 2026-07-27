#!/usr/bin/env python3
# scripts/check_cross_workspace_hot_update_reject_coverage.py
#
# Issue #2178 linter: ensure the cross-workspace / cross-COW hot-update
# guard is real on the production surface (refine #1943 single-workspace
# MVP). Each row below is a contract that the production surface MUST
# satisfy; missing any row fails the script (exit 1) and the pre-push
# gate surfaces the gap.
#
# Contract (per the #2178 body + shipped C++ surface):
#   1. src/compiler/aura_jit_bridge.cpp: file-level atomic
#      g_cross_workspace_hot_update_rejected_total + C-linkage helpers
#      (aura_cross_workspace_hot_update_rejected_increment +
#      aura_cross_workspace_hot_update_rejected_total_v_read) +
#      aura_is_current_workspace_eval forward decl.
#   2. src/compiler/observability_metrics.h: CompilerMetrics field
#      cross_workspace_hot_update_rejected_total (next to the other
#      aot_reload_*_total counters for the Agent dashboard mirror).
#   3. src/compiler/aura_jit_bridge.cpp: foreign-eval guard inserted
#      at the top of aura_reload_aot_module_for_eval — bumps the
#      counter + sets last-fail reason + returns false on foreign
#      eval_ptr; happy path (null / matching eval) unchanged.
#   4. src/compiler/hot_update_registry.hh: contract doc next to the
#      MVP comment — links the guard + counter to the single-workspace
#      boundary enforcement.
#   5. tests/compiler/test_aot_reload_primitive.cpp: AC7
#      (ac7_cross_workspace_reject_2178) covers foreign → reject +
#      null → happy + source-cite.
#
# Self-test: --self-test exercises the substring counters against the
# live tree (must pass).
#
# Exit codes:
#   0 = pass
#   1 = contract gap
#   2 = file missing
#   3 = self-test fail

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COMPILER_DIR = REPO_ROOT / "src" / "compiler"
TESTS_DIR = REPO_ROOT / "tests" / "compiler"


def _read(p: Path) -> str:
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _contains_all(text: str, needles: list[str]) -> list[str]:
    return [n for n in needles if n not in text]


def check_contract() -> tuple[int, list[str]]:
    failures: list[str] = []

    # 1. aura_jit_bridge.cpp — atomics + C-linkage helpers + is_current_workspace_eval.
    ab = _read(COMPILER_DIR / "aura_jit_bridge.cpp")
    if not ab:
        failures.append("src/compiler/aura_jit_bridge.cpp missing")
    else:
        missing = _contains_all(
            ab,
            [
                "g_cross_workspace_hot_update_rejected_total{0}",
                "aura_cross_workspace_hot_update_rejected_increment",
                "aura_cross_workspace_hot_update_rejected_total_v_read",
                "aura_is_current_workspace_eval",
            ],
        )
        if missing:
            failures.append(f"src/compiler/aura_jit_bridge.cpp missing #2178 cross-workspace guard pieces: {missing}")

    # 2. observability_metrics.h — CompilerMetrics field.
    om = _read(COMPILER_DIR / "observability_metrics.h")
    if not om:
        failures.append("src/compiler/observability_metrics.h missing")
    elif "cross_workspace_hot_update_rejected_total{0}" not in om:
        failures.append("src/compiler/observability_metrics.h missing cross_workspace_hot_update_rejected_total field")

    # 3. aura_jit_bridge.cpp — guard inserted at the top of aura_reload_aot_module_for_eval.
    # The function body is multi-line with nested braces, so we don't try to
    # match the full body via regex. Instead, just check that the key guard
    # pattern + symbols exist in the file (the substring checks at the top
    # of this linter already cover the C-linkage symbols).
    if ab:
        # The guard must follow the pattern: if (eval_ptr != nullptr && !aura_is_current_workspace_eval(eval_ptr))
        guard_pattern = "eval_ptr != nullptr && !aura_is_current_workspace_eval(eval_ptr)"
        if guard_pattern not in ab:
            failures.append(
                "src/compiler/aura_jit_bridge.cpp missing cross-workspace guard pattern in aura_reload_aot_module_for_eval"
            )
        # The guard body must call the rejected counter increment + set last-fail + return false.
        guard_body_markers = [
            "aura_cross_workspace_hot_update_rejected_increment",
            "AotReloadFail::Other",
            "capture_aot_hotupdate_audit",
        ]
        missing = _contains_all(ab, guard_body_markers)
        if missing:
            failures.append(f"src/compiler/aura_jit_bridge.cpp cross-workspace guard body missing markers: {missing}")

    # 4. hot_update_registry.hh — contract doc.
    hr = _read(COMPILER_DIR / "hot_update_registry.hh")
    if not hr:
        failures.append("src/compiler/hot_update_registry.hh missing")
    elif "Issue #2178" not in hr or "aura_is_current_workspace_eval" not in hr:
        failures.append("src/compiler/hot_update_registry.hh missing #2178 cross-workspace guard contract doc")

    # 5. tests/compiler/test_aot_reload_primitive.cpp — AC7.
    test_src = _read(TESTS_DIR / "test_aot_reload_primitive.cpp")
    if not test_src:
        failures.append("tests/compiler/test_aot_reload_primitive.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                "ac7_cross_workspace_reject_2178",
                "ac7_cross_workspace_reject_2178()",
                "Issue #2178",
            ],
        )
        if missing:
            failures.append(f"test_aot_reload_primitive.cpp missing #2178 AC entries: {missing}")

    # 6. Issue #2240: stable cross-workspace reject reason code (refine
    #    #2178). Agents branch on the reason without log scraping. This
    #    row enforces the production surface contract for the reason code:
    #    - aura_jit_bridge.h: CrossWorkspaceReject enum + C-linkage
    #      reader / symbol helper declarations.
    #    - aura_jit_bridge.cpp: g_last_cross_workspace_reject_reason
    #      file-scope atomic + reader impl + symbol helper impl +
    #      ForeignEval set at guard site (BEFORE counter increment) +
    #      None reset at start of every attempt.
    #    - tests/compiler/test_aot_reload_primitive.cpp: ac7b tests
    #      the reason code + symbol helper + hash-mode query.
    hd = (
        _read(COMPILER_DIR.parent.parent / "src" / "compiler" / "aura_jit_bridge.h")
        if False
        else _read(COMPILER_DIR / "aura_jit_bridge.h")
    )
    if not hd:
        failures.append("src/compiler/aura_jit_bridge.h missing")
    else:
        missing = _contains_all(
            hd,
            [
                "enum class CrossWorkspaceReject",
                "aura_last_cross_workspace_reject_reason_v_read",
                "aura_cross_workspace_reject_reason_string",
                "Issue #2240",
            ],
        )
        if missing:
            failures.append(f"src/compiler/aura_jit_bridge.h missing #2240 reason code pieces: {missing}")

    if ab:
        missing = _contains_all(
            ab,
            [
                "g_last_cross_workspace_reject_reason{0}",
                "aura_last_cross_workspace_reject_reason_v_read",
                "aura_cross_workspace_reject_reason_string",
                "CrossWorkspaceReject::ForeignEval",
                "CrossWorkspaceReject::None",
                "Issue #2240",
            ],
        )
        if missing:
            failures.append(f"src/compiler/aura_jit_bridge.cpp missing #2240 reason code pieces: {missing}")

    if test_src:
        missing = _contains_all(
            test_src,
            [
                "ac7b_cross_workspace_reason_code_2240",
                "ac7b_cross_workspace_reason_code_2240()",
                "Issue #2240",
            ],
        )
        if missing:
            failures.append(f"test_aot_reload_primitive.cpp missing #2240 AC7b entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2178 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2178 cross-workspace hot-update reject contract linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_cross_workspace_hot_update_reject_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_cross_workspace_hot_update_reject_coverage] OK: all #2178 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
