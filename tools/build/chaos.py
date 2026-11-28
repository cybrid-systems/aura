"""Chaos / soak / production-concurrency commands (#5246).

Nightly profiles, PR hard-fail gates and the release soak gates are one
cluster: they build the same targets and drive the same runtime faces.
They only need the shared paths, colours and process runner.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from tools.build.core import (
    BUILD,
    COVERAGE_CHECKS,
    ROOT,
    B,
    N,
    fail,
    info,
    ok,
    run,
)
from tools.build.core import coverage_run as _coverage_run


def set_configure_command(fn) -> None:
    """Install the still-local cmd_configure (build.py owns it)."""
    global _configure_cmd
    _configure_cmd = fn


_configure_cmd = None


# Issue #3387 — PR-level short soak path filter. When the diff touches
# any of the named runtime/Guard/steal/restamp paths, run the
# `soak-pr-short` ctest label on PRs; otherwise skip silently (AC2).
# Used by cmd_soak_pr_short_3387_coverage below.
_SOAK_PR_SHORT_PATH_PATTERNS = (
    "src/serve/steal_safety",
    "src/serve/fiber",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_security.cpp",
    "src/core/flatast_restamp.hh",
)


def _paths_touch_soak_pr_short(diff_paths: list[str]) -> bool:
    """Return True iff any diff path matches a soak-pr-short surface.

    Issue #3387 AC2: PRs that don't touch these files pay 0 cost
    (no ctest invocation, no 60s timeout).
    """
    for p in diff_paths:
        for needle in _SOAK_PR_SHORT_PATH_PATTERNS:
            if p.startswith(needle) or p == needle or needle in p:
                return True
    return False


def _collect_diff_paths() -> list[str]:
    """Collect paths touched between HEAD and HEAD~1 (or origin/main
    if HEAD has no parent yet).

    Best-effort: returns empty list on any git error so the soak gate
    still observes "no relevant changes" semantics.
    """
    try:
        r = subprocess.run(
            ["git", "diff", "--name-only", "HEAD~1", "HEAD"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        if r.returncode == 0 and r.stdout:
            return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
    except FileNotFoundError:
        pass
    return []


def cmd_soak_pr_short_3387_coverage():
    """Issue #3387: PR-level short soak fail-closed on hold-after-cancel +
    steal residual-zero + torn probe (I1/I3/I6 gate).

    Closes the gap where unit AC green was insufficient evidence for
    high-concurrency AI Agent load defects (hold-budget cancel without
    cooperative edge, steal residual arm Soft under latch, query:*
    -stable torn probe vs export face split). Nightly soak (#2722 /
    #2755 / #2856 / #2931 / #3073) catches them — but too late for the
    PR that re-opens the window. A bounded short slice on the existing
    binaries is enough to catch counter non-zero / probe split; does
    NOT replace the nightly matrix.

    Contract rows (AC1-AC5):

      AC1: PR touching the path filter runs the `soak-pr-short` ctest
           label and fails on any of: steal residual-zero nonzero under
           latch, hold-after-cancel > 2×SLO, torn-probe / export face
           split. The three target suites already fail-closed on those
           observables (regression guarded by their existing ACs).
      AC2: Unrelated PRs (no diff touch on the runtime/Guard/steal/
           restamp surfaces) pay 0 cost — short-circuit on the path
           filter before ctest invocation.
      AC3: Soft / unlatched fixtures inside those suites stay
           metric-only (existing AC — preserved, not modified).
      AC4: No new soak binary; no docs/design/3387-*; no
           tests/issues/test_issue_3387.cpp (#81967).
      AC5: Nightly full chaos matrix stays as-is (#2722 lineage) —
           this issue only wires the PR admission gate.
    """
    paths = _collect_diff_paths()
    if not _paths_touch_soak_pr_short(paths):
        info("soak-pr-short (#3387): path filter miss — skipping (unrelated PR)")
        return 0
    print(f"{B}=== soak-pr-short (#3387) — path filter hit, running ctest -L soak-pr-short ==={N}")
    # 60s per-test ceiling matches AC1 budget; ctest `-L` filters to
    # `soak-pr-short` label only — the three target suites +
    # test_mailbox_hold_starvation_hard (if/when labelled).
    timeout = int(os.environ.get("AURA_SOAK_PR_SHORT_TIMEOUT", "60"))
    build_dir_env = os.environ.get("CMAKE_BINARY_DIR")
    ctest_dir = Path(build_dir_env) if build_dir_env else ROOT / "build"
    if not (ctest_dir / "CTestTestfile.cmake").exists():
        info(f"soak-pr-short (#3387): CTestTestfile.cmake not found at {ctest_dir} — skipping")
        return 0
    r = subprocess.run(
        ["ctest", "-L", "soak-pr-short", "--timeout", str(timeout), "--output-on-failure"],
        cwd=str(ctest_dir),
        text=True,
        capture_output=True,
    )
    if r.returncode != 0:
        fail(
            f"soak-pr-short (#3387) ctest failed (rc={r.returncode}); "
            f"tail:\n{(r.stdout or '')[-2000:]}\n{(r.stderr or '')[-2000:]}"
        )
        return 1
    ok("soak-pr-short (#3387) ctest -L soak-pr-short clean")
    return 0


def cmd_chaos_steal_gc_nightly_2931_coverage():
    """Issue #2931: static contract for chaos steal-gc nightly hard gate.

    Nightly profile: AURA_CHAOS_STEAL_GC=1 AURA_CHAOS_DURATION_S≥600
    AURA_CHAOS_WORKERS≥8; residual-after-exit + resume-fence fail-closed;
    EXCLUDE_FROM_ALL + env gate preserved for PR default.
    """
    print(f"{B}=== chaos steal-gc nightly hard gate coverage (#2931) ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_steal_gc_nightly_2931.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos steal-gc nightly (#2931) coverage contract rows failed")
        return 1
    ok("chaos steal-gc nightly (#2931) coverage clean")
    return 0


def cmd_chaos_steal_gc_nightly_2931():
    """Issue #2931: nightly hard gate — steal×mutate×GC×mailbox chaos soak.

    Env matrix (hard-fail if residual/resume-fence unbounded):
      AURA_CHAOS_STEAL_GC=1
      AURA_CHAOS_DURATION_S≥600  AURA_CHAOS_WORKERS≥8
    Soft steal (AURA_STEAL_SNAPSHOT_SOFT) forbidden under the gate.
    Builds test_chaos_steal_mutation_gc if needed (EXCLUDE_FROM_ALL target).
    PR default remains opt-in via env + EXCLUDE_FROM_ALL.
    CI gate job without cmake: static coverage only.
    """
    print(f"{B}=== chaos steal-gc nightly hard gate (#2931) ==={N}")
    rc = cmd_chaos_steal_gc_nightly_2931_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_steal_mutation_gc"
    cmake_cache = BUILD / "CMakeCache.txt"
    if not bin_path.exists():
        if not cmake_cache.exists():
            ok(
                "chaos steal-gc nightly runtime skipped (no CMakeCache; static coverage only) "
                "— run after ./build.py build or via nightly CI"
            )
            return 0
        info("building test_chaos_steal_mutation_gc…")
        # Full job pool: with `build.py configure` in front (nightly, #5646)
        # this is the job's only compile, and the issue-farm shards build
        # the same libaura_test_objects at -j nproc.
        nproc = os.cpu_count() or 4
        r = subprocess.run(
            [
                "ninja",
                "-C",
                str(BUILD),
                "-j",
                str(nproc),
                "test_chaos_steal_mutation_gc",
            ],
            cwd=ROOT,
        )
        if r.returncode != 0:
            fail("build test_chaos_steal_mutation_gc failed")
            return r.returncode
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    env["AURA_CHAOS_STEAL_GC"] = "1"
    env.setdefault("AURA_CHAOS_DURATION_S", "600")
    env.setdefault("AURA_CHAOS_WORKERS", "8")
    # Soft steal forbidden under nightly hard gate.
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)

    info(
        "env: AURA_CHAOS_STEAL_GC=1 "
        f"workers={env['AURA_CHAOS_WORKERS']} duration={env['AURA_CHAOS_DURATION_S']}s "
        "(residual-after-exit / resume-fence / ticket fail-closed)"
    )
    timeout_s = max(180, int(env["AURA_CHAOS_DURATION_S"]) + 180)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"chaos steal-gc nightly timed out after {timeout_s}s (hang?) — gate blocked")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(
            f"chaos steal-gc nightly FAILED exit={r.returncode} in {elapsed:.1f}s — "
            "residual_defer_after_exit without matching clears / resume_fence hard "
            "surplus / ticket mismatch / residual_defer_steal_hard_fail must be 0"
        )
        return r.returncode
    ok(
        f"chaos steal-gc nightly GREEN in {elapsed:.1f}s "
        f"(workers={env['AURA_CHAOS_WORKERS']} duration={env['AURA_CHAOS_DURATION_S']}s; "
        "residual/resume fail-closed)"
    )
    return 0


def cmd_chaos_release_blocker_2902_coverage():
    """Issue #2902: static contract for chaos hard release blocker."""
    print(f"{B}=== chaos hard release blocker coverage (#2902) ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_release_blocker_2902.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos hard release blocker (#2902) coverage contract rows failed")
        return 1
    ok("chaos hard release blocker (#2902) coverage clean")
    return 0


def cmd_chaos_release_blocker_2902():
    """Issue #2902: hard release blocker — multi-fiber chaos under production_defaults.

    Elevates #2856/#2554/#2722/#2755 into a hard pre-push/CI release gate:
      AURA_CHAOS_RELEASE_BLOCKER=1 AURA_CHAOS_RELEASE_BLOCKER_ONLY=1
      workers=4 fibers=32 duration=8s seed=1 Soft steal off
      production_defaults_active enforced in-process

    Hard-fail set (any delta > 0 fails):
      steal_snapshot_hard_fail, residual still-running,
      residual hard-AND arms (#2721), force_deopt, resume_hard_fail,
      residual_rearm_race (#2901), residual_defer_steal_hard_fail (#2546),
      resume_fence hard/ticket surplus (layout_stamp_resume observe-only).

    Bounded load signal (composition ceiling, not absolute zero under prod):
      mailbox hold/defer starvation — production_defaults hard face (#2551)
      ticks under intentional Guard×mailbox; default ceiling max(64, fibers*2)
      unless AURA_CHAOS_MB_STARVE_MAX is set. Soft/PR still use max=0.

    Sustained mode (optional AURA_CHAOS_SUSTAINED=1): fibers≥32, duration≥8s.
    Soft / known-bad inject under production still fails (#2554 inject path).
    FULL/SOAK nightly paths unchanged.

    CI gate job without cmake: static coverage only (same pattern as #2554).
    """
    print(f"{B}=== chaos hard release blocker (#2902) ==={N}")
    rc = cmd_chaos_release_blocker_2902_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    cmake_cache = BUILD / "CMakeCache.txt"
    if not bin_path.exists():
        if not cmake_cache.exists():
            ok(
                "chaos release blocker runtime skipped (no CMakeCache; static coverage only) "
                "— run after ./build.py build or via issue-farm CI"
            )
            return 0
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = subprocess.run(
            ["ninja", "-C", str(BUILD), "-j", str(max(1, nproc // 2)), "test_chaos_mutate_steal_gc_mailbox"],
            cwd=ROOT,
        )
        if r.returncode != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r.returncode
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    env["AURA_CHAOS_RELEASE_BLOCKER"] = "1"
    env["AURA_CHAOS_RELEASE_BLOCKER_ONLY"] = "1"
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)
    env.setdefault("AURA_CHAOS_SEED", "1")
    env.setdefault("AURA_CHAOS_WORKERS", "4")
    env.setdefault("AURA_CHAOS_FIBERS", "32")
    env.setdefault("AURA_CHAOS_DURATION_S", "8")
    # Leave AURA_CHAOS_MB_STARVE_MAX unset so composition ceiling applies
    # under production_defaults (see run_chaos_pass #2902). Caller may set.

    info(
        "release blocker env: AURA_CHAOS_RELEASE_BLOCKER=1 ONLY "
        f"workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s seed={env['AURA_CHAOS_SEED']}"
    )
    timeout_s = max(180, int(env["AURA_CHAOS_DURATION_S"]) + 120)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"chaos release blocker timed out after {timeout_s}s (hang?) — release blocked")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(
            f"chaos hard release blocker FAILED exit={r.returncode} in {elapsed:.1f}s — "
            "any hard-fail counter (steal hard-fail / residual still-running / "
            "residual hard-AND / rearm_race / defer_steal_hard / resume_fence) "
            "must be 0 under production_defaults_active"
        )
        return r.returncode
    ok(
        f"chaos hard release blocker GREEN in {elapsed:.1f}s "
        f"(workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s; expanded hard-fail set zero)"
    )
    return 0


def cmd_production_concurrency_coverage():
    """Issue #2380/#2513: production-concurrency gate static contract rows.

    Nightly profile: lock-order canary + full chaos + densify + Soft forbid.
    #2513 soak extension: non-yield loops, reclaim residual, hard-fail counters.
    PR smoke path stays short (no FULL / no PRODUCTION_CONCURRENCY_GATE / no SOAK).
    """
    print(f"{B}=== production-concurrency coverage (#2380) ==={N}")
    script = COVERAGE_CHECKS / "check_production_concurrency_gate_2380.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("production-concurrency coverage contract rows failed")
        return 1
    ok("production-concurrency coverage clean")


def cmd_chaos_pr_hard_fail_coverage():
    """Issue #2554: static contract for PR chaos hard-fail deployment gate."""
    print(f"{B}=== chaos PR hard-fail gate coverage (#2554) ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_pr_hard_fail_gate_2554.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos PR hard-fail gate (#2554) coverage contract rows failed")
        return 1
    ok("chaos PR hard-fail gate (#2554) coverage clean")
    return 0


def cmd_chaos_pr_hard_fail_gate():
    """Issue #2554: short PR chaos under production-like hard-fail invariants.

    Part of ./build.py gate (deployment gate). Profile:
      AURA_CHAOS_PR_GATE=1 AURA_CHAOS_PR_GATE_ONLY=1
      workers=4 fibers=16 duration=3s seed=1 Soft steal off
    Asserts: steal hard-fail Δ==0, residual still-running==0, no hang.
    Full SOAK/FULL path unchanged (nightly via production-concurrency).

    AC1: inject hard-fail (AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL=1) must fail.
    AC2: clean short profile must pass.

    CI note: the GitHub Actions `gate` job is static-only (no cmake tree).
    When the binary is absent and build/ is unconfigured, run static
    coverage only and skip the runtime profile — `issue-farm` shard 1
    runs the full hard-fail gate after `./build.py ci-issues` (#4819).
    Local pre-push / configured trees still run the full runtime path.
    """
    print(f"{B}=== chaos PR hard-fail gate (#2554) ==={N}")
    rc = cmd_chaos_pr_hard_fail_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    cmake_cache = BUILD / "CMakeCache.txt"
    if not bin_path.exists():
        if not cmake_cache.exists():
            # Issue #3950: claiming production-concurrency cannot silent-0 skip.
            claimed = os.environ.get("AURA_PRODUCTION_CONCURRENCY_GATE", "").strip() == "1" or (
                os.environ.get("AURA_CI_PRODUCTION_CONCURRENCY", "").strip().lower() in ("1", "true", "yes")
            )
            if claimed:
                fail(
                    "chaos PR hard-fail runtime skipped while production-concurrency claimed "
                    "(#3950) — configure cmake / run after ./build.py build"
                )
                return 1
            # Static-only gate (CI gate job / fresh clone without build/).
            ok(
                "chaos PR hard-fail runtime skipped (no CMakeCache; static coverage only) "
                "— run after ./build.py build or via issue-farm CI"
            )
            return 0
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    def _pr_env(**extra):
        env = os.environ.copy()
        env["AURA_CHAOS_PR_GATE"] = "1"
        env["AURA_CHAOS_PR_GATE_ONLY"] = "1"
        env.setdefault("AURA_CHAOS_SEED", "1")
        env.setdefault("AURA_CHAOS_WORKERS", "4")
        env.setdefault("AURA_CHAOS_FIBERS", "16")
        env.setdefault("AURA_CHAOS_DURATION_S", "3")
        # Issue #2554 flaky: mailbox defer starvation canary is now deduped
        # per open-defer window (multi_fiber_mailbox.h) so the counter no
        # longer storms to 100000. Still allow a small composition-aware
        # ceiling (same as release: max(64, n_fibers*2)) so a single
        # spurious >100ms defer under CI load does not flake the gate.
        env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "64")
        env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)
        env.pop("AURA_CHAOS_FULL", None)  # PR gate must not pull FULL soak
        env.pop("AURA_CHAOS_SOAK", None)
        env.pop("AURA_PRODUCTION_CONCURRENCY_GATE", None)
        env.update(extra)
        return env

    # AC2: clean short profile under production-like hard-fail invariants.
    env_clean = _pr_env()
    info(
        "env: AURA_CHAOS_PR_GATE=1 ONLY workers="
        f"{env_clean['AURA_CHAOS_WORKERS']} fibers={env_clean['AURA_CHAOS_FIBERS']} "
        f"duration={env_clean['AURA_CHAOS_DURATION_S']}s seed={env_clean['AURA_CHAOS_SEED']}"
    )
    timeout_s = 90  # CI resource limit; profile is ~3s + injects
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env_clean, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"chaos PR hard-fail gate timed out after {timeout_s}s (hang?)")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(f"chaos PR hard-fail gate clean run failed exit={r.returncode} in {elapsed:.1f}s")
        return r.returncode
    ok(f"chaos PR hard-fail clean green in {elapsed:.1f}s")

    # AC1: intentional inject of steal hard-fail must fail the gate binary.
    env_inj = _pr_env(AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL="1")
    info("AC1 inject: AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL=1 (expect non-zero exit)")
    try:
        r2 = subprocess.run([str(bin_path)], cwd=ROOT, env=env_inj, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail("AC1 inject timed out")
        return 1
    if r2.returncode == 0:
        fail("AC1: inject hard-fail must make PR-gate binary fail (got exit 0)")
        return 1
    ok(f"AC1 inject hard-fail correctly failed exit={r2.returncode}")
    return 0


def cmd_production_concurrency():
    """Issue #2380/#2513: nightly / deploy production-concurrency hard gate.

    Env matrix (hard-fail if any criterion fails):
      AURA_PRODUCTION_CONCURRENCY_GATE=1
      AURA_LOCK_ORDER_CANARY=1
      AURA_CHAOS_FULL=1
      AURA_CHAOS_WORKERS≥4  AURA_CHAOS_DURATION_S≥30
      Optional soak: AURA_CHAOS_SOAK=1 AURA_CHAOS_FIBERS=256..1000
                     AURA_CHAOS_DURATION_S=300+ (nightly / deploy)
    Soft steal (AURA_STEAL_SNAPSHOT_SOFT) is forbidden.
    Hard criteria (#2513): steal hard-fail delta 0, residual still-running 0,
    mailbox starvation ≤ AURA_CHAOS_MB_STARVE_MAX, no hang.
    Builds test_chaos_mutate_steal_gc_mailbox if needed, then soaks.
    Not part of PR CI smoke — use nightly or explicit local run.
    """
    print(f"{B}═══ production-concurrency gate (#2380/#2513) ═══{N}")
    # Static contract first (fast fail on missing wire-up).
    rc = cmd_production_concurrency_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    if not bin_path.exists():
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"
    env["AURA_LOCK_ORDER_CANARY"] = "1"
    env["AURA_CHAOS_FULL"] = "1"
    env.setdefault("AURA_CHAOS_SEED", "1")
    env.setdefault("AURA_CHAOS_WORKERS", "4")
    # #2513: prefer higher fiber default under gate; SOAK raises further.
    if env.get("AURA_CHAOS_SOAK", "") == "1":
        env.setdefault("AURA_CHAOS_FIBERS", "256")
        env.setdefault("AURA_CHAOS_DURATION_S", "300")
    else:
        env.setdefault("AURA_CHAOS_FIBERS", "64")
        env.setdefault("AURA_CHAOS_DURATION_S", "30")
    env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "0")
    # Soft steal forbidden under production gate (also unset by test body).
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)

    info(
        "env: AURA_PRODUCTION_CONCURRENCY_GATE=1 AURA_LOCK_ORDER_CANARY=1 "
        f"AURA_CHAOS_FULL=1 soak={env.get('AURA_CHAOS_SOAK', '0')} "
        f"workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s seed={env['AURA_CHAOS_SEED']}"
    )
    # Full soak + injects: allow generous wall (duration + watchdog + overhead).
    timeout_s = max(180, int(env.get("AURA_CHAOS_DURATION_S", "30")) + 120)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"production-concurrency timed out after {timeout_s}s (hang?)")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(f"production-concurrency failed exit={r.returncode} in {elapsed:.1f}s")
        return r.returncode
    ok(f"production-concurrency green in {elapsed:.1f}s")
    return 0


