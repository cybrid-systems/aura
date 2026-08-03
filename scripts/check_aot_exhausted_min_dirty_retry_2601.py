#!/usr/bin/env python3
"""check_aot_exhausted_min_dirty_retry_2601.py — Issue #2601 source gate.

Exhausted min-dirty reemit closed-loop retry under sustained Global storm.
Refines #2544 (one-shot min-dirty) + #2502 (force-JIT re-promote window).

AC1: Exhaust + Global storm → min-dirty storm-skipped; after storm
     clear, the registry's on_exhausted_min_dirty_queue seeds the
     retry budget (attempts_left = cap, last_reason, last_at_ms = 0)
     and the lazy retry hook (aura_hot_update_maybe_retry_exhausted_min_dirty)
     fires from on_reemit_pipeline_call when force_jit_regions_mask_ != 0
     and storm has cleared → retry_total counter +1.
AC2: Retry success × N → force_jit_repromote_total advances; mask
     cleared. The optional policy knob
     (force_jit_repromote_allow_pending_idle_when_force_jit_covered)
     lets the #2502 streak advance with pending_dirty > 0 when the
     success covered the force-JIT reason regions.
AC3: Cap hit / hard storm → no infinite retry; metrics visible.
     - NoAttemptsLeft decision bumps aot_exhausted_min_dirty_retry_cap_hit_total
     - StormActive decision bumps aot_exhausted_min_dirty_retry_storm_skip_total
     - Hard storm cannot bypass (decide_exhausted_min_dirty_retry exits
       on hard_storm_active()).
AC4: Soft / idle force-JIT path → zero extra work.
     - decide_exhausted_min_dirty_retry short-circuits on NoForceJit
       when force_jit_regions_mask_ == 0 (idle path).
     - BackoffNotElapsed short-circuits without bumping counters.
AC5: Additive schema; #2544 / #2502 / #2367 / #2601 surfaces remain
     compatible. New keys
     (aot-exhausted-min-dirty-retry-total / _success / _storm-skip /
     _cap-hit / _wired) exposed on query:aot-stats. schema-2601 +
     issue-2601 cross-linked on query:reload-recovery-state and the
     hot-update-registry-stats surface. The 4 new counters mirror
     into CompilerMetrics alongside the #2544 fields.

Rationale (Issue #2601 body):
  #2544's one-shot min-dirty reemit after fall_back_jit_only exhaust
  can fail or starve under sustained Global storm + high-frequency
  dirty. Re-promote window is reset by the storm streaks. Close the
  loop with a bounded retry series (cap 2-3, backoff) + optional
  policy knob for the #2502 streak to advance even with pending_dirty.

  Default: non-strict (exit 0, prints coverage summary). Use --strict
  to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
HOT_UPDATE_HH = ROOT / "src" / "compiler" / "hot_update_registry.hh"
HOT_UPDATE_CPP = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
MUTATE = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
TEST = ROOT / "tests" / "compiler" / "test_exhausted_min_dirty_reemit_2544.cpp"


def _extract_body(text: str, open_idx: int) -> str:
    """Extract the body of a brace-delimited block (handles nested {})."""
    assert text[open_idx] == "{", f"Expected '{{' at {open_idx}"
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1 : i]
        i += 1
    return ""


def _find_function_body(text: str, signature_regex: str) -> str:
    """Find a function by its signature regex and return its body."""
    m = re.search(signature_regex, text)
    if not m:
        return ""
    open_idx = text.find("{", m.end() - 1)
    if open_idx < 0:
        return ""
    return _extract_body(text, open_idx)


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    # AC1: on_exhausted_min_dirty_queue seeds retry budget + retry hook
    # in on_reemit_pipeline_call drives the lazy retry path.
    if not HOT_UPDATE_CPP.exists():
        failures.append("AC1: src/compiler/hot_update_registry.cpp not found")
        hot_update_cpp = ""
    else:
        hot_update_cpp = HOT_UPDATE_CPP.read_text(encoding="utf-8", errors="replace")

    if hot_update_cpp:
        exhaust_body = _find_function_body(
            hot_update_cpp,
            r"void\s+HotUpdateRegistry::on_exhausted_min_dirty_queue\s*\(",
        )
        if not exhaust_body:
            failures.append(
                "AC1: on_exhausted_min_dirty_queue definition not found in "
                "hot_update_registry.cpp (seed retry budget path)"
            )
        else:
            for field in (
                "exhausted_min_dirty_retry_attempts_left_",
                "exhausted_min_dirty_retry_attempts_cap_",
                "exhausted_min_dirty_retry_last_reason_",
                "exhausted_min_dirty_retry_last_at_ms_",
            ):
                if field not in exhaust_body:
                    failures.append(f"AC1: on_exhausted_min_dirty_queue body does not seed {field} (retry budget)")

        # Lazy retry hook in on_reemit_pipeline_call.
        reemit_pipeline_body = _find_function_body(
            hot_update_cpp,
            r"void\s+HotUpdateRegistry::on_reemit_pipeline_call\s*\(",
        )
        if not reemit_pipeline_body:
            failures.append("AC1: on_reemit_pipeline_call not found in hot_update_registry.cpp")
        else:
            if "aura_hot_update_maybe_retry_exhausted_min_dirty" not in reemit_pipeline_body:
                failures.append(
                    "AC1: on_reemit_pipeline_call does not call "
                    "aura_hot_update_maybe_retry_exhausted_min_dirty (lazy retry hook)"
                )
            if "force_jit_regions_mask_" not in reemit_pipeline_body:
                failures.append(
                    "AC1: on_reemit_pipeline_call does not gate on force_jit_regions_mask_ (zero-cost idle path)"
                )

    # AC2: decide_exhausted_min_dirty_retry + consume + pending-idle policy knob.
    if hot_update_cpp:
        decide_body = _find_function_body(
            hot_update_cpp,
            r"HotUpdateRegistry::ExhaustedMinDirtyRetryDecision\s*\n"
            r"\s*HotUpdateRegistry::decide_exhausted_min_dirty_retry\s*\(\s*\)\s*const",
        )
        if not decide_body:
            # Try a less strict pattern (single-line).
            decide_body = _find_function_body(
                hot_update_cpp,
                r"decide_exhausted_min_dirty_retry\s*\(\s*\)\s*const",
            )
        if not decide_body:
            failures.append(
                "AC2: decide_exhausted_min_dirty_retry not found in hot_update_registry.cpp (decision helper)"
            )
        else:
            for token in (
                "ExhaustedMinDirtyRetryDecision::NoForceJit",
                "ExhaustedMinDirtyRetryDecision::NoAttemptsLeft",
                "ExhaustedMinDirtyRetryDecision::BackoffNotElapsed",
                "ExhaustedMinDirtyRetryDecision::StormActive",
                "ExhaustedMinDirtyRetryDecision::Retry",
            ):
                if token not in decide_body:
                    failures.append(
                        f"AC2: decide_exhausted_min_dirty_retry body is missing "
                        f"the {token} branch (decision enum completeness)"
                    )

        consume_body = _find_function_body(
            hot_update_cpp,
            r"void\s+HotUpdateRegistry::consume_exhausted_min_dirty_retry_attempt\s*\(\s*\)",
        )
        if not consume_body:
            failures.append(
                "AC2: consume_exhausted_min_dirty_retry_attempt not found in hot_update_registry.cpp (bookkeeping step)"
            )
        else:
            if "compare_exchange_weak" not in consume_body:
                failures.append(
                    "AC2: consume_exhausted_min_dirty_retry_attempt does not "
                    "use compare_exchange_weak (CAS loop on attempts_left)"
                )
            if "steady_ms_now" not in consume_body:
                failures.append(
                    "AC2: consume_exhausted_min_dirty_retry_attempt does not "
                    "stamp steady_ms_now() on last_at_ms (backoff anchor)"
                )

        # Pending-idle policy knob.
        if "force_jit_repromote_allow_pending_idle_when_force_jit_covered" not in hot_update_cpp:
            failures.append(
                "AC2: maybe_force_jit_repromote_on_clean_success does not "
                "consult force_jit_repromote_allow_pending_idle_when_force_jit_covered "
                "(policy knob for #2502 streak with pending_dirty > 0)"
            )

    # AC3: bridge retry helper bumps the 4 counters + cap + storm-skip
    # paths are observable.
    if not BRIDGE.exists():
        failures.append("AC3: src/compiler/aura_jit_bridge.cpp not found")
    else:
        bridge = BRIDGE.read_text(encoding="utf-8", errors="replace")
        retry_body = _find_function_body(
            bridge,
            r'extern\s+"C"\s+void\s+aura_hot_update_maybe_retry_exhausted_min_dirty\s*\(\s*void\s*\)',
        )
        if not retry_body:
            failures.append(
                "AC3: aura_hot_update_maybe_retry_exhausted_min_dirty C-linkage "
                "definition not found in aura_jit_bridge.cpp (retry driver)"
            )
        else:
            for counter in (
                "aot_exhausted_min_dirty_retry_total",
                "aot_exhausted_min_dirty_retry_success_total",
                "aot_exhausted_min_dirty_retry_storm_skip_total",
                "aot_exhausted_min_dirty_retry_cap_hit_total",
            ):
                if counter not in retry_body:
                    failures.append(
                        f"AC3: aura_hot_update_maybe_retry_exhausted_min_dirty body does not bump {counter}"
                    )
            if "aura_reemit_aot_for_dirty" not in retry_body:
                failures.append(
                    "AC3: aura_hot_update_maybe_retry_exhausted_min_dirty body "
                    "does not call aura_reemit_aot_for_dirty (no reemit drive)"
                )
            if "decide_exhausted_min_dirty_retry" not in retry_body:
                failures.append(
                    "AC3: aura_hot_update_maybe_retry_exhausted_min_dirty body "
                    "does not call decide_exhausted_min_dirty_retry (decision-driven)"
                )
            if "consume_exhausted_min_dirty_retry_attempt" not in retry_body:
                failures.append(
                    "AC3: aura_hot_update_maybe_retry_exhausted_min_dirty body "
                    "does not call consume_exhausted_min_dirty_retry_attempt "
                    "(attempts_left decrement / last_at_ms stamp)"
                )

    # AC4: Header state fields + getters + reset_for_test + zero-cost path.
    if not HOT_UPDATE_HH.exists():
        failures.append("AC4: src/compiler/hot_update_registry.hh not found")
    else:
        hh = HOT_UPDATE_HH.read_text(encoding="utf-8", errors="replace")

        for field in (
            "std::atomic<std::uint32_t> exhausted_min_dirty_retry_attempts_left_",
            "std::atomic<std::uint32_t> exhausted_min_dirty_retry_attempts_cap_",
            "std::atomic<std::uint64_t> exhausted_min_dirty_retry_backoff_ms_",
            "std::atomic<std::uint64_t> exhausted_min_dirty_retry_last_at_ms_",
            "std::atomic<std::uint8_t> exhausted_min_dirty_retry_last_reason_",
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_total_",
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_success_total_",
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_storm_skip_total_",
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_cap_hit_total_",
            "std::atomic<std::uint8_t> force_jit_repromote_allow_pending_idle_when_force_jit_covered_",
        ):
            if field not in hh:
                failures.append(f"AC4: hot_update_registry.hh missing field declaration: {field}")

        # C ABI declaration + zero-cost idle path
        if "aura_hot_update_maybe_retry_exhausted_min_dirty" not in hh:
            failures.append(
                'AC4: hot_update_registry.hh missing extern "C" declaration '
                "for aura_hot_update_maybe_retry_exhausted_min_dirty"
            )

        # Zero-cost hint in comments
        if "Zero-cost when force_jit_regions_mask_ == 0" not in hh:
            failures.append(
                "AC4: hot_update_registry.hh does not document zero-cost idle path (force_jit_regions_mask_ == 0)"
            )

    # AC5: observability_metrics.h has the 4 new counters paired with #2544.
    if not METRICS.exists():
        failures.append("AC5: src/compiler/observability_metrics.h not found")
    else:
        metrics = METRICS.read_text(encoding="utf-8", errors="replace")
        for counter in (
            "aot_exhausted_min_dirty_retry_total",
            "aot_exhausted_min_dirty_retry_success_total",
            "aot_exhausted_min_dirty_retry_storm_skip_total",
            "aot_exhausted_min_dirty_retry_cap_hit_total",
        ):
            if counter not in metrics:
                failures.append(
                    f"AC5: observability_metrics.h missing atomic counter "
                    f"{counter} (next to the #2544 exhausted min-dirty family)"
                )

        # Paired-declaration pattern: 4 new counters should be within ~200
        # chars of a #2544 counter (struct locality).
        anchor = re.search(
            r"std::atomic<std::uint64_t>\s+aot_reload_exhausted_min_dirty_reemit_storm_skip_total\b",
            metrics,
        )
        if anchor:
            for counter in (
                "aot_exhausted_min_dirty_retry_total",
                "aot_exhausted_min_dirty_retry_success_total",
                "aot_exhausted_min_dirty_retry_storm_skip_total",
                "aot_exhausted_min_dirty_retry_cap_hit_total",
            ):
                m = re.search(
                    r"std::atomic<std::uint64_t>\s+" + counter + r"\b",
                    metrics,
                )
                if m and abs(m.start() - anchor.start()) > 600:
                    failures.append(
                        f"AC5: {counter} is not adjacent to "
                        f"aot_reload_exhausted_min_dirty_reemit_storm_skip_total "
                        f"in observability_metrics.h"
                    )

    # AC5 (cont.): evaluator_primitives_obs_eval.cpp exposes the keys.
    if not OBS_EVAL.exists():
        failures.append("AC5: src/compiler/evaluator_primitives_obs_eval.cpp not found")
    else:
        obs_eval = OBS_EVAL.read_text(encoding="utf-8", errors="replace")
        for key in (
            "aot-exhausted-min-dirty-retry-total",
            "aot-exhausted-min-dirty-retry-success-total",
            "aot-exhausted-min-dirty-retry-storm-skip-total",
            "aot-exhausted-min-dirty-retry-cap-hit-total",
            "aot-exhausted-min-dirty-retry-wired",
            "schema-2601",
            "issue-2601",
        ):
            if key not in obs_eval:
                failures.append(f"AC5: evaluator_primitives_obs_eval.cpp does not expose {key} on query:aot-stats")

    # AC5 (cont.): evaluator_primitives_mutate.cpp cross-links schema-2601.
    if not MUTATE.exists():
        failures.append("AC5: src/compiler/evaluator_primitives_mutate.cpp not found")
    else:
        mutate = MUTATE.read_text(encoding="utf-8", errors="replace")
        if "schema-2601" not in mutate:
            failures.append(
                "AC5: evaluator_primitives_mutate.cpp does not cross-link "
                "schema-2601 on query:reload-recovery-state (or related surface)"
            )
        if "issue-2601" not in mutate:
            failures.append(
                "AC5: evaluator_primitives_mutate.cpp does not cross-link "
                "issue-2601 on query:reload-recovery-state (or related surface)"
            )

    # AC5 (cont.): test file has #2601 ACs.
    if not TEST.exists():
        failures.append("AC5: tests/compiler/test_exhausted_min_dirty_reemit_2544.cpp not found")
    else:
        test_text = TEST.read_text(encoding="utf-8", errors="replace")
        for ac_fn in (
            "ac2601_storm_clear_auto_retry",
            "ac2601_retry_success_repromote",
            "ac2601_cap_hit_no_infinite",
            "ac2601_soft_zero_cost",
            "ac2601_schema_and_source",
        ):
            if ac_fn not in test_text:
                failures.append(
                    f"AC5: tests/compiler/test_exhausted_min_dirty_reemit_2544.cpp "
                    f"missing test function {ac_fn} (issue #2601 coverage)"
                )
        if "main()" in test_text:
            for ac_fn in (
                "ac2601_storm_clear_auto_retry",
                "ac2601_retry_success_repromote",
                "ac2601_cap_hit_no_infinite",
                "ac2601_soft_zero_cost",
                "ac2601_schema_and_source",
            ):
                if f"{ac_fn}()" not in test_text:
                    failures.append(f"AC5: main() does not call {ac_fn}()")

    # Additional AC5: Schema compatibility cross-links.
    if METRICS.exists() and HOT_UPDATE_HH.exists():
        # #2544 / #2502 / #2367 surfaces must remain compatible (additive).
        # We don't enforce preservation strictly here (Schema additive is a
        # source discipline rather than a single source gate), but we do
        # require the #2367 reload-recovery snapshot struct to mention
        # schema_2601 (matches the issue AC5 "Additive schema").
        hh = HOT_UPDATE_HH.read_text(encoding="utf-8", errors="replace")
        if "schema_2601" not in hh:
            failures.append(
                "AC5: hot_update_registry.hh reload-recovery snapshot struct "
                "is missing schema_2601 field (additive schema compatibility)"
            )
        if "issue_2601" not in hh:
            failures.append(
                "AC5: hot_update_registry.hh reload-recovery snapshot struct "
                "is missing issue_2601 field (additive schema compatibility)"
            )

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
        "OK: all #2601 ACs satisfied (exhausted min-dirty retry closed loop — "
        "4 counters + retry hook + policy knob + schema-2601 + tests)"
    )
    return 0


def main_strict() -> int:
    """Always-strict variant for self-test fixtures."""
    saved = sys.argv
    try:
        sys.argv = list(saved) + ["--strict"]
        return main()
    finally:
        sys.argv = saved


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    tmp = Path(tempfile.mkdtemp(prefix="check_2601_selftest_"))
    try:
        good_bridge = tmp / "bridge.cpp"
        good_bridge.write_text(
            "#include <cstdint>\n"
            '#include "compiler/aura_jit_bridge.h"\n'
            "namespace aura::compiler { class HotUpdateRegistry { public:\n"
            "    enum class ExhaustedMinDirtyRetryDecision : std::uint8_t {\n"
            "        NoForceJit = 0, NoAttemptsLeft = 1, BackoffNotElapsed = 2, "
            "StormActive = 3, Retry = 4,\n"
            "    };\n"
            "    ExhaustedMinDirtyRetryDecision decide_exhausted_min_dirty_retry() const;\n"
            "    void consume_exhausted_min_dirty_retry_attempt();\n"
            "}; }\n"
            'extern "C" std::uint64_t aura_reemit_aot_for_dirty(std::uint64_t);\n'
            'extern "C" void aura_get_aot_defuse_version(void);\n'
            "struct aot_metrics_t; aot_metrics_t* aot_metrics();\n"
            'extern "C" void aura_hot_update_maybe_retry_exhausted_min_dirty(void) {\n'
            "    auto& hur = aura::compiler::hot_update_registry();\n"
            "    const auto d = hur.decide_exhausted_min_dirty_retry();\n"
            "    if (d == aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::Retry) {\n"
            "        hur.consume_exhausted_min_dirty_retry_attempt();\n"
            "        if (aot_metrics()) aot_metrics()->aot_exhausted_min_dirty_retry_total.fetch_add(1, std::memory_order_relaxed);\n"
            "        const auto n = aura_reemit_aot_for_dirty(aura_get_aot_defuse_version());\n"
            "        if (n > 0) { if (aot_metrics()) aot_metrics()->aot_exhausted_min_dirty_retry_success_total.fetch_add(1, std::memory_order_relaxed); }\n"
            "    } else if (d == aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::NoAttemptsLeft) {\n"
            "        if (aot_metrics()) aot_metrics()->aot_exhausted_min_dirty_retry_cap_hit_total.fetch_add(1, std::memory_order_relaxed);\n"
            "    } else if (d == aura::compiler::HotUpdateRegistry::ExhaustedMinDirtyRetryDecision::StormActive) {\n"
            "        if (aot_metrics()) aot_metrics()->aot_exhausted_min_dirty_retry_storm_skip_total.fetch_add(1, std::memory_order_relaxed);\n"
            "    }\n"
            "}\n",
            encoding="utf-8",
        )

        good_hh = tmp / "hh.h"
        good_hh.write_text(
            "// hot_update_registry.hh fixture\n"
            "std::atomic<std::uint32_t> exhausted_min_dirty_retry_attempts_left_{0};\n"
            "std::atomic<std::uint32_t> exhausted_min_dirty_retry_attempts_cap_{3};\n"
            "std::atomic<std::uint64_t> exhausted_min_dirty_retry_backoff_ms_{100};\n"
            "std::atomic<std::uint64_t> exhausted_min_dirty_retry_last_at_ms_{0};\n"
            "std::atomic<std::uint8_t> exhausted_min_dirty_retry_last_reason_{0};\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_total_{0};\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_success_total_{0};\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_storm_skip_total_{0};\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_cap_hit_total_{0};\n"
            "std::atomic<std::uint8_t> force_jit_repromote_allow_pending_idle_when_force_jit_covered_{0};\n"
            "// Zero-cost when force_jit_regions_mask_ == 0\n"
            'extern "C" void aura_hot_update_maybe_retry_exhausted_min_dirty(void);\n'
            "struct aura_reload_recovery_snapshot { std::int64_t schema_2601; std::int64_t issue_2601; };\n",
            encoding="utf-8",
        )

        good_cpp = tmp / "cpp.cpp"
        good_cpp.write_text(
            "// hot_update_registry.cpp fixture\n"
            "void HotUpdateRegistry::on_exhausted_min_dirty_queue(AotReloadFail reason) {\n"
            "    const auto cap = exhausted_min_dirty_retry_attempts_cap_.load();\n"
            "    if (cap > 0) {\n"
            "        exhausted_min_dirty_retry_attempts_left_.store(cap);\n"
            "        exhausted_min_dirty_retry_last_reason_.store(0);\n"
            "        exhausted_min_dirty_retry_last_at_ms_.store(0);\n"
            "    }\n"
            "}\n"
            "void HotUpdateRegistry::on_reemit_pipeline_call(std::uint64_t c, std::uint64_t s) {\n"
            "    if (force_jit_regions_mask_.load() != 0) {\n"
            "        aura_hot_update_maybe_retry_exhausted_min_dirty();\n"
            "    }\n"
            "}\n"
            "HotUpdateRegistry::ExhaustedMinDirtyRetryDecision\n"
            "HotUpdateRegistry::decide_exhausted_min_dirty_retry() const {\n"
            "    if (force_jit_regions_mask_.load() == 0) return ExhaustedMinDirtyRetryDecision::NoForceJit;\n"
            "    if (exhausted_min_dirty_retry_attempts_left_.load() == 0) return ExhaustedMinDirtyRetryDecision::NoAttemptsLeft;\n"
            "    if (last_at != 0) { /* backoff */ return ExhaustedMinDirtyRetryDecision::BackoffNotElapsed; }\n"
            "    if (current_storm_level() != StormLevel::None) return ExhaustedMinDirtyRetryDecision::StormActive;\n"
            "    if (hard_storm_active()) return ExhaustedMinDirtyRetryDecision::StormActive;\n"
            "    return ExhaustedMinDirtyRetryDecision::Retry;\n"
            "}\n"
            "void HotUpdateRegistry::consume_exhausted_min_dirty_retry_attempt() {\n"
            "    auto n = exhausted_min_dirty_retry_attempts_left_.load();\n"
            "    while (n > 0) {\n"
            "        if (exhausted_min_dirty_retry_attempts_left_.compare_exchange_weak(n, n - 1)) break;\n"
            "    }\n"
            "    exhausted_min_dirty_retry_last_at_ms_.store(steady_ms_now());\n"
            "}\n"
            "// force_jit_repromote_allow_pending_idle_when_force_jit_covered used in maybe_force_jit_repromote_on_clean_success\n"
            "if (force_jit_repromote_allow_pending_idle_when_force_jit_covered_.load() == 0) { /* reset */ }\n",
            encoding="utf-8",
        )

        good_metrics = tmp / "metrics.h"
        good_metrics.write_text(
            "// observability_metrics.h fixture\n"
            "std::atomic<std::uint64_t> aot_reload_exhausted_min_dirty_reemit_storm_skip_total{0}; // #2544\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_total{0};         // #2601\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_success_total{0};  // #2601\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_storm_skip_total{0}; // #2601\n"
            "std::atomic<std::uint64_t> aot_exhausted_min_dirty_retry_cap_hit_total{0};  // #2601\n",
            encoding="utf-8",
        )

        good_obs_eval = tmp / "obs_eval.cpp"
        good_obs_eval.write_text(
            "// obs_eval fixture\n"
            '{"aot-exhausted-min-dirty-retry-total", make_int(0)},\n'
            '{"aot-exhausted-min-dirty-retry-success-total", make_int(0)},\n'
            '{"aot-exhausted-min-dirty-retry-storm-skip-total", make_int(0)},\n'
            '{"aot-exhausted-min-dirty-retry-cap-hit-total", make_int(0)},\n'
            '{"aot-exhausted-min-dirty-retry-wired", make_int(1)},\n'
            '{"schema-2601", make_int(2601)},\n'
            '{"issue-2601", make_int(2601)},\n',
            encoding="utf-8",
        )

        good_mutate = tmp / "mutate.cpp"
        good_mutate.write_text(
            '// mutate fixture\ninsert_kv("schema-2601", 2601);\ninsert_kv("issue-2601", 2601);\n',
            encoding="utf-8",
        )

        good_test = tmp / "test.cpp"
        good_test.write_text(
            "// test fixture\n"
            "static void ac2601_storm_clear_auto_retry() {}\n"
            "static void ac2601_retry_success_repromote() {}\n"
            "static void ac2601_cap_hit_no_infinite() {}\n"
            "static void ac2601_soft_zero_cost() {}\n"
            "static void ac2601_schema_and_source() {}\n"
            "int main() {\n"
            "    ac2601_storm_clear_auto_retry();\n"
            "    ac2601_retry_success_repromote();\n"
            "    ac2601_cap_hit_no_infinite();\n"
            "    ac2601_soft_zero_cost();\n"
            "    ac2601_schema_and_source();\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )

        import check_aot_exhausted_min_dirty_retry_2601 as self_mod

        original = {
            "BRIDGE": self_mod.BRIDGE,
            "HOT_UPDATE_HH": self_mod.HOT_UPDATE_HH,
            "HOT_UPDATE_CPP": self_mod.HOT_UPDATE_CPP,
            "METRICS": self_mod.METRICS,
            "OBS_EVAL": self_mod.OBS_EVAL,
            "MUTATE": self_mod.MUTATE,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.HOT_UPDATE_HH = good_hh
            self_mod.HOT_UPDATE_CPP = good_cpp
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.MUTATE = good_mutate
            self_mod.TEST = good_test
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Bad fixture: missing AC function in test
        bad_test = tmp / "test_bad.cpp"
        bad_test.write_text(
            "// test fixture — missing AC functions\nint main() { return 0; }\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.HOT_UPDATE_HH = good_hh
            self_mod.HOT_UPDATE_CPP = good_cpp
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.MUTATE = good_mutate
            self_mod.TEST = bad_test
            rc_bad = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print(
                "SELF-TEST FAIL: known-bad (missing test ACs) accepted",
                file=sys.stderr,
            )
            return 1

        # Bad fixture: missing retry helper in bridge
        bad_bridge = tmp / "bridge_bad.cpp"
        bad_bridge.write_text(
            "#include <cstdint>\n"
            'extern "C" void aura_reemit_aot_for_dirty(void) {}\n'
            "// missing aura_hot_update_maybe_retry_exhausted_min_dirty\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = bad_bridge
            self_mod.HOT_UPDATE_HH = good_hh
            self_mod.HOT_UPDATE_CPP = good_cpp
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.MUTATE = good_mutate
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print(
                "SELF-TEST FAIL: known-bad (missing retry helper) accepted",
                file=sys.stderr,
            )
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
