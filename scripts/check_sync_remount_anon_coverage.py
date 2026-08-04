#!/usr/bin/env python3
"""check_sync_remount_anon_coverage.py — Issue #2637 source gate.

Env-gated sync remount_or_force_deopt walk for anonymous / residual
closures (sid == 0) on reemit success. Mirrors #2602 named sync
path on the opposite sid branch. Closes the first-call MustDeopt
window for anon / residual closures under AURA_SYNC_REMOUNT_ANON=1
(default OFF per AC1; preserves today's call-time MustDeopt for anon
when unset).

AC1: Default (knob off) → named path unchanged; anonymous still
     call-time MustDeopt. Verified by the env flag default 0 in
     aura_sync_remount_anon_enabled_default strong def.
AC2: Knob on + reemit + live anonymous → first subsequent call does
     NOT surprise-MustDeopt (either remounted via shared #2503
     path or already forced inside remount_or_force_deopt).
AC3: Distinct anon counters + wired sentinel + schema-2637.
AC4: Soft / Off + knob off → zero extra work. nslots==0 short-circuit
     matches named path pattern. Call site gates before the inner loop.
AC5: #2602 / #2605 / #2550 / #2542 surfaces and tests still green
     (linter verifies schema-* keys preserved).
AC6: Coverage gate (this linter + build.py gate step + src-aligned
     test in test_anonymous_residual_stable_id_policy_2605.cpp).

Default: non-strict (exit 0, prints coverage summary). Use --strict
to enforce (exit 1 if any AC fails — gate before merge).

Rationale (Issue #2637 body):
  Under high-frequency mutation + reemit, the first call on an
  anonymous / residual closure can still hit MustDeopt → interpreter
  fallback, producing observable first-call jitter even when the
  reemit itself succeeded. The named sync walk (#2602) closes this
  window for stable_func_id != 0 closures; #2637 extends the same
  contract to sid == 0 via an opt-in env knob (default off to
  preserve the conservative call-time MustDeopt policy #2550/#2605).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_STUB = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
RUNTIME_CPP = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
RUNTIME_H = ROOT / "src" / "compiler" / "runtime_shared.h"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST_2605 = ROOT / "tests" / "compiler" / "test_anonymous_residual_stable_id_policy_2605.cpp"


def _extract_body(text: str, open_idx: int) -> str:
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

    def must_present(path: Path, needle: str, label: str) -> None:
        if not path.exists():
            failures.append(f"{label}: {path} not found")
            return
        text = path.read_text(encoding="utf-8", errors="replace")
        if needle not in text:
            failures.append(f"{label}: missing {needle!r} in {path.name}")

    # AC1: env flag default 0 in strong def
    must_present(RUNTIME_CPP, "Issue #2637", "AC1: runtime cites #2637")
    must_present(
        RUNTIME_CPP,
        "aura_sync_remount_anon_enabled_default",
        "AC1: env flag resolver present in runtime.cpp",
    )
    runtime_cpp_text = RUNTIME_CPP.read_text(encoding="utf-8", errors="replace") if RUNTIME_CPP.exists() else ""
    env_body = _find_function_body(
        runtime_cpp_text,
        r'extern\s+"C"\s+int\s+aura_sync_remount_anon_enabled_default\s*\(\s*\)',
    )
    if not env_body:
        failures.append("AC1: aura_sync_remount_anon_enabled_default strong def not found")
    else:
        if "AURA_SYNC_REMOUNT_ANON" not in env_body:
            failures.append("AC1: env flag resolver does not read AURA_SYNC_REMOUNT_ANON env")
        if "return 0" not in env_body:
            failures.append("AC1: env flag resolver does not default to 0 (off) when env unset")

    # AC2: anon sync walk function in aura_jit_runtime.cpp + declaration in runtime_shared.h
    anon_body = _find_function_body(
        runtime_cpp_text,
        r'extern\s+"C"\s+void\s+aura_sync_remount_anon_live_closures\s*\(\s*std::uint64_t\*\s*ok_count\s*,\s*std::uint64_t\*\s*fail_count\s*\)',
    )
    if not anon_body:
        failures.append(
            "AC2: aura_sync_remount_anon_live_closures C-linkage definition not found in aura_jit_runtime.cpp"
        )
    else:
        # AC4 zero-cost: lock + nslots==0 short-circuit
        if "g_closure_table_mtx" not in anon_body:
            failures.append("AC4: anon sync walk body does not take g_closure_table_mtx exclusive lock")
        if "aura_lock_workspace_write" not in anon_body:
            failures.append("AC4: anon sync walk body does not call aura_lock_workspace_write")
        if "nslots == 0" not in anon_body:
            failures.append("AC4: anon sync walk body missing nslots==0 short-circuit (zero-cost)")
        # AC2: filters on sid != 0 means we keep sid == 0 (anonymous branch)
        if "sid != 0" not in anon_body:
            failures.append("AC2: anon sync walk body does not filter sid != 0 (skip named — keep sid == 0 anon)")
        if "g_closure_stable_func_ids" not in anon_body:
            failures.append("AC2: anon sync walk body does not reference g_closure_stable_func_ids (no sid-based walk)")
        # AC3: distinct anon counters (not named) — accept either direct bump
        # or the bumper helper call (which transitively bumps the counters).
        # The named path uses the helper too (#2602), so we mirror that pattern.
        if (
            "live_closure_sync_remount_anon_ok_total" not in anon_body
            and "aura_bump_live_closure_sync_remount_anon_totals" not in anon_body
        ):
            failures.append(
                "AC3: anon sync walk body does not bump "
                "live_closure_sync_remount_anon_ok_total "
                "(no direct bump or bumper helper call)"
            )
        if (
            "live_closure_sync_remount_anon_fail_total" not in anon_body
            and "aura_bump_live_closure_sync_remount_anon_totals" not in anon_body
        ):
            failures.append(
                "AC3: anon sync walk body does not bump "
                "live_closure_sync_remount_anon_fail_total "
                "(no direct bump or bumper helper call)"
            )
        # AC2: shared helper
        if "remount_or_force_deopt_unlocked_no_call_time_counter" not in anon_body:
            failures.append(
                "AC2: anon sync walk body does not call remount_or_force_deopt_unlocked_no_call_time_counter"
            )

    must_present(RUNTIME_H, "aura_sync_remount_anon_live_closures", "AC2: runtime_shared.h decl")
    must_present(RUNTIME_H, "Issue #2637", "AC2: runtime_shared.h cites #2637")

    # AC1+AC2: bridge drives sync walk + env flag weak decl
    bridge_text = BRIDGE.read_text(encoding="utf-8", errors="replace") if BRIDGE.exists() else ""
    if bridge_text:
        # Weak decl for env flag (no __attribute__((weak)) required since runtime provides it,
        # but bridge uses the weak-C pattern to handle non-evaluator builds)
        if "aura_sync_remount_anon_enabled_default" not in bridge_text:
            failures.append("AC1: bridge.cpp does not reference aura_sync_remount_anon_enabled_default")
        if "AURA_SYNC_REMOUNT_ANON" not in bridge_text and "anon_enabled_default" not in bridge_text:
            failures.append("AC1: bridge.cpp does not gate anon sync walk on env flag (AURA_SYNC_REMOUNT_ANON)")
        # AC2: bridge calls anon sync walk after named sync
        reemit_block = _find_function_body(bridge_text, r"if\s*\(\s*any_re_emit\s*\)\s*\{")
        if not reemit_block:
            failures.append("AC1: bridge.cpp missing if(any_re_emit) block (#2542 post-reemit site)")
        else:
            if "aura_sync_remount_named_live_closures" not in reemit_block:
                failures.append("AC2: named sync walk not inside if(any_re_emit) block")
            if "aura_sync_remount_anon_live_closures" not in reemit_block:
                failures.append(
                    "AC2: anon sync walk not called inside if(any_re_emit) block "
                    "(must follow named sync to close anon first-call window)"
                )

        # AC3: bump hook strong def in bridge.cpp (mirror of #2602 hook)
        if "aura_bump_live_closure_sync_remount_anon_totals" not in bridge_text:
            failures.append("AC3: bridge.cpp missing aura_bump_live_closure_sync_remount_anon_totals strong def")

    # AC4: stub in aura_jit_bridge_stub.cpp is zero-cost
    stub_text = BRIDGE_STUB.read_text(encoding="utf-8", errors="replace") if BRIDGE_STUB.exists() else ""
    if not stub_text:
        failures.append("AC4: aura_jit_bridge_stub.cpp not found")
    else:
        # Anonymous sync walk stub
        anon_stub_body = _find_function_body(
            stub_text,
            r'extern\s+"C"\s+__attribute__\(\(weak\)\)\s+void\s+'
            r"aura_sync_remount_anon_live_closures\s*\(\s*std::uint64_t\*\s*ok_count\s*,\s*"
            r"std::uint64_t\*\s*fail_count\s*\)",
        )
        if not anon_stub_body:
            failures.append("AC4: aura_jit_bridge_stub.cpp missing weak stub for aura_sync_remount_anon_live_closures")
        else:
            if "*ok_count = 0" not in anon_stub_body:
                failures.append("AC4: anon sync walk stub does not zero out *ok_count")
            if "*fail_count = 0" not in anon_stub_body:
                failures.append("AC4: anon sync walk stub does not zero out *fail_count")

        # Bumper weak stub — accept either single-line or multi-line
        # declaration (function name is the authoritative marker).
        if not re.search(
            r"aura_bump_live_closure_sync_remount_anon_totals\s*\(",
            stub_text,
        ):
            failures.append(
                "AC4: aura_jit_bridge_stub.cpp missing weak stub for "
                "aura_bump_live_closure_sync_remount_anon_totals (anon bumper)"
            )

        # Env flag weak stub (returns 0 = off)
        env_stub_body = _find_function_body(
            stub_text,
            r'extern\s+"C"\s+__attribute__\(\(weak\)\)\s+int\s+'
            r"aura_sync_remount_anon_enabled_default\s*\(\s*\)",
        )
        if not env_stub_body:
            failures.append(
                "AC4: aura_jit_bridge_stub.cpp missing weak stub for aura_sync_remount_anon_enabled_default"
            )
        elif "return 0" not in env_stub_body:
            failures.append("AC4: env flag weak stub does not return 0 (default off)")

    # AC3: 2 new counters in observability_metrics.h
    metrics_text = METRICS.read_text(encoding="utf-8", errors="replace") if METRICS.exists() else ""
    for counter in (
        "live_closure_sync_remount_anon_ok_total",
        "live_closure_sync_remount_anon_fail_total",
    ):
        if counter not in metrics_text:
            failures.append(
                f"AC3: observability_metrics.h missing atomic counter {counter} "
                f"(anon sync path, distinct from named live_closure_sync_remount_*)"
            )
    # Verify counters are adjacent to named ones (within ~30 lines)
    anchor_m = re.search(
        r"std::atomic<std::uint64_t>\s+live_closure_sync_remount_ok_total\b",
        metrics_text,
    )
    if anchor_m:
        for counter in (
            "live_closure_sync_remount_anon_ok_total",
            "live_closure_sync_remount_anon_fail_total",
        ):
            m = re.search(
                r"std::atomic<std::uint64_t>\s+" + counter + r"\b",
                metrics_text,
            )
            if m and abs(m.start() - anchor_m.start()) > 800:
                failures.append(
                    f"AC3: {counter} is not adjacent to live_closure_sync_remount_ok_total in observability_metrics.h"
                )

    # AC5: query surface in evaluator_primitives_obs_eval.cpp
    obs_text = OBS_EVAL.read_text(encoding="utf-8", errors="replace") if OBS_EVAL.exists() else ""
    for key in (
        "live-closure-sync-remount-anon-ok-total",
        "live-closure-sync-remount-anon-fail-total",
        "live-closure-sync-remount-anon-wired",
        "schema-2637",
        "issue-2637",
    ):
        if key not in obs_text:
            failures.append(f"AC5: evaluator_primitives_obs_eval.cpp does not expose {key}")
    # Schema compatibility — #2602 / #2605 / #2550 / #2542 surfaces preserved
    for preserved in ("schema-2602", "schema-2605", "schema-2550", "live-closure-sync-remount-wired"):
        if preserved not in obs_text:
            failures.append(
                f"AC5: evaluator_primitives_obs_eval.cpp does not preserve {preserved} (#2637 must not regress prior surfaces)"
            )

    # AC6: test + build.py
    test_text = TEST_2605.read_text(encoding="utf-8", errors="replace") if TEST_2605.exists() else ""
    for ac_fn in (
        "ac2637_anon_sync_off_default",
        "ac2637_schema_and_source_cite",
    ):
        if ac_fn not in test_text:
            failures.append(f"AC6: test_anonymous_residual_stable_id_policy_2605.cpp missing {ac_fn}")
    if "main()" in test_text:
        for ac_fn in (
            "ac2637_anon_sync_off_default",
            "ac2637_schema_and_source_cite",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: main() does not call {ac_fn}()")

    build_text = (ROOT / "build.py").read_text(encoding="utf-8", errors="replace")
    if "check_sync_remount_anon_coverage" not in build_text:
        failures.append("AC6: build.py does not reference check_sync_remount_anon_coverage linter")
    if "cmd_sync_remount_anon_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_sync_remount_anon_coverage function")

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
        "OK: all #2637 ACs satisfied (anon / residual sync remount on reemit — "
        "env-gated, distinct counters, no named-path regression, "
        "soft zero-cost when knob off, schema cross-links preserved)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
