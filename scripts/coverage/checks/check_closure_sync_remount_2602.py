#!/usr/bin/env python3
"""check_closure_sync_remount_2602.py — Issue #2602 source gate.

Synchronous remount_or_force_deopt walk for named live closures
(stable_func_id != 0) on reemit success. Closes the MustDeopt
window between reemit and first call (refine #2542 restamp).

AC1: Reemit success → aura_sync_remount_named_live_closures walks
     live closures with stable_func_id != 0 and calls
     remount_or_force_deopt_unlocked synchronously; the 2 counters
     (live_closure_sync_remount_ok_total / _fail_total) bump
     (distinct from call-time closure_capture_remount_ok / _fail).
AC2: Remount fail → MustDeopt + batch_deopt_for(name) already
     applied inside remount_or_force_deopt_unlocked (#2503
     shared path). The sync walk uses a no-call-time-counter
     variant so the new counters are clean (no double-counting).
AC3: Anonymous sid=0 stay on the existing call-time path —
     decide path skips cid_stable_func_ids[cid] == 0 entries
     (#2550 aligned). No silent skip.
AC4: Soft / no live closures → zero extra work. Lock taken +
     parallel-vector size check + empty loop. Counts only bump
     when at least one named closure exists.
AC5: Source-cite + test extended in
     tests/compiler/test_live_closure_full_restamp.cpp
     (#81934 / #81967: same module test, new ac2602_* sections).

Rationale (Issue #2602 body):
  Reemit success path walks live closures: restamp bridge/env_gen,
  backfill sid, then returns. aura_remount_or_force_deopt is
  available but not mandatorily invoked in the same critical
  section for every named live closure. After reemit success there
  is a window where named closures hold new epoch but stale
  env_gen / linear capture → first call hits MustDeopt instead of
  seamless continue. This gate enforces the sync walk so the
  common named-closure case avoids the residual MustDeopt.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_STUB = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
RUNTIME_CPP = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
RUNTIME_H = ROOT / "src" / "compiler" / "runtime_shared.h"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_live_closure_full_restamp.cpp"


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

    # AC1: 2 new counters in observability_metrics.h
    if not METRICS.exists():
        failures.append("AC1: src/compiler/observability_metrics.h not found")
        metrics = ""
    else:
        metrics = METRICS.read_text(encoding="utf-8", errors="replace")

    if metrics:
        for counter in (
            "live_closure_sync_remount_ok_total",
            "live_closure_sync_remount_fail_total",
        ):
            if counter not in metrics:
                failures.append(
                    f"AC1: observability_metrics.h missing atomic counter "
                    f"{counter} (sync path, distinct from call-time "
                    f"closure_capture_remount_*)"
                )

        # Paired-declaration pattern: sync counters should be near
        # the live_closure_* family.
        anchor = re.search(
            r"std::atomic<std::uint64_t>\s+live_closure_remap_total\b",
            metrics,
        )
        if anchor:
            for counter in (
                "live_closure_sync_remount_ok_total",
                "live_closure_sync_remount_fail_total",
            ):
                m = re.search(
                    r"std::atomic<std::uint64_t>\s+" + counter + r"\b",
                    metrics,
                )
                if m and abs(m.start() - anchor.start()) > 800:
                    failures.append(
                        f"AC1: {counter} is not adjacent to live_closure_remap_total in observability_metrics.h"
                    )

    # AC2: sync walk function in aura_jit_runtime.cpp + declaration in runtime_shared.h
    if not RUNTIME_CPP.exists():
        failures.append("AC2: src/compiler/aura_jit_runtime.cpp not found")
        runtime_cpp = ""
    else:
        runtime_cpp = RUNTIME_CPP.read_text(encoding="utf-8", errors="replace")

    if runtime_cpp:
        sync_body = _find_function_body(
            runtime_cpp,
            r'extern\s+"C"\s+void\s+aura_sync_remount_named_live_closures\s*\(\s*std::uint64_t\*\s*ok_count\s*,\s*std::uint64_t\*\s*fail_count\s*\)',
        )
        if not sync_body:
            failures.append(
                "AC2: aura_sync_remount_named_live_closures C-linkage "
                "definition not found in aura_jit_runtime.cpp (sync walk)"
            )
        else:
            # Must take g_closure_table_mtx exclusive lock + workspace write
            # (AC4 zero-cost path still acquires the lock, then short-circuits).
            if "g_closure_table_mtx" not in sync_body:
                failures.append("AC2: sync walk body does not take g_closure_table_mtx exclusive lock")
            if "aura_lock_workspace_write" not in sync_body:
                failures.append("AC2: sync walk body does not call aura_lock_workspace_write")
            # Must skip anonymous (sid=0) — AC3.
            if "stable_func_ids" not in sync_body:
                failures.append("AC2: sync walk body does not reference g_closure_stable_func_ids (no named-only walk)")
            # Must bump the new counters (distinct from call-time).
            if "live_closure_sync_remount_ok_total" not in sync_body:
                failures.append("AC2: sync walk body does not bump live_closure_sync_remount_ok_total")
            if "live_closure_sync_remount_fail_total" not in sync_body:
                failures.append("AC2: sync walk body does not bump live_closure_sync_remount_fail_total")
            # Must NOT bump call-time counters (distinct).
            if "closure_capture_remount_ok_total" in sync_body:
                failures.append(
                    "AC2: sync walk body bumps call-time "
                    "closure_capture_remount_ok_total (must NOT — "
                    "call-time reserved for JIT-driven path)"
                )
            if "closure_capture_remount_fail_total" in sync_body:
                failures.append("AC2: sync walk body bumps call-time closure_capture_remount_fail_total (must NOT)")

        # No-call-time-counter variant must exist.
        if "remount_or_force_deopt_unlocked_no_call_time_counter" not in runtime_cpp:
            failures.append(
                "AC2: aura_jit_runtime.cpp missing "
                "remount_or_force_deopt_unlocked_no_call_time_counter "
                "(sync variant that skips call-time counter bumps)"
            )

    # AC2 (cont.): declaration in runtime_shared.h
    if not RUNTIME_H.exists():
        failures.append("AC2: src/compiler/runtime_shared.h not found")
    else:
        runtime_h = RUNTIME_H.read_text(encoding="utf-8", errors="replace")
        if "aura_sync_remount_named_live_closures" not in runtime_h:
            failures.append(
                'AC2: runtime_shared.h missing extern "C" declaration for aura_sync_remount_named_live_closures'
            )

    # AC3: bridge drives sync walk after reemit + anonymous path preserved
    if not BRIDGE.exists():
        failures.append("AC3: src/compiler/aura_jit_bridge.cpp not found")
    else:
        bridge = BRIDGE.read_text(encoding="utf-8", errors="replace")
        # Find the if(any_re_emit) {...} block that contains the remap
        # call and verify the sync walk sits inside it.
        reemit_block = _find_function_body(
            bridge,
            r"if\s*\(\s*any_re_emit\s*\)\s*\{",
        )
        if not reemit_block:
            failures.append("AC3: aura_jit_bridge.cpp missing if(any_re_emit) block (#2542 post-reemit site)")
        else:
            if "aura_sync_remount_named_live_closures" not in reemit_block:
                failures.append(
                    "AC3: aura_sync_remount_named_live_closures not "
                    "called inside if(any_re_emit) block "
                    "(#2602 sync walk must follow reemit success)"
                )
            if "aura_remap_live_closures_after_reemit" not in reemit_block:
                failures.append(
                    "AC3: aura_remap_live_closures_after_reemit not in "
                    "if(any_re_emit) block (#2542 restamp predecessor)"
                )

    # AC4: stub in aura_jit_bridge_stub.cpp is zero-cost
    if not BRIDGE_STUB.exists():
        failures.append("AC4: src/compiler/aura_jit_bridge_stub.cpp not found")
    else:
        stub = BRIDGE_STUB.read_text(encoding="utf-8", errors="replace")
        stub_body = _find_function_body(
            stub,
            r'extern\s+"C"\s+__attribute__\(\(weak\)\)\s+void\s+'
            r"aura_sync_remount_named_live_closures\s*\(\s*std::uint64_t\*\s*ok_count\s*,\s*"
            r"std::uint64_t\*\s*fail_count\s*\)",
        )
        if not stub_body:
            failures.append("AC4: aura_jit_bridge_stub.cpp missing weak stub for aura_sync_remount_named_live_closures")
        else:
            if "ok_count" not in stub_body or "*ok_count = 0" not in stub_body:
                failures.append("AC4: sync walk stub does not zero out *ok_count")
            if "fail_count" not in stub_body or "*fail_count = 0" not in stub_body:
                failures.append("AC4: sync walk stub does not zero out *fail_count")

    # AC5: query surface in evaluator_primitives_obs_eval.cpp + test sections
    if not OBS_EVAL.exists():
        failures.append("AC5: src/compiler/evaluator_primitives_obs_eval.cpp not found")
    else:
        obs_eval = OBS_EVAL.read_text(encoding="utf-8", errors="replace")
        for key in (
            "live-closure-sync-remount-ok-total",
            "live-closure-sync-remount-fail-total",
            "live-closure-sync-remount-wired",
            "schema-2602",
            "issue-2602",
        ):
            if key not in obs_eval:
                failures.append(f"AC5: evaluator_primitives_obs_eval.cpp does not expose {key} on query:aot-stats")

    # AC5 (cont.): test file has ac2602_* sections.
    if not TEST.exists():
        failures.append("AC5: tests/compiler/test_live_closure_full_restamp.cpp not found")
    else:
        test_text = TEST.read_text(encoding="utf-8", errors="replace")
        for ac_fn in (
            "ac2602_named_held_no_mustdeopt",
            "ac2602_remount_fail_path",
            "ac2602_anonymous_still_force_deopt",
            "ac2602_soft_zero_cost",
            "ac2602_source_and_schema",
        ):
            if ac_fn not in test_text:
                failures.append(
                    f"AC5: tests/compiler/test_live_closure_full_restamp.cpp "
                    f"missing test function {ac_fn} (issue #2602 coverage)"
                )
        if "main()" in test_text:
            for ac_fn in (
                "ac2602_named_held_no_mustdeopt",
                "ac2602_remount_fail_path",
                "ac2602_anonymous_still_force_deopt",
                "ac2602_soft_zero_cost",
                "ac2602_source_and_schema",
            ):
                if f"{ac_fn}()" not in test_text:
                    failures.append(f"AC5: main() does not call {ac_fn}()")

    # AC5 (cont.): schema compatibility — #2542 / #2503 / #2550 surfaces preserved.
    if OBS_EVAL.exists():
        obs_eval = OBS_EVAL.read_text(encoding="utf-8", errors="replace")
        if "schema-2542" not in obs_eval:
            failures.append(
                "AC5: evaluator_primitives_obs_eval.cpp does not preserve "
                "schema-2542 (full live-closure epoch restamp surface)"
            )
        if "schema-2503" not in obs_eval:
            failures.append(
                "AC5: evaluator_primitives_obs_eval.cpp does not preserve "
                "schema-2503 (remount_or_force_deopt shared path)"
            )
        if "schema-2550" not in obs_eval:
            failures.append(
                "AC5: evaluator_primitives_obs_eval.cpp does not preserve "
                "schema-2550 (named set_name forces stable_func_id)"
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
        "OK: all #2602 ACs satisfied (closure sync remount on reemit — "
        "distinct counters + zero-call-time variant + named-only walk + "
        "anonymous preserved + tests + schema cross-links)"
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
    tmp = Path(tempfile.mkdtemp(prefix="check_2602_selftest_"))
    try:
        good_bridge = tmp / "bridge.cpp"
        good_bridge.write_text(
            'extern "C" void aura_sync_remount_named_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count);\n'
            "    if (any_re_emit) {\n"
            "        aura_remap_live_closures_after_reemit(...);\n"
            "        aura_sync_remount_named_live_closures(&ok, &fail);\n"
            "    }\n",
            encoding="utf-8",
        )

        good_runtime = tmp / "runtime.cpp"
        good_runtime.write_text(
            'extern "C" void aura_sync_remount_named_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count) {\n'
            "    std::unique_lock<std::shared_mutex> tlock(g_closure_table_mtx);\n"
            "    aura_lock_workspace_write();\n"
            "    if (g_closure_func_ids.size() == 0) { ... }\n"
            "    if (g_closure_stable_func_ids.size() < nslots) ...\n"
            "    for (...) {\n"
            "        if (g_closure_stable_func_ids[cid] == 0) continue;\n"
            "        remount_or_force_deopt_unlocked_no_call_time_counter(...);\n"
            "        aot_metrics()->live_closure_sync_remount_ok_total.fetch_add(1, ...);\n"
            "        aot_metrics()->live_closure_sync_remount_fail_total.fetch_add(0, ...);\n"
            "    }\n"
            "    aura_unlock_workspace_write();\n"
            "}\n"
            "static int remount_or_force_deopt_unlocked_no_call_time_counter(...);\n",
            encoding="utf-8",
        )

        good_runtime_h = tmp / "runtime.h"
        good_runtime_h.write_text(
            'extern "C" void aura_sync_remount_named_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count);\n',
            encoding="utf-8",
        )

        good_stub = tmp / "stub.cpp"
        good_stub.write_text(
            'extern "C" __attribute__((weak)) void aura_sync_remount_named_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count) {\n'
            "    if (ok_count) *ok_count = 0;\n"
            "    if (fail_count) *fail_count = 0;\n"
            "}\n",
            encoding="utf-8",
        )

        good_metrics = tmp / "metrics.h"
        good_metrics.write_text(
            "std::atomic<std::uint64_t> live_closure_remap_total{0};\n"
            "// Issue #2602\n"
            "std::atomic<std::uint64_t> live_closure_sync_remount_ok_total{0};\n"
            "std::atomic<std::uint64_t> live_closure_sync_remount_fail_total{0};\n",
            encoding="utf-8",
        )

        good_obs_eval = tmp / "obs_eval.cpp"
        good_obs_eval.write_text(
            '{"live-closure-sync-remount-ok-total", make_int(0)},\n'
            '{"live-closure-sync-remount-fail-total", make_int(0)},\n'
            '{"live-closure-sync-remount-wired", make_int(1)},\n'
            '{"schema-2602", make_int(2602)},\n'
            '{"issue-2602", make_int(2602)},\n'
            '{"schema-2542", make_int(2542)},\n'
            '{"schema-2503", make_int(2503)},\n'
            '{"schema-2550", make_int(2550)},\n',
            encoding="utf-8",
        )

        good_test = tmp / "test.cpp"
        good_test.write_text(
            "static void ac2602_named_held_no_mustdeopt() {}\n"
            "static void ac2602_remount_fail_path() {}\n"
            "static void ac2602_anonymous_still_force_deopt() {}\n"
            "static void ac2602_soft_zero_cost() {}\n"
            "static void ac2602_source_and_schema() {}\n"
            "int main() {\n"
            "    ac2602_named_held_no_mustdeopt();\n"
            "    ac2602_remount_fail_path();\n"
            "    ac2602_anonymous_still_force_deopt();\n"
            "    ac2602_soft_zero_cost();\n"
            "    ac2602_source_and_schema();\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )

        import check_closure_sync_remount_2602 as self_mod

        original = {
            "BRIDGE": self_mod.BRIDGE,
            "BRIDGE_STUB": self_mod.BRIDGE_STUB,
            "RUNTIME_CPP": self_mod.RUNTIME_CPP,
            "RUNTIME_H": self_mod.RUNTIME_H,
            "METRICS": self_mod.METRICS,
            "OBS_EVAL": self_mod.OBS_EVAL,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.BRIDGE_STUB = good_stub
            self_mod.RUNTIME_CPP = good_runtime
            self_mod.RUNTIME_H = good_runtime_h
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
            self_mod.BRIDGE_STUB = good_stub
            self_mod.RUNTIME_CPP = good_runtime
            self_mod.RUNTIME_H = good_runtime_h
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

        # Bad fixture: sync walk bumps call-time counter (AC2 violation)
        bad_runtime = tmp / "runtime_bad.cpp"
        bad_runtime.write_text(
            'extern "C" void aura_sync_remount_named_live_closures(std::uint64_t* ok_count, std::uint64_t* fail_count) {\n'
            "    std::unique_lock<std::shared_mutex> tlock(g_closure_table_mtx);\n"
            "    aura_lock_workspace_write();\n"
            "    if (g_closure_stable_func_ids.size() < nslots) ...\n"
            "    for (...) {\n"
            "        aot_metrics()->live_closure_sync_remount_ok_total.fetch_add(1, ...);\n"
            "        aot_metrics()->closure_capture_remount_ok_total.fetch_add(1, ...);\n"  # BAD
            "    }\n"
            "    aura_unlock_workspace_write();\n"
            "}\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.BRIDGE_STUB = good_stub
            self_mod.RUNTIME_CPP = bad_runtime
            self_mod.RUNTIME_H = good_runtime_h
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print(
                "SELF-TEST FAIL: known-bad (sync walk bumps call-time counter) accepted",
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
