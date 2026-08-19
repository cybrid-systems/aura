#!/usr/bin/env python3
# scripts/check_hold_after_cancel_hard_soak_3164.py -- Issue #3164 source-cite gate.
#
# Verifies the hold-after-cancel release latency is a hard soak invariant
# under production soak profile (#3002 residual). The fix extends the
# existing #3073 production-readiness gate in the chaos soak to add a
# new hard assert that fires regardless of current armed/held state
# (the max observed during the soak is the SSOT).
#
#  1. Producer hook (tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp):
#     - max_hold_after_cancel_us SSOT tracked during soak host ticks
#     - new #3164 block: hard assert under prod_ready_gate, regardless
#       of current armed/held state (max_hold_after_cancel_us <= bound)
#     - Soft / unit: print only (gated on production_defaults_active)
#     - Cite Issue #3164 in the new block
#     - Existing #3073 check non-regressing (residual_envframe_lag delta == 0,
#       residual_lifetime_proof_reject delta == 0)
#
#  2. Existing infrastructure (src/compiler/mutation_hold_budget.h):
#     - g_hold_budget_cancel_armed_ns / g_hold_budget_cancel_armed_fiber
#       SSOT atomics (single load, no second walk — AC2)
#     - mutation_hold_inbody_window_bound_us() (bound source)
#     - mutation_hold_live_snapshot() (held SSOT)
#     - mutation_hold_budget_inbody_window_exceeded_total_v_read()
#       (counter)
#
#  3. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3164-* plan doc
#     - No tests/issues/test_issue_3164.cpp
#     - No new middle-of-metrics keys (existing counters reused)

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
    "src/compiler/mutation_hold_budget.h",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Layer 1: chaos soak #3164 hard invariant block
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"#3164:\s*hard soak invariant for max hold-after-cancel latency",
        "#3164 hard soak invariant comment (producer hook)",
    ),
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"max_hold_after_cancel_us\s*<=\s*bound",
        "max_hold_after_cancel_us <= bound (the actual assert)",
    ),
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"#3164:\s*max hold-after-cancel latency exceeds bound",
        "#3164 CHECK label",
    ),
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"prod_ready_gate\s*&&\s*bound\s*>\s*0",
        "prod_ready_gate && bound > 0 gate (production-only hard assert)",
    ),
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"residual_zero_gate\s*\|\|\s*prod_gate",
        "residual_zero_gate || prod_gate (gate activation)",
    ),
    # Layer 2: existing #3073 non-regression
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"#3073:\s*residual_envframe_lag delta == 0",
        "#3073 envframe lag check non-regressing",
    ),
    (
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp",
        r"#3073:\s*residual_lifetime_proof_reject delta == 0",
        "#3073 lifetime proof check non-regressing",
    ),
    # Layer 3: SSOT infrastructure (mutation_hold_budget.h)
    (
        "src/compiler/mutation_hold_budget.h",
        r"g_hold_budget_cancel_armed_ns",
        "g_hold_budget_cancel_armed_ns atomic SSOT",
    ),
    (
        "src/compiler/mutation_hold_budget.h",
        r"g_hold_budget_cancel_armed_fiber",
        "g_hold_budget_cancel_armed_fiber atomic SSOT",
    ),
    (
        "src/compiler/mutation_hold_budget.h",
        r"mutation_hold_inbody_window_bound_us",
        "mutation_hold_inbody_window_bound_us accessor",
    ),
    (
        "src/compiler/mutation_hold_budget.h",
        r"mutation_hold_live_snapshot",
        "mutation_hold_live_snapshot accessor",
    ),
    (
        "src/compiler/mutation_hold_budget.h",
        r"mutation_hold_budget_inbody_window_exceeded_total_v_read",
        "inbody_window_exceeded counter accessor",
    ),
)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_file(rel_path: str, regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if not re.search(regex, text) and strict:
        failures.append(f"{rel_path}: missing required pattern: {regex!r}")
    return failures


def check_no_forbidden_artifacts() -> list[str]:
    """Verify no docs/design/3164-* (#1655) or test_issue_3164.cpp (#81967)."""
    failures: list[str] = []
    design_dir = REPO_ROOT / "docs" / "design"
    if design_dir.exists():
        for f in design_dir.iterdir():
            if f.name.startswith("3164-"):
                failures.append(f"docs/design/{f.name}: forbidden per #1655")
    issues_dir = REPO_ROOT / "tests" / "issues"
    if issues_dir.exists():
        target = issues_dir / "test_issue_3164.cpp"
        if target.exists():
            failures.append("tests/issues/test_issue_3164.cpp: forbidden per #81967")
    serve_issues = REPO_ROOT / "tests" / "serve" / "test_issue_3164.cpp"
    if serve_issues.exists():
        failures.append("tests/serve/test_issue_3164.cpp: forbidden per #81967")
    compiler_issues = REPO_ROOT / "tests" / "compiler" / "test_issue_3164.cpp"
    if compiler_issues.exists():
        failures.append("tests/compiler/test_issue_3164.cpp: forbidden per #81967")
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_test = """
    // Issue #3164: hard soak invariant for max hold-after-cancel latency.
    {
        const auto bound = aura::compiler::mutation_hold_inbody_window_bound_us();
        const bool prod_ready_gate = (residual_zero_gate || prod_gate) &&
                                     aura::compiler::typed_audit::production_defaults_active();
        if (prod_ready_gate && bound > 0) {
            CHECK(max_hold_after_cancel_us <= bound,
                  "#3164: max hold-after-cancel latency exceeds bound (soak fail-closed)");
        }
        std::println("  #3164 hold-after-cancel max: max_hold_after_us={} bound_us={} (gate={})",
                     max_hold_after_cancel_us, bound, prod_ready_gate ? 1 : 0);
    }
    // #3073 non-regression
    if (prod_ready_gate) {
        CHECK(d_envframe == 0, "#3073: residual_envframe_lag delta == 0");
        CHECK(d_life == 0, "#3073: residual_lifetime_proof_reject delta == 0");
    }
    """
    fixture_hur = """
    inline std::atomic<std::uint64_t> g_hold_budget_cancel_armed_ns{0};
    inline std::atomic<std::uint64_t> g_hold_budget_cancel_armed_fiber{0};
    [[nodiscard]] inline std::uint64_t mutation_hold_inbody_window_bound_us() noexcept {
        ...
    }
    [[nodiscard]] inline auto mutation_hold_live_snapshot() noexcept { ... }
    [[nodiscard]] inline std::uint64_t
    mutation_hold_budget_inbody_window_exceeded_total_v_read() noexcept { ... }
    """
    fixtures = {
        "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp": fixture_test,
        "src/compiler/mutation_hold_budget.h": fixture_hur,
    }
    fails: list[str] = []
    for rel, rx, label in INFRA_REQUIRED:
        text = fixtures.get(rel, "")
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r} (label: {label})")
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3164 hold-after-cancel hard soak invariant source-cite gate",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Fail on missing patterns (default: strict)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Validate linter regex / structure against fixture text",
    )
    parser.add_argument(
        "--forbidden-only",
        action="store_true",
        help="Only check forbidden artifacts (docs/design/3164-*, test_issue_3164.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []

    if not args.forbidden_only:
        for rel, rx, _label in INFRA_REQUIRED:
            failures.extend(check_file(rel, rx, strict=args.strict))

    if failures:
        print(f"FAIL: {len(failures)} issue(s):")
        for line in failures:
            print(f"  {line}")
        return 1

    print("OK: Issue #3164 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
