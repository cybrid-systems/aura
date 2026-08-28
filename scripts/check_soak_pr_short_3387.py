#!/usr/bin/env python3
# scripts/check_soak_pr_short_3387.py -- Issue #3387 source-cite gate.
#
# Verifies the PR-level short soak wiring (CI/runtime path):
#
#  AC1: The three target suites (`test_restamp_budget_hard_gate`,
#       `test_hold_budget_synthetic_yield_injection`,
#       `test_steal_safety_production_residual_zero`) carry the
#       `soak-pr-short` CTest label — gate runs them on PRs that touch
#       the runtime / Guard / steal / restamp paths.
#  AC2: Path filter — unrelated PRs (no diff touch on the listed
#       surfaces) pay 0 cost. Verified via the path-pattern list +
#       `_paths_touch_soak_pr_short` / `_collect_diff_paths` helpers in
#       build.py.
#  AC3: Soft / unlatched fixtures inside those suites stay metric-only
#       (existing AC — preserved, not modified). The label wires the
#       existing fail-closed assertions; no new soak harness.
#  AC4: No new soak binary; no docs/design/3387-* (per MEMORY 2026-07-19);
#      no tests/issues/test_issue_3387.cpp (#81967).
#  AC5: Nightly full chaos matrix (#2722 lineage) stays as-is — the
#       change adds the PR admission gate, does not modify nightly.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "CMakeLists.txt",
    "build.py",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: CMakeLists.txt sets the `soak-pr-short` label on all three target
    # suites. Each label line is anchored to its test name so the regex
    # does not accidentally match a different suite's label call.
    (
        "CMakeLists.txt",
        r"set_tests_properties\(test_restamp_budget_hard_gate_verification\s+PROPERTIES\s+LABELS\s+\"issue;soak-pr-short\"\)",
        "3387 AC1: CMakeLists.txt labels test_restamp_budget_hard_gate_verification as soak-pr-short",
    ),
    (
        "CMakeLists.txt",
        r"set_tests_properties\(test_hold_budget_synthetic_yield_injection_verification\s+PROPERTIES\s+LABELS\s+\"issue;soak-pr-short\"\)",
        "3387 AC1: CMakeLists.txt labels test_hold_budget_synthetic_yield_injection_verification as soak-pr-short",
    ),
    (
        "CMakeLists.txt",
        r"set_tests_properties\(test_steal_safety_production_residual_zero_verification\s+PROPERTIES\s+LABELS\s+\"issue;soak-pr-short\"\)",
        "3387 AC1: CMakeLists.txt labels test_steal_safety_production_residual_zero_verification as soak-pr-short",
    ),
    # AC2: build.py carries the path-filter + ctest invocation.
    (
        "build.py",
        r"def\s+cmd_soak_pr_short_3387_coverage\(\)\s*:\s*\n\s*\"\"\"[\s\S]{0,200}Issue\s+#3387",
        "3387 AC2: build.py declares cmd_soak_pr_short_3387_coverage + cites #3387",
    ),
    (
        "build.py",
        r"_SOAK_PR_SHORT_PATH_PATTERNS\s*=\s*\(\s*\"src/serve/steal_safety\"",
        "3387 AC2: build.py path filter includes src/serve/steal_safety",
    ),
    (
        "build.py",
        r"_SOAK_PR_SHORT_PATH_PATTERNS\s*=\s*\([\s\S]*\"src/serve/fiber\"",
        "3387 AC2: build.py path filter includes src/serve/fiber",
    ),
    (
        "build.py",
        r"_SOAK_PR_SHORT_PATH_PATTERNS\s*=\s*\([\s\S]*\"src/compiler/evaluator_mutation_boundary\.cpp\"",
        "3387 AC2: build.py path filter includes evaluator_mutation_boundary.cpp",
    ),
    (
        "build.py",
        r"_SOAK_PR_SHORT_PATH_PATTERNS\s*=\s*\([\s\S]*\"src/compiler/evaluator_fiber_mutation\.cpp\"",
        "3387 AC2: build.py path filter includes evaluator_fiber_mutation.cpp",
    ),
    (
        "build.py",
        r"_SOAK_PR_SHORT_PATH_PATTERNS\s*=\s*\([\s\S]*\"src/compiler/evaluator_security\.cpp\"",
        "3387 AC2: build.py path filter includes evaluator_security.cpp",
    ),
    (
        "build.py",
        r"_SOAK_PR_SHORT_PATH_PATTERNS\s*=\s*\([\s\S]*\"src/core/flatast_restamp\.hh\"",
        "3387 AC2: build.py path filter includes src/core/flatast_restamp.hh",
    ),
    (
        "build.py",
        r"def\s+_paths_touch_soak_pr_short\(diff_paths:\s*list\[str\]\)\s*->\s*bool",
        "3387 AC2: build.py defines _paths_touch_soak_pr_short helper",
    ),
    (
        "build.py",
        r"def\s+_collect_diff_paths\(\)\s*->\s*list\[str\]",
        "3387 AC2: build.py defines _collect_diff_paths helper (git diff best-effort)",
    ),
    (
        "build.py",
        r"ctest[\s\S]{0,80}-L[\s\S]{0,40}soak-pr-short[\s\S]{0,80}--timeout",
        "3387 AC1: ctest invocation uses -L soak-pr-short --timeout",
    ),
    # AC5: nightly full chaos gate (cmd_chaos_pr_hard_fail_gate / cmd_chaos_hard_release_blocker) unchanged — pattern still present.
    (
        "build.py",
        r"def\s+cmd_chaos_pr_hard_fail_gate\(\)",
        "3387 AC5: nightly chaos PR hard-fail gate preserved",
    ),
    # AC1/AC4: linter itself cites the issue + no new tests/issues file.
)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _resolve(rel_path: str) -> Path:
    return REPO_ROOT / rel_path


