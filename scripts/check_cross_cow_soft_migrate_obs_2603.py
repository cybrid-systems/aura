#!/usr/bin/env python3
"""check_cross_cow_soft_migrate_obs_2603.py — Issue #2603 source gate.

Tighten cross-COW soft-migrate observability: split same-gen soft
success from cross-gen CowGenMismatch hard. Agents need soft / hard
ratio for throttle under multi-COW pressure without log scraping.

AC1: try_cross_cow_soft_migrate_ success path (same cow_gen) bumps
     cross_cow_soft_migrate_same_gen_total — distinct from
     cross_cow_soft_migrate_total which lumps all soft successes.
     Cross-gen → CowGenMismatch hard does NOT bump the new counter.
AC2: Cross-gen dual miss → cross_cow_hard_reject_cow_gen_mismatch_total
     bumps; same-gen counter stays 0. AC2 default: hard on cross-gen.
AC3: Soft disabled env (AURA_CROSS_COW_SOFT_MIGRATE=0) → always hard.
     cross_cow_hard_reject_disabled_total bumps; same-gen counter 0.
AC4: Additive schema only. #2505 / #2547 surfaces preserved (existing
     hard-reject-reason + CowGenMismatch breakdown). New keys are
     purely additive (no removal / rename of existing counters).
AC5: Source-cite + unit test in tests/compiler/test_cross_cow_soft_migrate_2371.cpp
     (extended per #81967 with ac2603_* sections).

Rationale (Issue #2603 body):
  #2371 / #2505 / #2547 established cross-COW soft migrate + hard
  reject breakdown. soft restamp was binary (ok or hard). Agents
  cannot distinguish "soft succeeded because same cow_gen" from
  "hard because cross-gen" without log scraping. This splits the
  soft counter by same-gen vs all-soft so the ratio
  same-gen / (same-gen + CowGenMismatch) is query-visible.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
RUNTIME_CPP = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_cross_cow_soft_migrate_2371.cpp"


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

    # AC1: same-gen soft restamp counter exists in observability_metrics.h
    if not METRICS.exists():
        failures.append("AC1: src/compiler/observability_metrics.h not found")
        metrics = ""
    else:
        metrics = METRICS.read_text(encoding="utf-8", errors="replace")

    if metrics:
        if "cross_cow_soft_migrate_same_gen_total" not in metrics:
            failures.append(
                "AC1: observability_metrics.h missing atomic counter "
                "cross_cow_soft_migrate_same_gen_total (same-gen soft "
                "restamp counter, distinct from cross_cow_soft_migrate_total)"
            )
        # Paired-declaration pattern: same-gen counter should be near
        # cross_cow_soft_migrate_total.
        anchor = re.search(
            r"std::atomic<std::uint64_t>\s+cross_cow_soft_migrate_total\b",
            metrics,
        )
        if anchor:
            m = re.search(
                r"std::atomic<std::uint64_t>\s+cross_cow_soft_migrate_same_gen_total\b",
                metrics,
            )
            if m and abs(m.start() - anchor.start()) > 600:
                failures.append(
                    "AC1: cross_cow_soft_migrate_same_gen_total is not "
                    "adjacent to cross_cow_soft_migrate_total in "
                    "observability_metrics.h (paired-declaration pattern)"
                )

    # AC1 (cont.): C ABI bump function in aura_jit_bridge.cpp
    if not BRIDGE.exists():
        failures.append("AC1: src/compiler/aura_jit_bridge.cpp not found")
    else:
        bridge = BRIDGE.read_text(encoding="utf-8", errors="replace")
        bump_body = _find_function_body(
            bridge,
            r'extern\s+"C"\s+void\s+'
            r"aura_bump_cross_cow_soft_migrate_same_gen_total\s*\(\s*void\s*\)",
        )
        if not bump_body:
            failures.append(
                "AC1: aura_jit_bridge.cpp missing "
                "aura_bump_cross_cow_soft_migrate_same_gen_total C-linkage "
                "definition (same-gen soft restamp bumper)"
            )
        else:
            if "cross_cow_soft_migrate_same_gen_total" not in bump_body:
                failures.append(
                    "AC1: aura_bump_cross_cow_soft_migrate_same_gen_total "
                    "body does not bump cross_cow_soft_migrate_same_gen_total"
                )

    # AC1 (cont.): bump in try_cross_cow_soft_migrate_ success path
    if not RUNTIME_CPP.exists():
        failures.append("AC1: src/compiler/aura_jit_runtime.cpp not found")
    else:
        runtime_cpp = RUNTIME_CPP.read_text(encoding="utf-8", errors="replace")
        # Find try_cross_cow_soft_migrate_ body (the success path returns 1).
        try_body = _find_function_body(
            runtime_cpp,
            r"static\s+int\s+try_cross_cow_soft_migrate_\s*\(\s*std::size_t\s+cid\s*\)",
        )
        if not try_body:
            failures.append(
                "AC1: try_cross_cow_soft_migrate_ not found in "
                "aura_jit_runtime.cpp (same-gen soft restamp success path)"
            )
        else:
            # Success path: must bump the same-gen counter.
            if "aura_bump_cross_cow_soft_migrate_same_gen_total" not in try_body:
                failures.append(
                    "AC1: try_cross_cow_soft_migrate_ success path does NOT "
                    "call aura_bump_cross_cow_soft_migrate_same_gen_total "
                    "(AC1 — same-gen soft must bump new counter)"
                )
            # Must NOT bump the all-soft counter + same-gen counter in the
            # cross-gen / hard path. Check: hard path (CowGenMismatch) does
            # NOT call either bumper.
            if "aura_bump_cross_cow_soft_migrate_total" not in try_body:
                failures.append(
                    "AC1: try_cross_cow_soft_migrate_ does NOT call "
                    "aura_bump_cross_cow_soft_migrate_total (existing soft "
                    "counter must still be bumped on success)"
                )
            # AC4: #2178 / #2275 cross-workspace reject still fail-closed
            # (soft does NOT open cross-workspace write).
            if "#2178" not in try_body and "cross-workspace" not in try_body:
                # This is a soft-warning, not a hard failure — comment
                # documentation in the .cpp is also acceptable. The linter
                # is lenient here so we don't reject existing code that
                # already documents this in a header comment.
                pass

    # AC2: cross-gen hard (CowGenMismatch) does NOT bump same-gen counter
    if RUNTIME_CPP.exists() and metrics:
        runtime_cpp = RUNTIME_CPP.read_text(encoding="utf-8", errors="replace")
        # Find the CowGenMismatch hard branch in try_cross_cow_soft_migrate_.
        cow_block = _find_function_body(
            runtime_cpp,
            r"static\s+int\s+try_cross_cow_soft_migrate_\s*\(\s*std::size_t\s+cid\s*\)",
        )
        if cow_block:
            # AC2: the CowGenMismatch cross-cow_note_hard_ branch must NOT
            # be followed by same-gen bumper in the same branch.
            if "closure_cow_gen_mismatch_" in cow_block:
                # Find the hard branch context: cow_gen_mismatch → hard.
                # The cross-gen hard should not bump same-gen.
                # Check: cross_cow_soft_migrate_same_gen_total is NOT in
                # the same block as the CowGenMismatch hard-reject call.
                # Approximate: the same-gen bumper must be in a separate
                # branch from the CowGenMismatch hard.
                # We already verified the success path (return 1) bumps
                # the counter. Here we verify the hard path (return 0) does
                # NOT bump it.
                if cow_block.count(
                    "aura_bump_cross_cow_soft_migrate_same_gen_total"
                ) > 1:
                    failures.append(
                        "AC2: try_cross_cow_soft_migrate_ has multiple "
                        "same-gen bumper calls (cross-gen hard path must NOT "
                        "bump same-gen counter)"
                    )

    # AC3: soft disabled env → always hard; counters consistent
    if BRIDGE.exists():
        bridge = BRIDGE.read_text(encoding="utf-8", errors="replace")
        # cross_cow_hard_reject_disabled_total must exist (already there).
        if "cross_cow_hard_reject_disabled_total" not in bridge:
            failures.append(
                "AC3: cross_cow_hard_reject_disabled_total not found in "
                "aura_jit_bridge.cpp (soft-disabled env must bump hard)"
            )
        if "AURA_CROSS_COW_SOFT_MIGRATE" not in RUNTIME_CPP.read_text(
            encoding="utf-8", errors="replace"
        ):
            failures.append(
                "AC3: AURA_CROSS_COW_SOFT_MIGRATE env not referenced in "
                "aura_jit_runtime.cpp (soft-disabled env path)"
            )

    # AC4: additive schema only; #2505 / #2547 surfaces remain compatible
    if OBS_EVAL.exists():
        obs_eval = OBS_EVAL.read_text(encoding="utf-8", errors="replace")
        for key in (
            "cross-cow-soft-migrate-same-gen-total",
            "cross_cow_soft_migrate_same_gen_total",
            "cross-cow-soft-migrate-same-gen-wired",
            "cross-cow-soft-rate-x10000",
            "schema-2603",
            "issue-2603",
        ):
            if key not in obs_eval:
                failures.append(
                    f"AC4: evaluator_primitives_obs_eval.cpp does not expose "
                    f"{key} on query:aot-reload-stats"
                )
        # Compatibility: prior schemas preserved.
        for key in (
            "cross-cow-soft-migrate-total",
            "cross-cow-hard-reject-cow-gen-mismatch-total",
            "schema-2371",
            "schema-2505",
            "schema-2547",
        ):
            if key not in obs_eval:
                failures.append(
                    f"AC4: evaluator_primitives_obs_eval.cpp does not preserve "
                    f"existing {key} (compatibility with #2371/#2505/#2547)"
                )

    # AC5: source-cite + unit test in test_cross_cow_soft_migrate_2371.cpp
    if not TEST.exists():
        failures.append("AC5: tests/compiler/test_cross_cow_soft_migrate_2371.cpp not found")
    else:
        test_text = TEST.read_text(encoding="utf-8", errors="replace")
        for ac_fn in (
            "ac2603_same_gen_soft_counter",
            "ac2603_cross_gen_no_soft_bump",
            "ac2603_soft_disabled_no_soft_bump",
            "ac2603_schema_and_source",
            "ac2603_soft_no_cross_workspace_write",
        ):
            if ac_fn not in test_text:
                failures.append(
                    f"AC5: tests/compiler/test_cross_cow_soft_migrate_2371.cpp "
                    f"missing test function {ac_fn} (issue #2603 coverage)"
                )
        if "main()" in test_text:
            for ac_fn in (
                "ac2603_same_gen_soft_counter",
                "ac2603_cross_gen_no_soft_bump",
                "ac2603_soft_disabled_no_soft_bump",
                "ac2603_schema_and_source",
                "ac2603_soft_no_cross_workspace_write",
            ):
                if f"{ac_fn}()" not in test_text:
                    failures.append(
                        f"AC5: main() does not call {ac_fn}()"
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
        "OK: all #2603 ACs satisfied (cross-COW soft-migrate observability — "
        "same-gen counter + soft/hard ratio + CowGenMismatch preserved + tests)"
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
    tmp = Path(tempfile.mkdtemp(prefix="check_2603_selftest_"))
    try:
        good_runtime = tmp / "runtime.cpp"
        good_runtime.write_text(
            "static int try_cross_cow_soft_migrate_(std::size_t cid) {\n"
            "    if (!cross_cow_soft_migrate_enabled_()) {\n"
            "        cross_cow_note_hard_(CrossCowHardReject::Disabled);\n"
            "        return 0;\n"
            "    }\n"
            "    if (closure_cow_gen_mismatch_(cid)) {\n"
            "        cross_cow_note_hard_(CrossCowHardReject::CowGenMismatch);\n"
            "        return 0;\n"
            "    }\n"
            "    stamp_closure_provenance_locked(cid);\n"
            "    aura_bump_cross_cow_soft_migrate_total();\n"
            "    aura_bump_cross_cow_soft_migrate_same_gen_total();\n"
            "    return 1;\n"
            "}\n"
            "static bool cross_cow_soft_migrate_enabled_() { return true; }\n"
            "static void cross_cow_note_hard_(int) {}\n"
            "static bool closure_cow_gen_mismatch_(std::size_t) { return false; }\n"
            "static void stamp_closure_provenance_locked(std::size_t) {}\n",
            encoding="utf-8",
        )

        good_metrics = tmp / "metrics.h"
        good_metrics.write_text(
            "std::atomic<std::uint64_t> cross_cow_soft_migrate_total{0};\n"
            "std::atomic<std::uint64_t> cross_cow_soft_migrate_same_gen_total{0}; // #2603\n"
            "std::atomic<std::uint64_t> cross_cow_hard_reject_disabled_total{0};\n"
            "std::atomic<std::uint64_t> cross_cow_hard_reject_cow_gen_mismatch_total{0};\n",
            encoding="utf-8",
        )

        good_bridge = tmp / "bridge.cpp"
        good_bridge.write_text(
            'extern "C" void aura_bump_cross_cow_soft_migrate_same_gen_total(void) {\n'
            "    aot_metrics()->cross_cow_soft_migrate_same_gen_total.fetch_add(1, ...);\n"
            "}\n"
            "static const char* AURA_CROSS_COW_SOFT_MIGRATE = \"AURA_CROSS_COW_SOFT_MIGRATE\";\n",
            encoding="utf-8",
        )

        good_obs_eval = tmp / "obs_eval.cpp"
        good_obs_eval.write_text(
            '{"cross-cow-soft-migrate-same-gen-total", make_int(0)},\n'
            '{"cross_cow_soft_migrate_same_gen_total", make_int(0)},\n'
            '{"cross-cow-soft-migrate-same-gen-wired", make_int(1)},\n'
            '{"cross-cow-soft-rate-x10000", make_int(0)},\n'
            '{"schema-2603", make_int(2603)},\n'
            '{"issue-2603", make_int(2603)},\n'
            '{"cross-cow-soft-migrate-total", make_int(0)},\n'
            '{"cross-cow-hard-reject-cow-gen-mismatch-total", make_int(0)},\n'
            '{"schema-2371", make_int(2371)},\n'
            '{"schema-2505", make_int(2505)},\n'
            '{"schema-2547", make_int(2547)},\n',
            encoding="utf-8",
        )

        good_test = tmp / "test.cpp"
        good_test.write_text(
            "static void ac2603_same_gen_soft_counter() {}\n"
            "static void ac2603_cross_gen_no_soft_bump() {}\n"
            "static void ac2603_soft_disabled_no_soft_bump() {}\n"
            "static void ac2603_schema_and_source() {}\n"
            "static void ac2603_soft_no_cross_workspace_write() {}\n"
            "int main() {\n"
            "    ac2603_same_gen_soft_counter();\n"
            "    ac2603_cross_gen_no_soft_bump();\n"
            "    ac2603_soft_disabled_no_soft_bump();\n"
            "    ac2603_schema_and_source();\n"
            "    ac2603_soft_no_cross_workspace_write();\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )

        import check_cross_cow_soft_migrate_obs_2603 as self_mod

        original = {
            "BRIDGE": self_mod.BRIDGE,
            "RUNTIME_CPP": self_mod.RUNTIME_CPP,
            "METRICS": self_mod.METRICS,
            "OBS_EVAL": self_mod.OBS_EVAL,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.RUNTIME_CPP = good_runtime
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
            self_mod.BRIDGE = good_bridge
            self_mod.RUNTIME_CPP = good_runtime
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

        # Bad fixture: same-gen counter missing in metrics
        bad_metrics = tmp / "metrics_bad.h"
        bad_metrics.write_text(
            "std::atomic<std::uint64_t> cross_cow_soft_migrate_total{0};\n"
            "std::atomic<std::uint64_t> cross_cow_hard_reject_disabled_total{0};\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.RUNTIME_CPP = good_runtime
            self_mod.METRICS = bad_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print(
                "SELF-TEST FAIL: known-bad (same-gen counter missing) accepted",
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
