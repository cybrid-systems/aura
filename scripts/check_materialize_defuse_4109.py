#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4109: live-body closures skip the defuse-behind empty env and
# keep pre-mutate bindings. materialize_call_env keyed the behind/INVALID
# empty-Env fallback on body_live — a live body node is NOT a proof the
# capture is current, so any Guard defuse bump copied stale bindings into
# the next call. Fix shape:
#   1. materialize_call_env returns the empty Env for EVERY behind or
#      INVALID_VERSION frame (no body_live keying on that arm).
#   2. The env_gen_stamp_ mismatch arm returns the empty Env on every
#      mismatch (a live body id is not a proof the mismatch is not a
#      compact of this frame). Compact + truncate restamp survivor
#      env_gen_stamp_ in the same critical section as the env_generation_
#      bump so frames they kept do not strand.
#   3. enter/exit_mutation_boundary ride still-valid capture frames up to
#      the new defuse (restamp_live_capture_frames): frames whose captured
#      cell still matches the live workspace binding for that name (module
#      captures, #2579) plus frame-local captures. Frames whose captured
#      cell was replaced by the boundary (mutate:rebind / set-code new
#      cell) stay behind — the next materialize returns the empty Env
#      instead of copying pre-mutate bindings.
#
# AC1 — materialize behind arm: `if (terminal || behind)` present in
#       src/compiler/evaluator_env.cpp; the old `behind && !body_live`
#       exemption shape is gone; the arm cites #4109.
# AC2 — env_gen fence arm: the old `!body_live || env_terminal` condition
#       is gone; the arm cites #4109 (live body id is not the proof).
# AC3 — ride-up wired: restamp_live_capture_frames defined in
#       evaluator_env.cpp (cites #4109 + #2579, INVALID_VERSION never
#       restamped) and called from enter + exit_mutation_boundary.
# AC4 — compact/truncate restamp survivor env_gen_stamp_ in the same
#       critical section as the env_generation_ bump (cites #4109).
# AC5 — tests pin the contract: tests/compiler/test_module_rebind_residual.cpp
#       carries #4109 (AC6/AC7/AC8) and tests/compiler/test_envframe_epoch_batch.cpp
#       carries the behind live-body materialize checks (#4072 home).
# AC6 — wiring: build.py registers check_materialize_defuse_4109 and
#       scripts/coverage/root_check_allowlist.txt lists it.
# AC7 — no docs/design/4109-* (banned per #1655) and no
#       tests/compiler/test_issue_4109.cpp (extend the existing test
#       files per #81934).
#
# Self-test:
#   python3 scripts/check_materialize_defuse_4109.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks (does not need to handle raw strings /
    trigraphs).
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def main() -> int:
    fails: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    env_path = ROOT / "src" / "compiler" / "evaluator_env.cpp"
    env_src = env_path.read_text(encoding="utf-8")
    env_code = _strip_cpp_comments(env_src)

    # AC1 — behind arm: every behind/INVALID frame takes the empty Env.
    check("if (terminal || behind)" in env_code, "AC1: materialize behind arm must be `if (terminal || behind)`")
    check("behind && !body_live" not in env_code, "AC1: old `behind && !body_live` exemption must be gone")
    check("Issue #4109" in env_src, "AC1: evaluator_env.cpp must cite Issue #4109 at the behind arm")

    # AC2 — env_gen fence arm: every mismatch takes the empty Env.
    check("!body_live || env_terminal" not in env_code, "AC2: old `!body_live || env_terminal` exemption must be gone")
    check(env_src.count("#4109") >= 2, "AC2: env_gen fence arm must cite #4109 (live body id is not the proof)")

    # AC3 — ride-up helper defined + wired into the boundary bumps.
    check(
        "restamp_live_capture_frames" in env_code,
        "AC3: restamp_live_capture_frames must be defined in evaluator_env.cpp",
    )
    check("Issue #4109: ride still-valid capture frames" in env_src, "AC3: ride-up helper must cite #4109 rationale")
    check(
        "fr.version_ == INVALID_VERSION || fr.version_ >= cur_defuse" in env_code,
        "AC3: ride-up must skip INVALID_VERSION frames (#356 terminal rule)",
    )
    boundary_path = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
    boundary_src = boundary_path.read_text(encoding="utf-8")
    boundary_code = _strip_cpp_comments(boundary_src)
    calls = boundary_code.count("restamp_live_capture_frames(")
    check(calls >= 2, "AC3: restamp must ride at BOTH enter and exit boundary bumps")
    check("Issue #4109" in boundary_src, "AC3: boundary ride-up call sites must cite #4109")

    # AC4 — compact/truncate survivor stamp restamps (same critical section).
    stamp_restamps = env_code.count("fr.env_gen_stamp_ = env_generation_;")
    check(
        stamp_restamps >= 3,
        f"AC4: compact + truncate must restamp survivor env_gen_stamp_ (alloc + 2 bump sites); found {stamp_restamps}",
    )
    check(
        env_src.count("#4109: restamp survivor env_gen_stamp_") >= 2,
        "AC4: both compact and truncate stamp restamps must cite #4109",
    )

    # AC5 — tests pin the contract.
    rebind_test = ROOT / "tests" / "compiler" / "test_module_rebind_residual.cpp"
    rt = rebind_test.read_text(encoding="utf-8")
    check(
        "#4109" in rt and "ac8_source_gate_4109" in rt, "AC5: test_module_rebind_residual.cpp must carry the #4109 ACs"
    )
    epoch_test = ROOT / "tests" / "compiler" / "test_envframe_epoch_batch.cpp"
    et = epoch_test.read_text(encoding="utf-8")
    check(
        "4109: behind live-body materialize does not copy the capture" in et,
        "AC5: #4072 home must pin the behind live-body empty fallback",
    )
    check(
        "4109: INVALID live-body materialize does not copy the capture" in et,
        "AC5: #4072 home must pin INVALID_VERSION + live body",
    )

    # AC6 — build.py + allowlist wiring.
    build_py = (ROOT / "build.py").read_text(encoding="utf-8")
    check("check_materialize_defuse_4109" in build_py, "AC6: build.py must register check_materialize_defuse_4109")
    allow = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"
    allow_txt = allow.read_text(encoding="utf-8") if allow.exists() else ""
    check(
        "check_materialize_defuse_4109.py" in allow_txt,
        "AC6: root_check_allowlist.txt must list check_materialize_defuse_4109.py",
    )

    # AC7 — no design doc, no standalone issue test.
    for stale in (ROOT / "docs" / "design").glob("4109-*"):
        fails.append(f"AC7: docs/design/{stale.name} is banned (per #1655)")
    for stale in (ROOT / "tests").rglob("test_issue_4109.cpp"):
        fails.append(f"AC7: {stale.relative_to(ROOT)} is banned (extend existing tests)")

    if fails:
        for f in fails:
            print(f"FAIL check_materialize_defuse_4109: {f}")
        print(f"check_materialize_defuse_4109: {len(fails)} failure(s)")
        return 1
    print("check_materialize_defuse_4109: OK (AC1-AC7)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