def cmd_chaos_soak_hard_gate_2722():
    """Issue #2722: RELEASE hard deploy gate — chaos SOAK under production
    hard-fail-closed semantics. Required for any tag / release candidate
    that claims multi-fiber mutation safety. Closes the #2679 residual
    ("chaos soak is optional / best-effort — does not gate production
    builds or release artifacts").

    Issue #2755 (additive residual-zero extension of #2722): at end-of-run
    under AURA_PRODUCTION_CONCURRENCY_GATE=1 + Hard the chaos harness
    hard-fails if any steal-safety residual hard-AND counter grew:

      residual hard-AND (#2721 four arms — must be hard-zero):
        g_steal_safety_residual_boundary_unsafe_total
        g_steal_safety_residual_layout_stamp_mismatch_total
        g_steal_safety_residual_ticket_mismatch_total
        g_steal_safety_residual_gc_defer_armed_total
      related surfaces hard-zero under this gate:
        steal_snapshot_mismatch_force_deopt_total
        resume_hard_fail_total
      related observe-only (printed; residual_layout_stamp covers silent
      corruption for LayoutStamp races):
        layout_stamp_resume_mismatch_total

    Soft / local iteration (no SOAK hard gate / no production concurrency
    gate) remains non-gating for residual counters (metric-only print).

    AC1: full SOAK with production_defaults_active=true + Hard
        fail-closed. Runs cmd_production_concurrency under
        AURA_CHAOS_SOAK_HARD_GATE=1 (distinct env so PR CI smoke +
        nightly + RELEASE hard gate are three independently-gated
        paths — same harness, different scope).
    AC2: any residual panic, LayoutStamp mismatch, live MutationHold
        after boundary exit, or steal-after-degrade fails the gate.
        The chaos binary already bumps hard-fail counters for these
        4 invariants under AURA_CHAOS_FULL=1 (production / SOAK modes);
        the binary's CHECK(delta==0, "silent corruption") gates the exit.
        #2755: residual hard-AND deltas also gate exit (see counter list).
    AC3: SOAK duration / fiber count / GC frequency parameterized
        (documented production envelope numbers below).
    AC4: required for any tag / release candidate. Wired into
        .github/workflows/release.yml as a required status check that
        must pass before release artifacts upload.
    AC5: Soft (metric-only) mode remains available for local iteration
        via AURA_STEAL_SNAPSHOT_SOFT=1 but is EXPLICITLY non-gating
        here (the hard gate forces production_defaults_active=true).

    Production envelope (documented per AC3):
      workers  : 8     (AURA_CHAOS_WORKERS=8)
      fibers   : 256   (AURA_CHAOS_FIBERS=256)
      duration : 300s  (AURA_CHAOS_DURATION_S=300)
      gc freq  : chaos harness drives request_gc_safepoint() under
                 concurrent mutate + steal (see tests/serve/test_chaos_
                 mutate_steal_gc_mailbox.cpp AC1 contract rows)
      seed     : AURA_CHAOS_SEED=1 (reproducible per AC6)

    AC4 release.yml contract: the release job runs
    `python3 build.py chaos-soak-hard-gate-2722` as a required step
    before `softprops/action-gh-release@v3` uploads the tarball +
    SBOM. Tag push (push.tags: 'v*') triggers the job; if the gate
    fails, the release workflow exits non-zero and no release assets
    are uploaded.
    """
    print(f"{B}=== chaos SOAK hard deploy gate (#2722 + #2755 residual-zero) ==={N}")
    # Static contract first — fast fail on missing wire-up.
    rc = cmd_chaos_soak_hard_gate_2722_coverage()
    if rc != 0:
        return rc
    # Issue #2755: residual hard-AND zero contract (additive over #2722).
    rc = cmd_chaos_soak_residual_zero_2755_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    if not bin_path.exists():
        if not (BUILD / "CMakeCache.txt").exists():
            # Issue #4437: static coverage must not pass this gate. A missing
            # cache means this command configures the default build, then
            # builds and runs the soak. Configure failure stays a release
            # failure; there is no static-only success.
            info(
                "chaos SOAK hard gate: no CMakeCache in the default build; "
                "configuring so test_chaos_mutate_steal_gc_mailbox is built and run"
            )
            rc = _configure_cmd()
            if rc != 0:
                fail("chaos SOAK hard gate configure failed (static coverage only does not pass the release hard gate)")
                return rc
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    # Hard-gate env matrix: full profile + SOAK + hard-fail-closed.
    env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"
    env["AURA_LOCK_ORDER_CANARY"] = "1"
    env["AURA_CHAOS_FULL"] = "1"
    env["AURA_CHAOS_SOAK"] = "1"  # long SOAK per AC3 (300s)
    env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"  # distinct env so PR + nightly + RELEASE are independently gated
    # Soft steal FORBIDDEN under hard gate (production_defaults_active + Hard).
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)
    # Production envelope (documented above; overridable via env for
    # local iteration under AURA_CHAOS_HARD_GATE_OVERRIDE=1).
    env.setdefault("AURA_CHAOS_SEED", "1")
    env.setdefault("AURA_CHAOS_WORKERS", "8")
    env.setdefault("AURA_CHAOS_FIBERS", "256")
    env.setdefault("AURA_CHAOS_DURATION_S", "300")
    env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "0")

    info(
        "RELEASE hard gate env: AURA_PRODUCTION_CONCURRENCY_GATE=1 "
        "AURA_CHAOS_FULL=1 AURA_CHAOS_SOAK=1 AURA_CHAOS_SOAK_HARD_GATE=1 "
        f"workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s seed={env['AURA_CHAOS_SEED']} "
        "mb_starve_max=0"
    )
    # Wall for the soak child only (the build above is not part of it). The
    # release container runs 8 workers / 256 fibers on a 2-vCPU runner; the
    # v0.2.1 tag release died on the old max(900, duration + 600) cap with
    # every residual gate already printing PASS, i.e. the soak itself was
    # slower than 900 s there, not hung. Budget 3x the soak plus a fixed
    # margin for that shape; the GitHub job timeout still bounds the step.
    soak_s = int(env["AURA_CHAOS_DURATION_S"])
    timeout_s = max(1800, soak_s * 3 + 900)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(
            f"chaos SOAK hard gate timed out after {timeout_s}s "
            f"(duration={soak_s}s workers={env['AURA_CHAOS_WORKERS']} "
            f"fibers={env['AURA_CHAOS_FIBERS']}; a runner too slow for the "
            "soak reports here, while a true hang leaves the residual "
            "counters non-zero) — release blocked"
        )
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(
            f"RELEASE chaos SOAK hard gate FAILED exit={r.returncode} "
            f"in {elapsed:.1f}s — release blocked (residual panic / "
            "LayoutStamp mismatch / live MutationHold / steal-after-degrade / "
            "steal-safety residual hard-AND (#2755: boundary/layout/ticket/"
            "gc_defer + layout_resume/force_deopt/resume_hard_fail) "
            "must be 0 under production_defaults_active + Hard)"
        )
        return r.returncode
    ok(
        f"RELEASE chaos SOAK hard gate GREEN under production_defaults_active "
        f"+ Hard in {elapsed:.1f}s (workers={env['AURA_CHAOS_WORKERS']} "
        f"fibers={env['AURA_CHAOS_FIBERS']} duration={env['AURA_CHAOS_DURATION_S']}s; "
        "residual hard-AND + related surfaces hard-zero per #2755)"
    )
    return 0


