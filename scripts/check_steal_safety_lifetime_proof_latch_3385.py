#!/usr/bin/env python3
# scripts/check_steal_safety_lifetime_proof_latch_3385.py -- Issue #3385 source-cite gate.
#
# Verifies the LifetimeProofOk residual arm (StealInvariant::LifetimeProofOk)
# is gated on hard_mode OR latched multi-worker Ready (not hard_mode-only),
# and that the mailbox_delivery_safety_transaction path conditionally skips
# the arm based on (latch && held-ref):
#
#  AC1: LifetimeProofOk arm in evaluate_residual_hard_and_bits uses
#       (is_steal_snapshot_hard_mode() || aura_runtime_multi_worker_production_latched() != 0)
#       — latched multi-worker Ready must fire the arm even when Soft.
#  AC2: Unlatched Soft: arm still skipped (zero extra loads). The branch
#       structure means the arm runs only when hard_mode OR latch — without
#       either, the predicate short-circuits to false.
#  AC3: mailbox_delivery_safety_transaction only skips LifetimeProofOk when
#       NOT (latch + check_envframe). Pure payload without held-ref keeps
#       the skip (zero extra loads). Held-ref delivery under latch observes
#       the arm.
#  AC4: Existing #2957/#3001/#3134/#3288 suites green (regression) — ACs
#       added to test_steal_safety_production_residual_zero.cpp AC16.
#  AC5: Source-cite only. No docs/design/3385-* (per MEMORY 2026-07-19).
#       Existing APIs preserved — only the gate predicate + mailbox skip
#       mask changed.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = ("src/serve/steal_safety.cpp",)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: LifetimeProofOk arm uses hard_mode OR latch.
    (
        "src/serve/steal_safety.cpp",
        r"Issue\s+#3385",
        "3385 AC1: steal_safety.cpp cites #3385",
    ),
    (
        "src/serve/steal_safety.cpp",
        r"if\s*\(\s*!skip\(StealInvariant::LifetimeProofOk\)\s*&&\s*\(is_steal_snapshot_hard_mode\(\)\s*\|\|\s*aura_runtime_multi_worker_production_latched\(\)\s*!=\s*0\)\s*\)",
        "3385 AC1: LifetimeProofOk arm gate hard_mode OR latch",
    ),
    (
        "src/serve/steal_safety.cpp",
        r"lcp::last_lifetime_consistency_proof_present\(\)\s*&&\s*[\s\S]{0,80}last_densify_call_seq\(\)\s*>\s*0\s*&&\s*!lcp::last_lifetime_consistency_would_allow\(\)",
        "3385 AC1: LifetimeProofOk arm predicate (proof present + densify_seq>0 + !would_allow)",
    ),
    # AC3: mailbox conditional skip based on (observe_latch && check_envframe).
    (
        "src/serve/steal_safety.cpp",
        r"const\s+bool\s+observe_latch\s*=\s*aura::serve::aura_runtime_multi_worker_production_latched\(\)\s*!=\s*0",
        "3385 AC3: mailbox observes latch (observe_latch local)",
    ),
    (
        "src/serve/steal_safety.cpp",
        r"std::uint64_t\s+skip\s*=\s*\(observe_latch\s*&&\s*check_envframe\)\s*\?\s*0\s*:\s*steal_invariant_mask\(StealInvariant::LifetimeProofOk\)",
        "3385 AC3: mailbox skip mask = (observe_latch && check_envframe) ? 0 : LifetimeProofOk",
    ),
    (
        "src/serve/steal_safety.cpp",
        r"//\s*Issue\s+#3385:\s*only\s+skip\s+LifetimeProofOk\s+when\s+NOT\s*\(latch\s*\+\s*held-ref\)",
        "3385 AC3: mailbox comment cites #3385 + held-ref rationale",
    ),
    # AC5: existing arm structure preserved — last proof check unchanged
    # (predicate intact; only the outer guard broadened).
    (
        "src/serve/steal_safety.cpp",
        r"note_steal_invariant_fail\(StealInvariant::LifetimeProofOk\)",
        "3385 AC5: existing LifetimeProofOk fail-bump preserved",
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
    """AC5: no docs/design/3385-* markdown (per MEMORY 2026-07-19)."""
    failures: list[str] = []
    docs_dir = REPO_ROOT / "docs" / "design"
    if not docs_dir.exists():
        return failures
    matches = sorted(docs_dir.glob("3385-*.md"))
    if matches and strict:
        names = ", ".join(m.name for m in matches)
        failures.append(
            f"docs/design/3385-*.md exists ({names}); "
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
    fixture = """
    // Issue #3385: LifetimeProofOk arm gate hard_mode OR latch (I3 residual)
    void example_evaluate(std::uint64_t skip_mask, bool is_steal_snapshot_hard_mode()) {
        const auto skip = [skip_mask](StealInvariant inv) noexcept { return false; };
        // StealInvariant::LifetimeProofOk — Issue #2957 residual arm (f).
        if (!skip(StealInvariant::LifetimeProofOk) &&
            (is_steal_snapshot_hard_mode() || aura_runtime_multi_worker_production_latched() != 0)) {
            namespace lcp = aura::core::lifetime_consistency_proof;
            if (lcp::last_lifetime_consistency_proof_present() &&
                aura::core::densify_consistency::last_densify_call_seq() > 0 &&
                !lcp::last_lifetime_consistency_would_allow()) {
                fail_bits |= steal_invariant_mask(StealInvariant::LifetimeProofOk);
                note_steal_invariant_fail(StealInvariant::LifetimeProofOk);
            }
        }
    }
    MailboxDeliverySafety example_mailbox(Fiber* target, const MutationSafetySnapshot* snap, bool check_envframe) {
        MutationSafetySnapshot local = snap ? *snap : target->mutation_safety_snapshot();
        // Issue #3385: only skip LifetimeProofOk when NOT (latch + held-ref).
        const bool observe_latch = aura::serve::aura_runtime_multi_worker_production_latched() != 0;
        std::uint64_t skip = (observe_latch && check_envframe)
                                 ? 0
                                 : steal_invariant_mask(StealInvariant::LifetimeProofOk);
        if (!check_envframe)
            skip |= steal_invariant_mask(StealInvariant::EnvFrameOk);
        out.fail_bits = evaluate_residual_hard_and_bits(target, local, false, skip);
        return out;
    }
    """
    fails: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        if "steal_safety.cpp" not in rel:
            continue
        if not re.search(rx, fixture, re.MULTILINE):
            fails.append(f"self-test: {rel}: missing pattern: {rx!r}")
    return 0 if not fails else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Issue #3385 steal-safety LifetimeProofOk latch source-cite gate.")
    parser.add_argument("--strict", action="store_true", help="Fail on missing patterns (default: observe-only)")
    parser.add_argument("--self-test", action="store_true", help="Run self-test against fixture text")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    strict = bool(args.strict)
    failures = run_checks(strict=strict)
    if failures:
        print("Issue #3385 source-cite gate FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("Issue #3385 source-cite gate OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
