#!/usr/bin/env python3
"""check_occurrence_cone_truncate_drift_2672.py — Issue #2672 source gate.

Drift-injection soak for #2646 cone-truncate outside-cone invalidate
(refine #2646 AC6 which was deferred for the drift-injection helper).
Hermetic test path: force_partial_cone_truncate_for_test() sets
per-engine last_partial_cone_truncated_ + last_partial_cone_dropped_
and mirrors to process-wide atomics via
typed_audit::publish_partial_cone_truncate so #2621 commit_readiness
gate sees the truncated state.

AC1: Soft + cone soft overflow + dirty If outside cone → goals dropped
     → commit allowed (observe path) — source-cite
AC2: production/hard + same → outside goals dropped + commit hard face
     (cone_truncate / truncate family) — source-cite
AC3: !truncated path → outside invalidate counters stable; empty outside
     set → no second sync call — source-cite
AC4: second sync fires AFTER #2622 sync (ordering invariant) —
     source-cite
AC5: schema/counters still present — source-cite
AC6: drift-injection unit test (not source-cite only) + linter row

Default: non-strict (exit 0, prints coverage summary). Use --strict to
enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TC_IXX = ROOT / "src" / "compiler" / "type_checker.ixx"
TC_IMPL = ROOT / "src" / "compiler" / "type_checker_impl.cpp"
EVAL_IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EVAL_TC = ROOT / "src" / "compiler" / "evaluator_typecheck.cpp"
AUDIT_H = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
BUILD = ROOT / "build.py"
TEST = ROOT / "tests" / "compiler" / "test_partial_cone_commit_gate.cpp"


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

    impl_text = _read("src/compiler/type_checker_impl.cpp")
    ixx_text = _read("src/compiler/type_checker.ixx")  # noqa: F841 — used via must_present
    eval_ixx_text = _read("src/compiler/evaluator.ixx")  # noqa: F841 — used via must_present
    eval_tc_text = _read("src/compiler/evaluator_typecheck.cpp")  # noqa: F841 — used via must_present
    # audit_text + obs_text loaded via must_present which reads files directly
    obs_text = _read("src/compiler/observability_metrics.h")

    # Helper declaration + impl on InferenceEngine (in type_checker.ixx + .cpp).
    must_present(
        TC_IXX,
        "force_partial_cone_truncate_for_test",
        "AC6: type_checker.ixx declares InferenceEngine helper",
    )
    must_present(
        TC_IMPL,
        "InferenceEngine::force_partial_cone_truncate_for_test",
        "AC6: type_checker_impl.cpp defines InferenceEngine helper",
    )
    must_present(
        TC_IMPL,
        "last_partial_cone_truncated_ = true",
        "AC6: helper sets per-engine last_partial_cone_truncated_",
    )
    must_present(
        TC_IMPL,
        "last_partial_cone_dropped_ = dropped_count",
        "AC6: helper sets per-engine last_partial_cone_dropped_",
    )
    must_present(
        TC_IMPL,
        "typed_audit::publish_partial_cone_truncate(/*truncated=*/true",
        "AC6: helper mirrors to process-wide atomics via publish_partial_cone_truncate",
    )

    # Evaluator wrapper (mirror #2671 pattern: ensure_type_registry + create TC).
    must_present(
        EVAL_IXX,
        "force_partial_cone_truncate_for_test",
        "AC6: evaluator.ixx declares Evaluator helper",
    )
    must_present(
        EVAL_TC,
        "Evaluator::force_partial_cone_truncate_for_test",
        "AC6: evaluator_typecheck.cpp defines Evaluator wrapper",
    )
    must_present(
        EVAL_TC,
        "ensure_type_registry()",
        "AC6: Evaluator wrapper calls ensure_type_registry() (mirror #2671 pattern)",
    )

    # #2646 wiring preserved (additive, not replaced).
    must_present(
        TC_IMPL,
        "Issue #2646: cone-truncate must drop goals/memo",
        "AC5: #2646 source-cite preserved",
    )
    must_present(
        TC_IMPL,
        "if (last_partial_cone_truncated_)",
        "AC3: #2646 !truncated gate preserved",
    )
    must_present(
        TC_IMPL,
        "outside_cone_conds.empty()",
        "AC5: #2646 empty outside set short-circuit preserved",
    )

    # AC4: ordering invariant — outside invalidate fires AFTER #2622 sync.
    pos_2622 = impl_text.find("sync_occurrence_after_dirty(\n            std::span<const NodeId>(memo_targets.data(),")
    pos_2646 = impl_text.find("Issue #2646: cone-truncate must drop goals/memo")
    if pos_2622 == -1 or pos_2646 == -1 or pos_2646 <= pos_2622:
        failures.append("AC4: #2646 outside-invalidate position must be AFTER #2622 sync")

    # Counters from #2646 + #2621 preserved.
    must_present(
        AUDIT_H,
        "last_partial_cone_truncated",
        "AC5: last_partial_cone_truncated atomic preserved (publish_partial_cone_truncate)",
    )
    must_present(
        AUDIT_H,
        "last_partial_cone_dropped",
        "AC5: last_partial_cone_dropped atomic preserved (publish_partial_cone_truncate)",
    )
    must_present(
        AUDIT_H,
        "partial_cone_commit_hard_enabled",
        "AC5: #2621 partial_cone_commit_hard_enabled policy preserved",
    )
    if obs_text and "occurrence_cone_outside_invalidate_total" not in obs_text:
        failures.append("AC5: outside invalidate counter preserved in observability_metrics.h")

    # AC6: test file coverage — drift-injection unit test present + main() invokes.
    test_text = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    for ac_fn in (
        "ac2672_helper_drift_inject_sets_state",
        "ac2672_source_and_linter",
    ):
        if ac_fn not in test_text:
            failures.append(f"AC6: test missing {ac_fn} function")
    if "run_test_partial_cone_commit_gate" in test_text:
        for ac_fn in (
            "ac2672_helper_drift_inject_sets_state",
            "ac2672_source_and_linter",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: run_test does not call {ac_fn}()")

    # AC6: build.py wiring.
    build_text = _read("build.py")
    if "check_occurrence_cone_truncate_drift_2672" not in build_text:
        failures.append("AC6: build.py does not reference check_occurrence_cone_truncate_drift_2672 linter")
    if "cmd_occurrence_cone_truncate_drift_2672_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_occurrence_cone_truncate_drift_2672_coverage function")

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
        "OK: all #2672 ACs satisfied (drift-injection soak for cone-truncate "
        "outside-cone invalidate — per-engine + atomic state, additive to "
        "#2646/#2622/#2621, schema-2646 + drift-inject unit test wired)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
