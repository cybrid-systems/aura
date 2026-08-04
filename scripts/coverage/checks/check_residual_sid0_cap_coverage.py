#!/usr/bin/env python3
"""check_residual_sid0_cap_coverage.py — Issue #2638 source gate.

Residual sid=0 growth hard cap + fail-closed drop/MustDeopt under
sustained reemit. Clamps the #2605 residual one-shot backfill so
the named-residual path cannot invent unlimited new ids when
long-running self-mod + reemit + occasional name-fallback edge
cases produce sustained residual growth.

AC1: Cap reached → no further invent; MustDeopt + cap-hit counter
AC2: Below cap → existing one-shot backfill still works (#2605)
AC3: Soft / Off + cap=0 → unlimited (env "0"/"off"/"unlimited")
AC4: Named create path (sid≠0) never hits residual cap
AC5: Query keys + schema-2638 + wired sentinel; #2605 axes preserved
AC6: src-aligned soak (no residual growth under normal) + inject-over-cap
     test + coverage gate (this linter + build.py gate step)

Default: non-strict (exit 0, prints coverage summary). Use --strict
to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_STUB = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
RUNTIME_CPP = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST_2605 = ROOT / "tests" / "compiler" / "test_anonymous_residual_stable_id_policy_2605.cpp"
CMAKE = ROOT / "CMakeLists.txt"
BUILD = ROOT / "build.py"


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

    # AC1: cap env flag default + cap check in residual branch
    must_present(RUNTIME_CPP, "Issue #2638", "AC1: runtime cites #2638")
    must_present(RUNTIME_CPP, "AURA_RESIDUAL_SID0_CAP", "AC1: env var name present")
    must_present(RUNTIME_CPP, "aura_residual_sid0_cap_default", "AC1: env flag resolver called")
    must_present(
        RUNTIME_CPP,
        "aura_bump_live_closure_residual_cap_hit_total",
        "AC1: cap-hit bumper called from residual branch",
    )
    must_present(
        RUNTIME_CPP,
        "aura_closure_set_must_deopt",
        "AC1: MustDeopt forced on cap-hit",
    )
    must_present(
        RUNTIME_CPP,
        "continue",
        "AC1: cap-hit path skips backfill (continue)",
    )

    runtime_text = RUNTIME_CPP.read_text(encoding="utf-8", errors="replace") if RUNTIME_CPP.exists() else ""
    env_body = _find_function_body(
        runtime_text,
        r'extern\s+"C"\s+std::uint64_t\s+aura_residual_sid0_cap_default\s*\(\s*\)',
    )
    if not env_body:
        failures.append("AC1: aura_residual_sid0_cap_default strong def not found in aura_jit_runtime.cpp")
    else:
        # AC1: default 256 production-safe
        if "return 256" not in env_body:
            failures.append("AC1: env flag resolver does not default to 256 (production-safe)")
        # AC3: cap=0 → unlimited
        if "return 0" not in env_body:
            failures.append("AC3: env flag resolver does not handle cap=0 (unlimited)")

    # AC1 + AC2: cap check sits inside the residual branch (sid == 0 && named)
    residual_block = re.search(
        r"if\s*\(\s*cid_stable_id\s*==\s*0\s*&&\s*named\s*\)\s*\{",
        runtime_text,
    )
    if not residual_block:
        failures.append("AC2: residual branch `if (cid_stable_id == 0 && named)` not found in remap walk")
    else:
        # Find the matching closing brace from the residual block open
        # to verify cap check is inside it (not before the block opens).
        depth = 0
        i = residual_block.end() - 1  # position of '{'
        block_end = -1
        while i < len(runtime_text):
            c = runtime_text[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    block_end = i
                    break
            i += 1
        if block_end < 0:
            failures.append("AC2: residual branch closing brace not found")
        else:
            block_body = runtime_text[residual_block.end() : block_end]
            if "cap_2638" not in block_body:
                failures.append("AC2: cap check not inside residual branch body")
            if "cur_backfill >= cap_2638" not in block_body:
                failures.append("AC1: cap check condition `cur_backfill >= cap_2638` not found in residual branch")

    # AC1: bump hook strong def in bridge.cpp + weak stubs in stub.cpp
    must_present(
        BRIDGE,
        "aura_bump_live_closure_residual_cap_hit_total",
        "AC1: cap-hit bumper strong def in bridge.cpp",
    )
    must_present(
        BRIDGE,
        "aura_residual_sid0_cap_default",
        "AC1: env flag weak decl in bridge.cpp",
    )

    stub_text = BRIDGE_STUB.read_text(encoding="utf-8", errors="replace") if BRIDGE_STUB.exists() else ""
    if not stub_text:
        failures.append("AC4: aura_jit_bridge_stub.cpp not found")
    else:
        if not re.search(
            r"aura_bump_live_closure_residual_cap_hit_total\s*\(",
            stub_text,
        ):
            failures.append("AC4: cap-hit bumper weak stub missing in bridge_stub.cpp")
        if not re.search(
            r"aura_residual_sid0_cap_default\s*\(",
            stub_text,
        ):
            failures.append("AC4: env flag weak stub missing in bridge_stub.cpp")

    # AC5: 2 new counters in observability_metrics.h
    metrics_text = METRICS.read_text(encoding="utf-8", errors="replace") if METRICS.exists() else ""
    for counter in ("live_closure_residual_cap_hit_total",):
        if counter not in metrics_text:
            failures.append(f"AC5: observability_metrics.h missing atomic counter {counter}")
    # Adjacent to live_closure_stable_id_backfill_total (same family)
    anchor_m = re.search(
        r"std::atomic<std::uint64_t>\s+live_closure_stable_id_backfill_total\b",
        metrics_text,
    )
    if anchor_m:
        m = re.search(
            r"std::atomic<std::uint64_t>\s+live_closure_residual_cap_hit_total\b",
            metrics_text,
        )
        if m and abs(m.start() - anchor_m.start()) > 800:
            failures.append(
                "AC5: live_closure_residual_cap_hit_total is not adjacent to live_closure_stable_id_backfill_total"
            )

    # AC5: query surface in evaluator_primitives_obs_eval.cpp
    obs_text = OBS_EVAL.read_text(encoding="utf-8", errors="replace") if OBS_EVAL.exists() else ""
    for key in (
        "live-closure-residual-cap-hit-total",
        "live-closure-residual-sid0-cap",
        "live-closure-residual-cap-wired",
        "schema-2638",
        "issue-2638",
    ):
        if key not in obs_text:
            failures.append(f"AC5: evaluator_primitives_obs_eval.cpp does not expose {key}")
    # #2605 / #2637 / #2602 axes preserved
    for preserved in (
        "schema-2605",
        "schema-2637",
        "schema-2602",
        "stable-id-residual-backfill-total",
    ):
        if preserved not in obs_text:
            failures.append(f"AC5: evaluator_primitives_obs_eval.cpp does not preserve {preserved}")

    # AC6: tests + build.py gate
    test_text = TEST_2605.read_text(encoding="utf-8", errors="replace") if TEST_2605.exists() else ""
    for ac_fn in (
        "ac2638_cap_below_threshold_backfill_works",
        "ac2638_cap_above_threshold_force_must_deopt",
        "ac2638_named_sid_nonzero_skips_cap",
        "ac2638_source_and_schema_cite",
    ):
        if ac_fn not in test_text:
            failures.append(f"AC6: test missing {ac_fn}")
    if "main()" in test_text:
        for ac_fn in (
            "ac2638_cap_below_threshold_backfill_works",
            "ac2638_cap_above_threshold_force_must_deopt",
            "ac2638_named_sid_nonzero_skips_cap",
            "ac2638_source_and_schema_cite",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: main() does not call {ac_fn}()")

    build_text = BUILD.read_text(encoding="utf-8", errors="replace") if BUILD.exists() else ""
    if "check_residual_sid0_cap_coverage" not in build_text:
        failures.append("AC6: build.py does not reference check_residual_sid0_cap_coverage linter")
    if "cmd_residual_sid0_cap_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_residual_sid0_cap_coverage function")

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
        "OK: all #2638 ACs satisfied (residual sid=0 growth hard cap + "
        "fail-closed drop/MustDeopt — env-gated AURA_RESIDUAL_SID0_CAP, "
        "distinct cap-hit counter, named-path skip, #2605 axes preserved)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