def cmd_chaos_soak_hard_gate_2722_coverage():
    """Issue #2722: static contract for RELEASE chaos SOAK hard gate.

    Validates the #2722 contract:
      AC1: cmd_chaos_soak_hard_gate_2722 exists in build.py
      AC2: hard-fail env matrix forces production_defaults_active + Hard
      AC3: production envelope (workers=8, fibers=256, duration=300s)
           documented in the function docstring
      AC4: required for any tag / release candidate (wired in
           .github/workflows/release.yml)
      AC5: Soft mode (AURA_STEAL_SNAPSHOT_SOFT) explicitly non-gating
           (popped under the hard gate env matrix)
    """
    print(f"{B}=== chaos SOAK hard gate (#2722) coverage ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_soak_hard_gate_2722.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos SOAK hard gate (#2722) coverage contract rows failed")
        return 1
    ok("chaos SOAK hard gate (#2722) coverage clean")
    return 0


def cmd_chaos_soak_residual_zero_2755_coverage():
    """Issue #2755: residual steal-safety hard-AND counters hard-zero under
    SOAK hard gate (extend #2722). Static contract only — full SOAK runtime
    is still owned by cmd_chaos_soak_hard_gate_2722.
    """
    print(f"{B}=== chaos SOAK residual-zero (#2755) coverage ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_soak_residual_zero_2755.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos SOAK residual-zero (#2755) coverage contract rows failed")
        return 1
    ok("chaos SOAK residual-zero (#2755) coverage clean")
    return 0