def _check_pattern(rel_path: str, regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = _resolve(rel_path)
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if not re.search(regex, text, re.MULTILINE) and strict:
        failures.append(f"{rel_path}: missing required pattern: {regex!r}")
    return failures


def _check_no_design_doc(strict: bool) -> list[str]:
    """AC4: no docs/design/3387-* markdown (per MEMORY 2026-07-19)."""
    failures: list[str] = []
    docs_dir = REPO_ROOT / "docs" / "design"
    if not docs_dir.exists():
        return failures
    matches = sorted(docs_dir.glob("3387-*.md"))
    if matches and strict:
        names = ", ".join(m.name for m in matches)
        failures.append(
            f"docs/design/3387-*.md exists ({names}); "
            "MEMORY 2026-07-19 forbids — close comment + commit carry rationale"
        )
    return failures


def _check_no_test_issue_file(strict: bool) -> list[str]:
    """AC4: no tests/issues/test_issue_3387.cpp (#81967)."""
    failures: list[str] = []
    p = REPO_ROOT / "tests" / "issues" / "test_issue_3387.cpp"
    if p.exists() and strict:
        failures.append(f"{p}: tests/issues/test_issue_3387.cpp exists; #81967 forbids — wire existing suite")
    return failures


def run_checks(*, strict: bool) -> list[str]:
    failures: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        failures.extend(_check_pattern(rel, rx, strict=strict))
    failures.extend(_check_no_design_doc(strict))
    failures.extend(_check_no_test_issue_file(strict))
    return failures


def _self_test() -> int:
    """Validate the linter regexes against a fixture approximating the post-fix files."""
    fixture_cmake = """
    aura_add_issue_test(test_restamp_budget_hard_gate)
    aura_issue_test_link_light(test_restamp_budget_hard_gate)
    # Issue #3387 — PR-level short soak label (covers torn-probe / export face split).
    set_tests_properties(test_restamp_budget_hard_gate_verification PROPERTIES LABELS "issue;soak-pr-short")
    aura_add_issue_test(test_hold_budget_synthetic_yield_injection)
    aura_issue_test_link_light(test_hold_budget_synthetic_yield_injection)
    add_dependencies(all_test_issue_targets test_hold_budget_synthetic_yield_injection)
    # Issue #3387 — PR-level short soak label (covers hold-after-cancel > 2×SLO).
    set_tests_properties(test_hold_budget_synthetic_yield_injection_verification PROPERTIES LABELS "issue;soak-pr-short")
    aura_add_issue_test(test_steal_safety_production_residual_zero)
    aura_issue_test_link_light(test_steal_safety_production_residual_zero)
    add_dependencies(all_test_issue_targets test_steal_safety_production_residual_zero)
    # Issue #3387 — PR-level short soak label (covers steal residual-zero + sticky-fail).
    set_tests_properties(test_steal_safety_production_residual_zero_verification PROPERTIES LABELS "issue;soak-pr-short")
    """
    fixture_build = '''
    _SOAK_PR_SHORT_PATH_PATTERNS = (
        "src/serve/steal_safety",
        "src/serve/fiber",
        "src/compiler/evaluator_mutation_boundary.cpp",
        "src/compiler/evaluator_fiber_mutation.cpp",
        "src/compiler/evaluator_security.cpp",
        "src/core/flatast_restamp.hh",
    )

    def _paths_touch_soak_pr_short(diff_paths):
        for p in diff_paths:
            for needle in _SOAK_PR_SHORT_PATH_PATTERNS:
                if p.startswith(needle) or p == needle or needle in p:
                    return True
        return False

    def _collect_diff_paths():
        r = subprocess.run(["git", "diff", "--name-only", "HEAD~1", "HEAD"], cwd=ROOT, text=True, capture_output=True)
        if r.returncode == 0 and r.stdout:
            return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
        return []

    def cmd_soak_pr_short_3387_coverage():
        """Issue #3387: PR-level short soak fail-closed on hold-after-cancel +
        steal residual-zero + torn probe (I1/I3/I6 gate).
        """
        paths = _collect_diff_paths()
        if not _paths_touch_soak_pr_short(paths):
            info("soak-pr-short (#3387): path filter miss — skipping")
            return 0
        r = subprocess.run(["ctest", "-L", "soak-pr-short", "--timeout", "60", "--output-on-failure"], cwd=str(ctest_dir))
        if r.returncode != 0:
            fail(...)
            return 1
        ok("soak-pr-short (#3387) clean")
        return 0

    def cmd_chaos_pr_hard_fail_gate():
        ...
    '''
    fails: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        if "CMakeLists.txt" in rel:
            text = fixture_cmake
        elif "build.py" in rel:
            text = fixture_build
        else:
            text = ""
        if not re.search(rx, text, re.MULTILINE):
            fails.append(f"self-test: {rel}: missing pattern: {rx!r}")
    return 0 if not fails else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3387 soak-pr-short path-filter + ctest label source-cite gate."
    )
    parser.add_argument("--strict", action="store_true", help="Fail on missing patterns (default: observe-only)")
    parser.add_argument("--self-test", action="store_true", help="Run self-test against fixture text")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    strict = bool(args.strict)
    failures = run_checks(strict=strict)
    if failures:
        print("Issue #3387 source-cite gate FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("Issue #3387 source-cite gate OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
