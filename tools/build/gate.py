"""Gate pipeline composition (#5246).

The ordered gate steps and the cmd_gate runner. Checks live in
tools/build/checks.py, soak/chaos in tools/build/chaos.py; the two
format/lint commands still live in build.py and are installed here
with set_commands() to avoid a circular import.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from collections.abc import Callable

from tools.build.chaos import (
    cmd_chaos_pr_hard_fail_gate,
    cmd_chaos_release_blocker_2902,
    cmd_soak_pr_short_3387_coverage,
)
from tools.build.checks import (
    cmd_aot_env_linear_stamp,
    cmd_catch_silent_swallow,
    cmd_cli_templates,
    cmd_dead_heap_push,
    cmd_docs,
    cmd_fixtures,
    cmd_legacy_test_inventory,
    cmd_new_test_files,
    cmd_primitive_surface,
    cmd_test_binding,
    cmd_test_registry,
)
from tools.build.core import (
    BUILD,
    COVERAGE_RUN_CHECKS,
    ROOT,
    B,
    N,
    _build_jobs,
    _env_flag,
    _git_changed_files,
    _ninja_build,
    fail,
    info,
    ok,
)


def set_commands(*, lint, format_cmd) -> None:
    """Install the lint/format commands owned by build.py."""
    global _cmd_lint, _cmd_format
    _cmd_lint = lint
    _cmd_format = format_cmd


_cmd_lint = None
_cmd_format = None
_test_concurrent = None


def set_test_command(fn) -> None:
    global _test_concurrent
    _test_concurrent = fn


def _gate_parse_jobs() -> int:
    """Parallel workers for coverage checks (0 → run_checks default)."""
    if "--serial" in sys.argv[2:] or os.environ.get("AURA_GATE_SERIAL", "").strip() in (
        "1",
        "true",
        "yes",
    ):
        return 1
    argv = sys.argv[2:]
    for i, a in enumerate(argv):
        if a == "--jobs" and i + 1 < len(argv) and argv[i + 1].isdigit():
            return max(1, int(argv[i + 1]))
        if a.startswith("--jobs=") and a.split("=", 1)[1].isdigit():
            return max(1, int(a.split("=", 1)[1]))
    env = os.environ.get("AURA_GATE_JOBS", "").strip()
    if env.isdigit() and int(env) > 0:
        return int(env)
    return 0  # run_checks chooses min(16, nproc)


def _run_parallel_coverage_checks(*, changed: bool) -> int:
    """Drive scripts/coverage/run_checks.py (parallel + cascade-free)."""
    if not COVERAGE_RUN_CHECKS.is_file():
        fail(f"missing {COVERAGE_RUN_CHECKS}")
        return 1
    print(f"{B}═══ Coverage checks ({'changed' if changed else 'all'}, parallel) ═══{N}")
    # Cascade suppression is the default for gate: every check runs once.
    os.environ["AURA_COVERAGE_NO_CASCADE"] = "1"
    os.environ["AURA_LINT_SKIP_COVERAGE"] = "1"
    cmd = [sys.executable, str(COVERAGE_RUN_CHECKS)]
    cmd.append("--changed" if changed else "--all")
    jobs = _gate_parse_jobs()
    if jobs > 0:
        cmd.extend(["--jobs", str(jobs)])
    # Forward --base if present.
    argv = sys.argv[2:]
    for i, a in enumerate(argv):
        if a == "--base" and i + 1 < len(argv):
            cmd.extend(["--base", argv[i + 1]])
            break
        if a.startswith("--base="):
            cmd.append(a)
            break
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        fail("coverage checks failed — see scripts/coverage/run_checks.py output above")
        return r.returncode
    ok("coverage checks clean")
    return 0


def _gate_run_issue_bin(target: str) -> int:
    """Ninja + run one issue binary from repo root (CI source-cite cwd)."""
    if not (BUILD / "build.ninja").is_file():
        info(f"changed: skip {target} (no ninja tree)")
        return 0
    print(f"{B}═══ Gate changed: {target} ═══{N}")
    jobs = _gate_parse_jobs() or _build_jobs()
    rc = _ninja_build([target], jobs=max(1, jobs))
    if rc:
        fail(f"ninja {target} failed")
        return rc
    bin_path = BUILD / target
    if not bin_path.is_file():
        fail(f"{target} binary not found")
        return 1
    env = os.environ.copy()
    # Match tests/python/run_issue_tests.py — inherit hole is only
    # visible when the runner default AURA_SANDBOX=off is set.
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
    env["AURA_IR_DIRTY_BATCH_ONLY"] = "0"
    r = subprocess.run([str(bin_path)], cwd=str(ROOT), env=env)
    if r.returncode != 0:
        fail(f"{target} rc={r.returncode}")
    return r.returncode


def _gate_changed_runtime_suites() -> int:
    """Run the slow CI leaves that gate --changed would otherwise skip.

    ci/concurrent and ci/issues are wave-only. Local Debug often misses
    RelWithDebInfo unused-result / AURA_SANDBOX=off isolate / source-cite
    wraps. Compile+run the touched leaf here so those cannot land blind.
    """
    changed = _git_changed_files()
    rc = 0
    if any(p == "tests/serve/test_concurrent.cpp" or p.startswith("src/serve/") for p in changed):
        if not (BUILD / "build.ninja").is_file():
            info("changed serve: skip test_concurrent (no ninja tree)")
        else:
            print(f"{B}═══ Gate changed: test_concurrent (serve/fiber surface) ═══{N}")
            jobs = _gate_parse_jobs() or _build_jobs()
            nrc = _ninja_build(["test_concurrent"], jobs=max(1, jobs))
            if nrc:
                fail("ninja test_concurrent failed")
                return nrc
            rc = _test_concurrent() or rc
    densify_hit = any(
        p
        in (
            "tests/core/test_moving_densify_fail_closed.cpp",
            "tests/core/test_densify_pin_batch.cpp",
            "src/core/arena.ixx",
            "src/core/densify_consistency_report.h",
        )
        for p in changed
    )
    if densify_hit:
        rc = _gate_run_issue_bin("test_densify_pin_batch") or rc
    # Do not ninja-run test_orch_agent_batch here: leftover isolate
    # members (apply-mutex join, schedule-gate SIGSEGV) flake the
    # 12-member binary. #3179 ACs are covered by densify-style
    # AURA_SANDBOX=off runs of that member in ci/issues.
    return rc


def _gate_steps(*, fix: bool, changed: bool, scripts_only: bool) -> list[tuple[str, Callable[[], int]]]:
    """Ordered gate pipeline: (label, callable); a non-zero return aborts.

    Issue #5246: this replaces the historical ~60-call hand-wired `or` chain.
    Coverage is a single step — scripts/coverage/run_checks.py owns the whole
    check_*.py set (parallel, cascade-free) plus the per-check argv that used
    to be spelled out here as ~40 separate 'or cmd_*_coverage()' calls.
    """
    check = not fix
    steps: list[tuple[str, Callable[[], int]]] = [
        ("docs", lambda: cmd_docs(check=check)),
        ("cli-templates", lambda: cmd_cli_templates(check=check)),
        ("lint", _cmd_lint),
    ]
    if not scripts_only:
        steps.append(("format", _cmd_format))
    steps.extend(
        [
            ("fixtures", cmd_fixtures),
            ("primitive-surface", cmd_primitive_surface),
            ("test-registry", cmd_test_registry),
            ("test-binding", cmd_test_binding),
            ("new-test-files", cmd_new_test_files),
            ("dead-heap-push", cmd_dead_heap_push),
            ("catch-silent-swallow", cmd_catch_silent_swallow),
            ("aot-env-linear-stamp", cmd_aot_env_linear_stamp),
            ("legacy-test-inventory", cmd_legacy_test_inventory),
            ("soak-pr-short-3387", cmd_soak_pr_short_3387_coverage),
            ("coverage", lambda: _run_parallel_coverage_checks(changed=changed)),
        ]
    )
    return steps


def cmd_gate():
    """Fast static checks for CI / pre-push.

    Core: docs + ruff + clang-format + fixtures + surface audits.
    Coverage: scripts/coverage/run_checks.py — parallel custom check_*.py
    plus declarative manifests (runner.py). See tests/COVERAGE.md.

    Flags:
      --fix          auto-regen docs/registry/inventory; lint/format fix
      --scripts-only  skip clang-format
      --changed       only coverage/format for git-diff paths (pre-push)
      --serial        coverage jobs=1
      --jobs N        coverage parallel workers
      --full          force full coverage + chaos runtime even with --changed
                      (or AURA_GATE_FULL=1)

    Issue #1572/#1573/#1668/#1669/#1931/#1957/#1966/#2057/#2168 lineage
    preserved; execution order lives in the _gate_steps table (Issue #5246).
    """
    fix = "--fix" in sys.argv[2:]
    scripts_only = "--scripts-only" in sys.argv[2:] or _env_flag("AURA_GATE_SCRIPTS_ONLY")
    changed = "--changed" in sys.argv[2:] or _env_flag("AURA_GATE_CHANGED")
    full = "--full" in sys.argv[2:] or _env_flag("AURA_GATE_FULL")
    if full:
        changed = False

    mode = "fix" if fix else "check"
    if scripts_only:
        mode += "+scripts-only"
    if changed:
        mode += "+changed"
    if full:
        mode += "+full"
    print(f"{B}═══ Gate ({mode}) ═══{N}")

    os.environ["AURA_LINT_SKIP_COVERAGE"] = "1"
    os.environ["AURA_COVERAGE_NO_CASCADE"] = "1"
    if changed:
        os.environ["AURA_FORMAT_CHANGED"] = "1"

    t_gate = time.time()
    if scripts_only:
        info("scripts-only: skipping clang-format (cross-platform gate)")
    for label, step in _gate_steps(fix=fix, changed=changed, scripts_only=scripts_only):
        rc = step()
        if rc:
            fail(f"gate step failed: {label}")
            return rc

    # Chaos runtime profiles (need cmake tree / binary). Static coverage for
    # these issues already ran via run_checks. On --changed pre-push, skip the
    # multi-second runtime profiles unless --full.
    if changed and not full:
        info("changed mode: skipping chaos runtime profiles (static coverage already ran)")
    else:
        rc = cmd_chaos_release_blocker_2902() or cmd_chaos_pr_hard_fail_gate()
        if rc:
            return rc

    # Issue #3567: ci/concurrent is a stress-wave suite (~16s, x86 SIGSEGV).
    # Agents that only run gate --changed + unit never see it. When the
    # serve/fiber surface changes and a ninja tree exists, compile+run here.
    if changed and not scripts_only:
        rc = _gate_changed_runtime_suites()
        if rc:
            return rc

    ok(f"gate clean in {time.time() - t_gate:.1f}s ({mode})")
    return 0