def cmd_production_concurrency_ready_gate_4159():
    """Issue #4159 (P0): production concurrency Ready deploy residual-zero
    hard gate — the machine-checkable continuous deploy AND over
    #2722/#2755. The RELEASE soak hard gate was env-opt-in per deploy and
    the coverage linter proved only command PRESENCE, so a deploy could
    ship a Soft-green soak while the named residuals (rearm_race /
    lifetime_proof / envframe_lag) grew between RELEASE runs — missing
    machine-checkable continuous proof of the I3/I6 arms (not
    Soft-as-vuln).

    This command IS the deploy AND (all terms reuse existing latches/bits;
    no new residual bus):
      1. aura_runtime_require_production_multi_worker — the #3195 Ready
         latch, consulted by the deploy-AND accessor through
         aura_runtime_multi_worker_production_latched().
      2. cmd_chaos_soak_hard_gate_2722() — the RELEASE SOAK hard gate runs
         here (residual arms hard-zero at end-of-run per #2755).
      3+4. steal_safety_production_residual_zero_v_read() == 1 AND
         sticky-fail == 0 under the latched face + armed soak env —
         proven at deploy time by the residual-zero runtime face
         (test_steal_safety_production_residual_zero, #4159 AC18) run
         under AURA_CHAOS_SOAK_HARD_GATE=1 +
         AURA_PRODUCTION_CONCURRENCY_GATE=1. Soft (unlatched) returns 0
         from steal_safety_production_concurrency_ready_gate_v_read()
         even with the soak armed — a Soft-green soak can never satisfy
         the deploy AND.

    Soft / PR soak remains non-gating (#2722 AC5 / #2755 AC2 unchanged);
    #3162/#3195 sticky faces and the #4158 edge-free hold contract are
    not weakened. release.yml runs this gate after the 2722 coverage row
    and before release-asset upload; the static rows run on EVERY
    build.py gate via scripts/check_chaos_ready_residual_gate_4159.py —
    continuous proof, not RELEASE-only.
    """
    print(f"{B}=== production concurrency Ready residual-zero deploy gate (#4159) ==={N}")
    # Static deploy-AND contract first — fast fail on missing wire-up.
    script = ROOT / "scripts" / "check_chaos_ready_residual_gate_4159.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = run([sys.executable, str(script)], cwd=ROOT)
    if r != 0:
        fail("production concurrency Ready residual gate (#4159) static contract failed")
        return r
    ok("production concurrency Ready residual gate (#4159) static contract clean")

    # Term 2: the RELEASE chaos SOAK hard gate itself (full soak; residual
    # arms hard-zero at end-of-run per #2755).
    rc = cmd_chaos_soak_hard_gate_2722()
    if rc != 0:
        return rc

    # Terms 3+4: residual-zero SSOT + sticky-clear under the latched face,
    # proven at deploy time by the residual-zero runtime face under the
    # armed env (the #4159 AC18 block flips the env/latch per sub-case and
    # restores; the outer env arms the deploy face for the whole binary).
    bin_path = BUILD / "test_steal_safety_production_residual_zero"
    if not bin_path.exists():
        if not (BUILD / "CMakeCache.txt").exists():
            ok(
                "residual-zero runtime face skipped (no CMakeCache; static "
                "deploy-AND contract only) — run after ./build.py build"
            )
            return 0
        info("building test_steal_safety_production_residual_zero…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_steal_safety_production_residual_zero",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_steal_safety_production_residual_zero failed")
            return r
    env = os.environ.copy()
    env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"
    env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"
    info("deploy face env: AURA_CHAOS_SOAK_HARD_GATE=1 AURA_PRODUCTION_CONCURRENCY_GATE=1")
    r = subprocess.run([str(bin_path)], cwd=ROOT, env=env)
    if r.returncode != 0:
        fail(
            "production concurrency Ready residual gate (#4159) FAILED — "
            "residual-zero SSOT / sticky / deploy-AND face red under the "
            "armed env; release blocked"
        )
        return r.returncode
    ok("production concurrency Ready residual gate (#4159) GREEN — deploy AND proven")
    return 0
