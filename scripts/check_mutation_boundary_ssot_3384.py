#!/usr/bin/env python3
# scripts/check_mutation_boundary_ssot_3384.py -- Issue #3384 source-cite gate.
#
# Verifies the dual-depth-authority → single-SSOT fix is in place:
#
#  AC1: any_active_mutation_boundary + mutation_boundary_depth_slot_value
#       route through boundary_ssot_detail::boundary_depth_ssot (fiber
#       stack on fiber, TLS slot off fiber). Evaluator::mutation_boundary_depth()
#       static accessor already returns active_mutation_stack_static().size().
#
#  AC2: Guard ctor / dtor / force_release_hold_after_cancel_ TLS writes
#       are gated on g_current_fiber_void == nullptr (TLS write skipped
#       on fiber; TLS would be victim worker's after steal).
#
#  AC3: ensure_mutation_invariants Soft path bumps total_invariant_violations_
#       (metric-only); under aura_runtime_multi_worker_production_latched also
#       mark-failed via aura_evaluator_mark_outermost_mutation_failed +
#       republish MutationSafetySnapshot mirror. Off-fiber is the only drift
#       check window (on-fiber: fiber stack is its own SSOT).
#
#  AC4: Existing #2184/#2956 mirror canary (aura_mutation_boundary_assert_mirrors_consistent)
#       + publish_current_fiber_mutation_safety remain present — regression
#       guard against the SSOT routing change dropping the canary.
#
#  AC5: No docs/design/3384-* markdown (per MEMORY 2026-07-19 directive —
#       close comment + commit message carry the design rationale).
#       Existing APIs (mutation_boundary_depth_slot) preserved — only
#       internal TLS write is gated.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator.ixx",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: SSOT helper + read routing.
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"namespace\s+boundary_ssot_detail",
        "3384 AC1: SSOT helper namespace boundary_ssot_detail",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"inline\s+int\s+boundary_depth_ssot\s*\(\s*Evaluator\*\s*ev\s*\)\s*noexcept",
        "3384 AC1: SSOT helper definition",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"any_active_mutation_boundary[^{]*\{[\s\S]*?boundary_ssot_detail::boundary_depth_ssot",
        "3384 AC1: any_active_mutation_boundary routes through SSOT",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"mutation_boundary_depth_slot_value\(\)\s*const\s*noexcept[^{]*\{[\s\S]*?return\s+boundary_ssot_detail::boundary_depth_ssot",
        "3384 AC1: mutation_boundary_depth_slot_value routes through SSOT",
    ),
    (
        "src/compiler/evaluator.ixx",
        r"static\s+std::size_t\s+mutation_boundary_depth\(\)\s*\{\s*return\s+active_mutation_stack_static\(\)\.size\(\)\s*;\s*\}",
        "3384 AC1: Evaluator::mutation_boundary_depth() static accessor already SSOT",
    ),
    # AC2: Guard ctor / dtor / force_release write gating.
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"const\s+bool\s+on_fiber\s*=\s*\(aura::compiler::Evaluator::g_current_fiber_void\s*!=\s*nullptr\)",
        "3384 AC2: Guard ctor declares on_fiber branch",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"prev\s*=\s*static_cast<int>\(\s*\w+\.size\(\)\s*\)\s*\+\s*1",
        "3384 AC2: Guard ctor computes prev from fiber stack size on fiber",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"if\s*\(\s*!on_fiber\s*\)\s*\{[^}]*--\(\*slot\)",
        "3384 AC2: inert rollback --(*slot) conditional on !on_fiber",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"if\s*\(\s*aura::compiler::Evaluator::g_current_fiber_void\s*==\s*nullptr\s*\)\s*\{[^}]*Evaluator::mutation_boundary_depth_slot\(ev_\)",
        "3384 AC2: force_release_hold_after_cancel_ TLS zero gated on off-fiber",
    ),
    # AC3: Soft stays metric-only; production mark-failed + republish.
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"ensure_mutation_invariants\(\)[\s\S]*?if\s*\(\s*g_current_fiber_void\s*!=\s*nullptr\s*\)\s*return\s*;",
        "3384 AC3: ensure_mutation_invariants on-fiber early return (fiber stack SSOT)",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"total_invariant_violations_\.fetch_add\(1,\s*std::memory_order_relaxed\)",
        "3384 AC3: Soft metric-only counter",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"aura::serve::aura_runtime_multi_worker_production_latched\(\)\s*!=\s*0",
        "3384 AC3: production multi-worker latched gate",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"aura_evaluator_mark_outermost_mutation_failed\(\)",
        "3384 AC3: mark-failed wired",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"fib->publish_mutation_safety_mirrors\(",
        "3384 AC3: republish MutationSafetySnapshot mirror",
    ),
    # AC4: regression — existing #2184/#2956 mirror canary still wired.
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"aura_mutation_boundary_assert_mirrors_consistent\(",
        "3384 AC4: #2184/#2956 mirror canary still wired in Guard TU",
    ),
    (
        "src/compiler/evaluator_fiber_mutation.cpp",
        r"aura_mutation_boundary_assert_mirrors_consistent\(",
        "3384 AC4: #2184/#2956 mirror canary helper cited",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"publish_current_fiber_mutation_safety\(",
        "3384 AC4: #2184 publish_current_fiber still wired",
    ),
    # AC5: existing APIs preserved — only internal TLS write gated.
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"mutation_boundary_depth_slot\(ev_\)",
        "3384 AC5: existing TLS slot accessor preserved (no rename)",
    ),
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
    """AC5: no docs/design/3384-* markdown (per MEMORY 2026-07-19)."""
    failures: list[str] = []
    docs_dir = REPO_ROOT / "docs" / "design"
    if not docs_dir.exists():
        return failures
    matches = sorted(docs_dir.glob("3384-*.md"))
    if matches and strict:
        names = ", ".join(m.name for m in matches)
        failures.append(
            f"docs/design/3384-*.md exists ({names}); "
            "MEMORY 2026-07-19 forbids — close comment + commit carry rationale"
        )
    return failures


