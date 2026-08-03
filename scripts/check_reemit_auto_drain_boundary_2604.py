#!/usr/bin/env python3
"""check_reemit_auto_drain_boundary_2604.py — Issue #2604 source gate.

Outermost MutationBoundary exit auto-drain deferred reemit + one
region-filtered pass. Closes the "visible but unhealed" stale window
without making reemit unbounded.

AC1: Deferred reemit pending → outermost exit triggers one reemit;
     deferred flag cleared (take_deferred_reemit_version returns 0
     after drain). Counters bump on_boundary_exit + success.
AC2: Only `last_region_mask_from_dirty` set → same auto pass.
     take_deferred_reemit_version may return 0 (no pending), but
     aura_reemit_aot_for_dirty still runs with current_defuse.
AC3: Storm throttle active → skip body, bump throttled counter.
     Deferred NOT cleared (re-defer or leave pending per policy).
AC4: Common path (no deferred, mask=0) → zero extra work. AC4
     short-circuits at the top: single relaxed load on the common
     path, no C ABI bumper calls.
AC5: Source-cite + unit test in
     test_reemit_mutation_boundary_handshake_2114.cpp (extended per
     #81967 with ac2604_* sections).

Rationale (Issue #2604 body):
  #2205/#2208 Defer production default records pending reemit
  outside a real boundary; drain is expected on outermost Guard
  exit. Today drain is present but not always followed by an
  automatic region-filtered reemit when last_region_mask_from_dirty
  or deferred version is set. Stale heal remains largely
  Agent-driven via query surfaces. This adds the auto-drain on the
  outermost exit so boundary exit ≈ consistent epoch without
  requiring Agent round-trips on every soft dirty.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVAL = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
HUR = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
HUR_H = ROOT / "src" / "compiler" / "hot_update_registry.hh"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_reemit_mutation_boundary_handshake_2114.cpp"


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

    # AC1: 3 new counters in observability_metrics.h
    if not METRICS.exists():
        failures.append("AC1: src/compiler/observability_metrics.h not found")
        metrics = ""
    else:
        metrics = METRICS.read_text(encoding="utf-8", errors="replace")

    if metrics:
        for counter in (
            "reemit_auto_drain_on_boundary_exit_total",
            "reemit_auto_drain_success_total",
            "reemit_auto_drain_throttled_total",
        ):
            if counter not in metrics:
                failures.append(
                    f"AC1: observability_metrics.h missing atomic counter {counter} (outermost exit auto-drain metric)"
                )
        # Paired-declaration: new counters should be near existing
        # aot_reload_auto_retry_total (related reemit family).
        anchor = re.search(
            r"std::atomic<std::uint64_t>\s+aot_reload_auto_retry_total\b",
            metrics,
        )
        if anchor:
            for counter in (
                "reemit_auto_drain_on_boundary_exit_total",
                "reemit_auto_drain_success_total",
                "reemit_auto_drain_throttled_total",
            ):
                m = re.search(
                    r"std::atomic<std::uint64_t>\s+" + counter + r"\b",
                    metrics,
                )
                if m and abs(m.start() - anchor.start()) > 600:
                    failures.append(
                        f"AC1: {counter} is not adjacent to aot_reload_auto_retry_total in observability_metrics.h"
                    )

    # AC1 (cont.): C ABI bump functions in hot_update_registry.cpp
    if not HUR.exists():
        failures.append("AC1: src/compiler/hot_update_registry.cpp not found")
    else:
        hur = HUR.read_text(encoding="utf-8", errors="replace")
        for fn in (
            "aura_bump_reemit_auto_drain_on_boundary_exit_total",
            "aura_bump_reemit_auto_drain_success_total",
            "aura_bump_reemit_auto_drain_throttled_total",
        ):
            if fn not in hur:
                failures.append(
                    f"AC1: hot_update_registry.cpp missing C ABI definition for {fn} (outermost exit auto-drain bumper)"
                )

    # AC1 (cont.): declarations in hot_update_registry.hh
    if not HUR_H.exists():
        failures.append("AC1: src/compiler/hot_update_registry.hh not found")
    else:
        hur_h = HUR_H.read_text(encoding="utf-8", errors="replace")
        for fn in (
            "bump_reemit_auto_drain_on_boundary_exit_total",
            "bump_reemit_auto_drain_success_total",
            "bump_reemit_auto_drain_throttled_total",
        ):
            if fn not in hur_h:
                failures.append(
                    f"AC1: hot_update_registry.hh missing class declaration for {fn} (outermost exit auto-drain bumper)"
                )

    # AC1 (cont.): auto-drain logic in exit_mutation_boundary
    if not EVAL.exists():
        failures.append("AC1: src/compiler/evaluator_mutation_boundary.cpp not found")
    else:
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")
        # Find the auto-drain block in exit_mutation_boundary.
        # It should call aura_bump_reemit_auto_drain_on_boundary_exit_total.
        if "aura_bump_reemit_auto_drain_on_boundary_exit_total" not in eval_cpp:
            failures.append(
                "AC1: evaluator_mutation_boundary.cpp missing "
                "aura_bump_reemit_auto_drain_on_boundary_exit_total call "
                "(outermost exit auto-drain not wired)"
            )
        # Must be guarded by !nested_boundary (outermost only).
        # The auto-drain block should reference both nested_boundary
        # and success (or have the guard inline).
        if "aura_bump_reemit_auto_drain" in eval_cpp:
            # Find the context around the auto-drain call to verify
            # the !nested_boundary && success guard. Guard is BEFORE
            # the bumper call (the if-condition is in preceding text).
            block_idx = eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total")
            surrounding = eval_cpp[max(0, block_idx - 800) : block_idx + 400]
            if "nested_boundary" not in surrounding or "success" not in surrounding:
                failures.append("AC1: auto-drain block must guard on !nested_boundary && success (outermost exit only)")

    # AC1 (cont.): auto-drain takes deferred + calls aura_reemit_aot_for_dirty
    if EVAL.exists():
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")
        if "aura_bump_reemit_auto_drain_on_boundary_exit_total" in eval_cpp:
            block_idx = eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total")
            surrounding_after = eval_cpp[block_idx : block_idx + 1500]
            if "aura_reemit_aot_for_dirty" not in surrounding_after:
                failures.append("AC1: auto-drain must call aura_reemit_aot_for_dirty (region-filtered reemit pass)")

    # AC2: same auto pass when only last_region_mask_from_dirty set.
    # Source-cite: the auto-drain block must check
    # last_region_mask_from_dirty != 0 OR has_deferred_reemit().
    if EVAL.exists():
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")
        if "aura_bump_reemit_auto_drain_on_boundary_exit_total" in eval_cpp:
            block_idx = eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total")
            # Guard is BEFORE the bumper call (the if-condition).
            surrounding = eval_cpp[max(0, block_idx - 800) : block_idx + 400]
            if "last_region_mask_from_dirty" not in surrounding:
                failures.append(
                    "AC2: auto-drain must check last_region_mask_from_dirty != 0 "
                    "(mask-only path also triggers auto pass)"
                )
            if "has_deferred_reemit" not in surrounding:
                failures.append(
                    "AC2: auto-drain must check has_deferred_reemit() (deferred-only path also triggers auto pass)"
                )

    # AC3: storm throttle → skip body, bump throttled counter.
    if EVAL.exists():
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")
        if "aura_bump_reemit_auto_drain_on_boundary_exit_total" in eval_cpp:
            block_idx = eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total")
            surrounding = eval_cpp[block_idx : block_idx + 1500]
            if "aura_bump_reemit_auto_drain_throttled_total" not in surrounding:
                failures.append(
                    "AC3: auto-drain must call "
                    "aura_bump_reemit_auto_drain_throttled_total on storm "
                    "throttle (no silent drop of deferred forever)"
                )
            if "should_throttle_reemit" not in surrounding:
                failures.append("AC3: auto-drain must check should_throttle_reemit (storm gate before reemit body)")

    # AC4: soft path (no deferred, mask=0) → zero extra work.
    if EVAL.exists():
        eval_cpp = EVAL.read_text(encoding="utf-8", errors="replace")
        if "aura_bump_reemit_auto_drain_on_boundary_exit_total" in eval_cpp:
            # The soft path short-circuits BEFORE any bumper call.
            # Find the if-block and verify the guard is first.
            block_idx = eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total")
            # Look for the guard pattern: "if (!reg.has_deferred_reemit() &&"
            # or "if (!reg.has_deferred_reemit() ||"
            preceding = eval_cpp[max(0, block_idx - 600) : block_idx]
            if "has_deferred_reemit" not in preceding or "last_region_mask_from_dirty" not in preceding:
                failures.append(
                    "AC4: soft path guard (has_deferred_reemit || "
                    "last_region_mask_from_dirty != 0) must precede the "
                    "bumper call (zero extra work on common path)"
                )

    # AC5: query surface in evaluator_primitives_obs_eval.cpp
    if not OBS_EVAL.exists():
        failures.append("AC5: src/compiler/evaluator_primitives_obs_eval.cpp not found")
    else:
        obs_eval = OBS_EVAL.read_text(encoding="utf-8", errors="replace")
        for key in (
            "reemit-auto-drain-on-boundary-exit-total",
            "reemit-auto-drain-success-total",
            "reemit-auto-drain-throttled-total",
            "reemit-auto-drain-wired",
            "schema-2604",
            "issue-2604",
        ):
            if key not in obs_eval:
                failures.append(
                    f"AC5: evaluator_primitives_obs_eval.cpp does not expose {key} on query:aot-reload-stats"
                )
        # Compatibility: prior aot-reload-stats schemas preserved.
        for key in (
            "schema-2165",
            "schema-2232",
            "schema-2249",
        ):
            if key not in obs_eval:
                failures.append(
                    f"AC5: evaluator_primitives_obs_eval.cpp does not preserve "
                    f"existing {key} (compatibility with #2165/#2232/#2249)"
                )

    # AC5 (cont.): test file has ac2604_* sections
    if not TEST.exists():
        failures.append("AC5: tests/compiler/test_reemit_mutation_boundary_handshake_2114.cpp not found")
    else:
        test_text = TEST.read_text(encoding="utf-8", errors="replace")
        for ac_fn in (
            "ac2604_deferred_triggers_reemit",
            "ac2604_mask_only_triggers_reemit",
            "ac2604_storm_throttle_bumps_throttled",
            "ac2604_soft_zero_cost",
            "ac2604_source_and_schema",
        ):
            if ac_fn not in test_text:
                failures.append(
                    f"AC5: tests/compiler/test_reemit_mutation_boundary_handshake_2114.cpp "
                    f"missing test function {ac_fn} (issue #2604 coverage)"
                )
        if "main()" in test_text:
            for ac_fn in (
                "ac2604_deferred_triggers_reemit",
                "ac2604_mask_only_triggers_reemit",
                "ac2604_storm_throttle_bumps_throttled",
                "ac2604_soft_zero_cost",
                "ac2604_source_and_schema",
            ):
                if f"{ac_fn}()" not in test_text:
                    failures.append(f"AC5: main() does not call {ac_fn}()")

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
        "OK: all #2604 ACs satisfied (outermost exit auto-drain deferred "
        "reemit + one region-filtered pass — counters + soft path + storm "
        "throttle + schema + tests)"
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
    tmp = Path(tempfile.mkdtemp(prefix="check_2604_selftest_"))
    try:
        good_eval = tmp / "eval.cpp"
        good_eval.write_text(
            "Evaluator::MutationCheckpoint Evaluator::exit_mutation_boundary(bool success) {\n"
            "    if (!nested_boundary && success) {\n"
            "        auto& reg = hot_update_registry();\n"
            "        if (reg.has_deferred_reemit() || reg.last_region_mask_from_dirty() != 0) {\n"
            "            aura_bump_reemit_auto_drain_on_boundary_exit_total();\n"
            "            if (reg.should_throttle_reemit()) {\n"
            "                aura_bump_reemit_auto_drain_throttled_total();\n"
            "            } else {\n"
            "                auto v = reg.take_deferred_reemit_version();\n"
            "                if (v == 0) v = aura_get_aot_defuse_version();\n"
            "                const auto n = aura_reemit_aot_for_dirty(v);\n"
            "                if (n > 0) aura_bump_reemit_auto_drain_success_total();\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "    return cp;\n"
            "}\n",
            encoding="utf-8",
        )

        good_metrics = tmp / "metrics.h"
        good_metrics.write_text(
            "std::atomic<std::uint64_t> aot_reload_auto_retry_total{0};\n"
            "// Issue #2604\n"
            "std::atomic<std::uint64_t> reemit_auto_drain_on_boundary_exit_total{0};\n"
            "std::atomic<std::uint64_t> reemit_auto_drain_success_total{0};\n"
            "std::atomic<std::uint64_t> reemit_auto_drain_throttled_total{0};\n",
            encoding="utf-8",
        )

        good_hur = tmp / "hur.cpp"
        good_hur.write_text(
            'extern "C" void aura_bump_reemit_auto_drain_on_boundary_exit_total(void) {\n'
            "    hot_update_registry().bump_reemit_auto_drain_on_boundary_exit_total();\n"
            "}\n"
            'extern "C" void aura_bump_reemit_auto_drain_success_total(void) {\n'
            "    hot_update_registry().bump_reemit_auto_drain_success_total();\n"
            "}\n"
            'extern "C" void aura_bump_reemit_auto_drain_throttled_total(void) {\n'
            "    hot_update_registry().bump_reemit_auto_drain_throttled_total();\n"
            "}\n",
            encoding="utf-8",
        )

        good_hur_h = tmp / "hur.h"
        good_hur_h.write_text(
            "void bump_reemit_auto_drain_on_boundary_exit_total() noexcept;\n"
            "void bump_reemit_auto_drain_success_total() noexcept;\n"
            "void bump_reemit_auto_drain_throttled_total() noexcept;\n",
            encoding="utf-8",
        )

        good_obs_eval = tmp / "obs_eval.cpp"
        good_obs_eval.write_text(
            '{"reemit-auto-drain-on-boundary-exit-total", make_int(0)},\n'
            '{"reemit-auto-drain-success-total", make_int(0)},\n'
            '{"reemit-auto-drain-throttled-total", make_int(0)},\n'
            '{"reemit-auto-drain-wired", make_int(1)},\n'
            '{"schema-2604", make_int(2604)},\n'
            '{"issue-2604", make_int(2604)},\n'
            '{"schema-2165", make_int(2165)},\n'
            '{"schema-2232", make_int(2232)},\n'
            '{"schema-2249", make_int(2249)},\n',
            encoding="utf-8",
        )

        good_test = tmp / "test.cpp"
        good_test.write_text(
            "static void ac2604_deferred_triggers_reemit() {}\n"
            "static void ac2604_mask_only_triggers_reemit() {}\n"
            "static void ac2604_storm_throttle_bumps_throttled() {}\n"
            "static void ac2604_soft_zero_cost() {}\n"
            "static void ac2604_source_and_schema() {}\n"
            "int main() {\n"
            "    ac2604_deferred_triggers_reemit();\n"
            "    ac2604_mask_only_triggers_reemit();\n"
            "    ac2604_storm_throttle_bumps_throttled();\n"
            "    ac2604_soft_zero_cost();\n"
            "    ac2604_source_and_schema();\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )

        import check_reemit_auto_drain_boundary_2604 as self_mod

        original = {
            "EVAL": self_mod.EVAL,
            "HUR": self_mod.HUR,
            "HUR_H": self_mod.HUR_H,
            "METRICS": self_mod.METRICS,
            "OBS_EVAL": self_mod.OBS_EVAL,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.EVAL = good_eval
            self_mod.HUR = good_hur
            self_mod.HUR_H = good_hur_h
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
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
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        try:
            self_mod.EVAL = good_eval
            self_mod.HUR = good_hur
            self_mod.HUR_H = good_hur_h
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
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

        # Bad fixture: storm throttle check missing
        bad_eval = tmp / "eval_bad.cpp"
        bad_eval.write_text(
            "Evaluator::MutationCheckpoint Evaluator::exit_mutation_boundary(bool success) {\n"
            "    if (!nested_boundary && success) {\n"
            "        aura_bump_reemit_auto_drain_on_boundary_exit_total();\n"
            "        auto v = aura_get_aot_defuse_version();\n"
            "        aura_reemit_aot_for_dirty(v); // missing should_throttle check\n"
            "    }\n"
            "    return cp;\n"
            "}\n",
            encoding="utf-8",
        )
        try:
            self_mod.EVAL = bad_eval
            self_mod.HUR = good_hur
            self_mod.HUR_H = good_hur_h
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print(
                "SELF-TEST FAIL: known-bad (storm throttle missing) accepted",
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