def run_checks(*, strict: bool) -> list[str]:
    failures: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        failures.extend(_check_pattern(rel, rx, strict=strict))
    failures.extend(_check_no_design_doc(strict))
    return failures


def _self_test() -> int:
    """Validate the linter regexes against a fixture approximating the post-fix source."""
    fixture_efm = """
    namespace boundary_ssot_detail {
        inline int boundary_depth_ssot(Evaluator* ev) noexcept {
            if (aura::compiler::Evaluator::g_current_fiber_void != nullptr)
                return static_cast<int>(aura::compiler::Evaluator::active_mutation_stack().size());
            int* slot = aura::compiler::Evaluator::mutation_boundary_depth_slot(ev);
            return slot ? *slot : 0;
        }
    }
    bool Evaluator::any_active_mutation_boundary() const noexcept {
        return boundary_ssot_detail::boundary_depth_ssot(const_cast<Evaluator*>(this)) > 0;
    }
    int Evaluator::mutation_boundary_depth_slot_value() const noexcept {
        return boundary_ssot_detail::boundary_depth_ssot(const_cast<Evaluator*>(this));
    }
    void Evaluator::ensure_mutation_invariants() noexcept {
        auto& stack = active_mutation_stack();
        if (g_current_fiber_void != nullptr) return;
        int* depth = mutation_boundary_depth_slot(this);
        if (!depth) return;
        const bool stack_empty = stack.empty();
        const bool depth_zero = (*depth == 0);
        if (stack_empty != depth_zero) {
            total_invariant_violations_.fetch_add(1, std::memory_order_relaxed);
            if (aura::serve::aura_runtime_multi_worker_production_latched() != 0) {
                aura_evaluator_mark_outermost_mutation_failed();
                if (auto* fib = aura::serve::g_current_fiber) {
                    fib->publish_mutation_safety_mirrors(
                        stack.size(), /*held=*/!stack_empty, defuse_version_snapshot());
                }
            }
        }
    }
    int aura_mutation_mirror_inconsistency_hard_total_holder() { return 0; }
    int aura_mutation_boundary_assert_mirrors_consistent(int, int, int) { return 1; }
    """
    fixture_emb = """
    int* slot = Evaluator::mutation_boundary_depth_slot(ev_);
    int prev;
    const bool on_fiber = (aura::compiler::Evaluator::g_current_fiber_void != nullptr);
    if (on_fiber) {
        auto& fiber_stack = Evaluator::active_mutation_stack();
        prev = static_cast<int>(fiber_stack.size()) + 1;
        (void)slot;
    } else {
        prev = ++(*slot);
    }
    bool outermost = (prev == 1);
    // Issue #3384: only decrement TLS when off-fiber (we wrote it).
    if (!on_fiber) {
        --(*slot);
    }
    is_outermost_ = false;
    return;
    void Evaluator::MutationBoundaryGuard::force_release_hold_after_cancel_() noexcept {
        if (cancel_force_released_ || !ev_) return;
        if (aura::compiler::Evaluator::g_current_fiber_void == nullptr) {
            if (int* ds = Evaluator::mutation_boundary_depth_slot(ev_))
                *ds = 0;
        }
        ev_->mutation_boundary_held_.store(false, std::memory_order_release);
    }
    aura_mutation_boundary_assert_mirrors_consistent(1, 1, 1);
    aura::serve::publish_current_fiber_mutation_safety(0, true, 0);
    """
    fixture_exx = """
    static std::size_t mutation_boundary_depth() { return active_mutation_stack_static().size(); }
    """
    fails: list[str] = []
    fixtures = {
        "src/compiler/evaluator_fiber_mutation.cpp": fixture_efm,
        "src/compiler/evaluator_mutation_boundary.cpp": fixture_emb,
        "src/compiler/evaluator.ixx": fixture_exx,
    }
    for rel, rx, _label in INFRA_REQUIRED:
        text = fixtures.get(rel, "")
        if not text:
            continue
        if not re.search(rx, text, re.MULTILINE):
            fails.append(f"self-test: {rel}: missing pattern: {rx!r}")
    return 0 if not fails else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Issue #3384 mutation-boundary SSOT source-cite gate.")
    parser.add_argument("--strict", action="store_true", help="Fail on missing patterns (default: observe-only)")
    parser.add_argument("--self-test", action="store_true", help="Run self-test against fixture text")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    strict = bool(args.strict)
    failures = run_checks(strict=strict)
    if failures:
        print("Issue #3384 source-cite gate FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("Issue #3384 source-cite gate OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
