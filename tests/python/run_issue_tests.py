#!/usr/bin/env python3
"""
run_issue_tests.py — issue/bundle C++ binary runner.

Prefer the unified CLI (#1961):
  python3 tests/run.py issues [--tier fast|full] …
  python3 tests/run.py issues-fast

This module remains the implementation (and a stable import path).
Direct invocation still works for transition.

Usage:
  python3 tests/run_issue_tests.py                # run all (full tier)
  python3 tests/run_issue_tests.py --tier fast    # PR subset + git-changed
  python3 tests/run_issue_tests.py --build        # build first
  python3 tests/run_issue_tests.py --filter 196   # run only #196
  python3 tests/run_issue_tests.py --jobs 8       # parallel execution
  python3 tests/run_issue_tests.py --timeout 30   # per-test timeout (default 60)
  python3 tests/run_issue_tests.py --list         # list available tests
  python3 tests/run_issue_tests.py --json         # machine-readable summary

Wired into:
  - tests/run.py issues / issues-fast
  - build.py: cmd_test("issues") → tests/run.py issues
  - .github/workflows/ci.yml: PR uses AURA_ISSUES_TIER=fast
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from threading import Lock

# Issue #1932: allow sibling imports when run as tests/python/run_issue_tests.py
_py = str(Path(__file__).resolve().parent)
if _py not in sys.path:
    sys.path.insert(0, _py)

import contextlib  # noqa: E402

from _aura_harness import AURA_BIN, BUILD, ROOT, B, G, N, R, Y  # noqa: E402
from issue_tier import (  # noqa: E402
    _member_to_bundle,
    git_changed_issue_targets,
    issues_tier,
    resolve_issue_targets,
)

# No pre-existing failures allowed (2026-08-25): every issue binary must
# pass; any failure fails CI. The historical PRE_EXISTING_FAILURES list was
# cleared — entries were either dead (no CMake target / absorbed into
# src-aligned batches, so the runner never executes them) or fixed
# (test_macro_hygiene_batch AC5 race; test_concurrent/test_contracts are in
# _ISSUE_DISCOVERY_SKIP; test_lock_order_audit is
# test_lock_order_audit_batch in CMake).

_print_lock = Lock()


def _bundled_standalone_members() -> set[str]:
    """Issue members linked into bundles — skip stale standalone binaries."""
    return set(_member_to_bundle().keys())


def _test_binary_has_source(name: str) -> bool:
    """True if a source for *name* still exists (mirrors cmake/AuraTest.cmake).

    Stale executables left in build/ after source cleanup (e.g. orphan
    tests/issues/test_issue_1956.cpp removed in #1978) must not be
    rediscovered as NEW CI failures.
    """
    # tests/issues/ removed (#1957 wave 59+); resolve src/-aligned only.
    # 1. tests/core/<NAME>.cpp (R1 src/-aligned default)
    if (ROOT / "tests" / "core" / f"{name}.cpp").is_file():
        return True
    # 2. tests/compiler/<NAME>.cpp / tests/<src-subdir>/<NAME>.cpp  ·  3. tests/<theme>/<NAME>.cpp
    core = ROOT / "tests" / "core"
    if core.is_dir():
        for theme in core.iterdir():
            if theme.is_dir() and (theme / f"{name}.cpp").is_file():
                return True
    tests = ROOT / "tests"
    for theme in tests.iterdir():
        if theme.is_dir() and (theme / f"{name}.cpp").is_file():
            return True
    # 5. tests/<NAME>.cpp
    if (tests / f"{name}.cpp").is_file():
        return True
    # Bundle drivers (tests/bundles/test_issues_*_main.cpp)
    if (tests / "bundles" / f"{name}_main.cpp").is_file():
        return True
    return bool((tests / "bundles" / f"{name}.cpp").is_file())


# Unit / sanitizer / ICE-isolated bins that live in build/ as test_* but
# are not issue-suite members. Glob discovery used to pick these up
# (test_concurrent then sat in PRE_EXISTING as a 60s timeout).
_ISSUE_DISCOVERY_SKIP = frozenset(
    {
        "test_ir",
        "test_concurrent",
        "test_gc_evaluator_integration",
        "test_contracts",
    }
)


def discover_test_issue_binaries() -> list[str]:
    """Find issue bundle + standalone test binaries in build/."""
    bundled = _bundled_standalone_members()
    bins = []
    if not BUILD.is_dir():
        return bins
    for entry in sorted(BUILD.iterdir()):
        if not entry.is_file():
            continue
        name = entry.name
        if name in _ISSUE_DISCOVERY_SKIP:
            continue
        if (
            name.startswith("test_issues_")
            or name.startswith("test_obs_")
            or name.startswith("test_")
            or name.startswith("test_aura_result_")
            or (name.startswith("test_issue_") and name not in bundled)
            or name.startswith("test_primitives_hotpath")
            # src/-aligned pilots built as aura_add_issue_test targets (R1)
            or name
            in {
                "test_arena_batch",
                "test_compact_batch",
                "test_compact_sweep_batch",
                "test_gc_batch",
                "test_arena_defrag_concurrent",
            }
        ):
            # Drop build/ leftovers whose sources were deleted/relocated.
            if not _test_binary_has_source(name):
                continue
            bins.append(name)
    return bins


def discover_test_issue_targets() -> list[str]:
    """Discover test_issue_* ninja targets via CMake build.ninja."""
    if not (BUILD / "build.ninja").is_file():
        return []
    try:
        r = subprocess.run(
            ["ninja", "-C", str(BUILD), "-t", "targets", "all"],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        return []
    targets = []
    for line in r.stdout.splitlines():
        if ":" not in line:
            continue
        name = line.split(":", 1)[0].strip()
        if name in _ISSUE_DISCOVERY_SKIP:
            continue
        if (
            (
                name.startswith("test_issues_")
                or name.startswith("test_issue_")
                or name.startswith("test_obs_")
                or name.startswith("test_")
                or name.startswith("test_aura_result_")
                or name.startswith("test_primitives_hotpath")
            )
            and not name.startswith("CMakeFiles")
            and "cmake_object" not in name
        ):
            targets.append(name)
    return sorted(set(targets))


def filter_bins_for_tier(bins: list[str], tier: str) -> list[str]:
    if tier == "full":
        return bins
    allowed = set(resolve_issue_targets("fast"))
    return [b for b in bins if b in allowed]


def _last_bundle_member(stdout: str) -> str | None:
    import re

    last: str | None = None
    for line in stdout.splitlines():
        m = re.search(r"════ Bundle member: (\S+) ════", line)
        if m:
            last = m.group(1)
    return last


def parse_pass_fail_count(stdout: str) -> tuple[int, int]:
    """Parse a test binary's stdout for pass/fail counts."""
    import re

    # Prefer the final summary line (bundle drivers print member Results first).
    last: tuple[int, int] | None = None
    for line in stdout.splitlines():
        m = re.search(r"(?:Total|Results):\s+(\d+)\s+passed,\s+(\d+)\s+failed", line)
        if m:
            last = (int(m.group(1)), int(m.group(2)))
            continue
        m = re.search(
            r"(?:Total|Results):\s+(\d+)/(\d+)\s+passed,\s+(\d+)/(\d+)\s+failed",
            line,
        )
        if m:
            last = (int(m.group(1)), int(m.group(3)))
    return last if last is not None else (0, 0)


def build_targets(targets: list[str], *, timeout_s: int = 600) -> int:
    """Build the given test_issue_* targets via ninja (-k 0).

    timeout_s caps a single ninja invocation so a stuck module rebuild cannot
    hang the whole issues suite (common when SO graph is dirty).
    """
    if not targets:
        return 0
    print(f"{B}Building {len(targets)} test_issue_* binaries (ninja -k 0)...{N}")
    # Cap link parallelism to avoid mold SIGBUS under multi-GB LLVM link.
    jobs = max(1, min(4, int(os.environ.get("AURA_ISSUE_BUILD_JOBS", "2") or "2")))
    cmd = ["ninja", "-k", "0", f"-j{jobs}", "-C", str(BUILD)] + targets
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        print(
            f"{Y}ninja rebuild timed out after {timeout_s}s for "
            f"{len(targets)} target(s); continuing with existing binaries.{N}"
        )
        return 1
    if r.returncode != 0:
        print(f"{Y}Some targets failed to build (pre-existing). Continuing with what built.{N}")
        if r.stderr:
            print(r.stderr[-1500:], file=sys.stderr)
    return 0


def _shared_so_mtimes() -> list[float]:
    """Mtimes of shared libs that issue binaries DT_NEEDED-link."""
    mtimes: list[float] = []
    for name in (
        "libaura_test_objects.so",
        "libaura_tl_arena.so",
        "libaura_jit_light_test_objects.so",
        "libaura_jit_test_objects.so",
        "libaura-reflect.so",
    ):
        p = BUILD / name
        if p.is_file():
            with contextlib.suppress(OSError):
                mtimes.append(p.stat().st_mtime)
    return mtimes


def _binary_stale_vs_shared_sos(bin_path: Path) -> bool:
    """True when binary is older than any shared SO it depends on (ABI skew)."""
    if not bin_path.is_file():
        return True
    try:
        bt = bin_path.stat().st_mtime
    except OSError:
        return True
    sos = _shared_so_mtimes()
    if not sos:
        return False
    # Rebuild if any shared SO is strictly newer (common after SO-only rebuild).
    return any(st > bt + 0.5 for st in sos)


def refresh_stale_issue_binaries(bins: list[str]) -> list[str]:
    """Ninja-rebuild binaries older than libaura_test_objects.so (ABI skew → rc=127).

    Returns the list of names that were attempted. Safe no-op when none stale.
    """
    stale = [b for b in bins if _binary_stale_vs_shared_sos(BUILD / b)]
    if not stale:
        return []
    print(f"{Y}Refreshing {len(stale)} stale issue binaries (older than shared SOs — prevents rc=127 ABI skew)...{N}")
    build_targets(stale)
    return stale


# Crash / OOM-class signals that deserve one serial retry after parallel load.
_CRASH_RCS = frozenset(
    {
        -4,  # SIGILL
        -6,  # SIGABRT
        -7,  # SIGBUS
        -9,  # SIGKILL (OOM)
        -11,  # SIGSEGV
        132,  # 128+SIGILL
        134,  # 128+SIGABRT
        135,  # 128+SIGBUS
        137,  # 128+SIGKILL
        139,  # 128+SIGSEGV
    }
)


def _eff_timeout(bin_name: str, timeout: int) -> int:
    """Per-binary timeout scaling for stress / late bundles / orch."""
    # late1 alone can exceed 6 min under parallel load on aarch64 CI
    # (was timing out at 60*4=240s with rc=124).
    is_very_heavy = bin_name in (
        "test_issues_jit_late1",
        "test_issues_jit_late3",
        "test_issues_jit_late4",
        "test_concurrent",
        "test_orch_agent_batch",
        "test_tenant_isolation_enforcement",
        "test_fiber_orch_parallel_quota_batch",
        "test_chaos_mutate_steal_gc_mailbox",
        # 24-member mailbox/fiber/join-drain batch; 60s default dies mid-batch
        # (rc=124) before test_join_drain_reclaim even starts.
        "test_mailbox_fiber_batch",
        "test_pmr_alloc_fiber_safe",
        "test_string_heap_corruption_guard",
        "test_mutation_aot_unit_batch",
        "test_hygiene_mutate_closed_loop",
    )
    is_heavy = (
        "bench" in bin_name
        or bin_name == "test_issues_jit"
        or bin_name.startswith("test_jit_")
        or bin_name.startswith("test_issues_jit_late")
        or bin_name.startswith("test_issues_fiber")
        or bin_name.startswith("test_orch_")
        or bin_name.startswith("test_fiber_orch_")
        or "stress" in bin_name
        or "chaos" in bin_name
    )
    if is_very_heavy:
        return timeout * 10  # 600s default
    if is_heavy:
        return timeout * 4
    return timeout


def _run_one_attempt(bin_name: str, timeout: int) -> tuple[str, int, int, int, str]:
    """Single attempt. Returns (name, passed, failed, returncode, error_msg)."""
    bin_path = BUILD / bin_name
    if not bin_path.is_file():
        return bin_name, 0, 0, 127, f"binary not found: {bin_path}"
    eff_timeout = _eff_timeout(bin_name, timeout)
    # Unique TMPDIR per process: parallel issue binaries that write
    # /tmp/*.so AOT artifacts / sockets must not collide (rc=-11 under jobs>1).
    import tempfile

    tmp_root = tempfile.mkdtemp(prefix=f"aura_issue_{bin_name}_")
    # Issue #226 follow-up: pass AURA_BIN + AURA_SRC_ROOT to
    # subprocesses so tests that shell out to the aura binary
    # (test_issue_294, test_issue_295) can resolve relative
    # paths regardless of cwd. The bundle binaries themselves
    # don't read these vars, but the tests they link do.
    # Use ROOT as cwd (consistent with build.py / CI infra)
    # so the test's `cd <repo_root>` works as expected.
    #
    # AURA_SANDBOX=off is required for Soft pipeline / Soft audit /
    # non-Forbidden tree-walker (matches build.py _aura_test_env,
    # tests/python/run.py, run-tests.sh). Without it, production
    # defaults (Issue #2213 pipeline Forbidden, #2053 security) make
    # dozens of issue binaries hard-fail on main CI full tier.
    env = {
        **os.environ,
        "AURA_BIN": str(AURA_BIN),
        "AURA_SRC_ROOT": str(ROOT),
        "TMPDIR": tmp_root,
        "TMP": tmp_root,
        "TEMP": tmp_root,
    }
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
    # Issue tests are Soft/dev (AURA_SANDBOX=off). build.py production
    # presets / inherited CI env may set AURA_IR_DIRTY_BATCH_ONLY=1 which
    # std::abort()s on residual mark_block_dirty (occurrence_coercion,
    # aot_jit_stamp, ir_closure, misc_issue_fold). Members that need the
    # hard abort setenv 1 themselves.
    env["AURA_IR_DIRTY_BATCH_ONLY"] = "0"
    # Hard lock-order canary (mode 3) aborts on Mutate-while-Workspace
    # which several issue members do intentionally. Keep it off unless
    # the binary is the lock-order batch itself.
    if bin_name != "test_lock_order_audit_batch":
        env.pop("AURA_LOCK_ORDER_CANARY", None)
    try:
        r = subprocess.run(
            [str(bin_path)],
            capture_output=True,
            text=True,
            timeout=eff_timeout,
            errors="replace",
            cwd=str(ROOT),
            env=env,
        )
    except subprocess.TimeoutExpired:
        return bin_name, 0, 0, 124, f"timeout after {eff_timeout}s"
    finally:
        # Best-effort cleanup of private tmp (AOT .so / sockets).
        try:
            import shutil as _shutil

            _shutil.rmtree(tmp_root, ignore_errors=True)
        except Exception:
            pass
    passed, failed = parse_pass_fail_count(r.stdout)
    # Always keep stderr snippets so pre-existing member classification
    # can match "bundle member X failed/crashed" lines from the driver.
    stderr_tail = (r.stderr or "")[-2000:]
    if passed + failed == 0:
        if r.returncode == 0:
            return bin_name, 1, 0, 0, ""
        return bin_name, 0, 1, r.returncode, stderr_tail if stderr_tail else "no output"
    err = ""
    if r.returncode != 0 and r.returncode not in (0, 1):
        member = _last_bundle_member(r.stdout)
        if member:
            err = f"crashed during bundle member {member}\n{stderr_tail}"
        elif stderr_tail:
            err = stderr_tail
        else:
            err = "no output"
    elif r.returncode != 0 and stderr_tail:
        err = stderr_tail
    return bin_name, passed, failed, r.returncode, err


def run_one(bin_name: str, timeout: int) -> tuple[str, int, int, int, str]:
    """Run one test binary with private TMPDIR + one crash/symbol retry.

    Robustness (full-tier parallel):
      1. First attempt under private TMPDIR (no /tmp AOT collisions).
      2. On crash signal or undefined-symbol: ninja-relink once + retry once.
    Proactive bulk rebuild is deferred to phase-0 (capped) / phase-2 recovery
    so a dirty SO graph cannot serialize the whole suite behind one hung ninja.
    """
    name, passed, failed, rc, err = _run_one_attempt(bin_name, timeout)
    # Symbol lookup / ABI skew often leaves rc=127 with "undefined symbol".
    need_rebuild = rc == 127 or (err and ("undefined symbol" in err or "symbol lookup error" in err))
    need_retry = rc in _CRASH_RCS or need_rebuild or rc == 124
    if not need_retry:
        return name, passed, failed, rc, err
    # Fresh link then solo retry (clears transient parallel pressure flakes).
    # Short timeout: if the SO graph is dirty this can take minutes; phase-2
    # serial recovery will rebuild in bulk more efficiently.
    build_targets([bin_name], timeout_s=180)
    time.sleep(0.05)
    name2, p2, f2, rc2, err2 = _run_one_attempt(bin_name, timeout)
    return name2, p2, f2, rc2, err2


def _print_result(
    b: str,
    passed: int,
    failed: int,
    rc: int,
    err: str,
) -> None:
    with _print_lock:
        if rc == 0 and failed == 0:
            print(f"  {G}✓{N} {b} ({passed} passed)")
        else:
            print(f"  {R}✗{N} {b} ({passed} passed, {failed} failed, rc={rc})")
            if err:
                print(f"      {err[:200]}")


def _classify_result(
    b: str,
    passed: int,
    failed: int,
    rc: int,
    err: str,
    *,
    failures: list,
) -> None:
    """Append to failures and print. Any non-zero result fails CI."""
    if rc == 0 and failed == 0:
        _print_result(b, passed, failed, rc, err)
    else:
        failures.append((b, passed, failed, rc, err))
        _print_result(b, passed, failed, rc, err)


def run_bins_parallel(bins: list[str], jobs: int, timeout: int) -> tuple[int, int, list, list]:
    """Run binaries with a thread pool + serial recovery pass for crashers.

    Phase 1: parallel (jobs). Phase 2: any crash/signal/rc=127 failures are
    rebuilt and re-run serially — catches load-induced flakes. Only phase-2
    residual failures gate CI; there are no pre-existing waivers.
    """
    total_passed = 0
    total_failed = 0
    failures: list[tuple] = []
    skipped: list[str] = []

    runnable = []
    for b in bins:
        # Include missing bins that still have source so refresh/rebuild can recover.
        if (BUILD / b).is_file() or _test_binary_has_source(b):
            runnable.append(b)
        else:
            skipped.append(b)
            print(f"  {Y}⊘{N} {b} (not built, no source)")

    if not runnable:
        return total_passed, total_failed, failures, skipped

    # Phase 0: bulk refresh when a modest number of bins are SO-stale.
    # Large skew (local half-rebuilt trees) rebuilds on-demand in run_one
    # to avoid a multi-hour ninja front-load.
    # CI: cmd_build already linked all_test_issue_targets — skip the happy-path
    # ninja insert (serial recovery still rebuilds on rc=127 / crash / undef).
    present = [b for b in runnable if (BUILD / b).is_file()]
    stale_n = sum(1 for b in present if _binary_stale_vs_shared_sos(BUILD / b))
    ci_skip_refresh = str(os.environ.get("AURA_CI", "")).strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }
    force_refresh = os.environ.get("AURA_ISSUES_REFRESH_STALE", "").strip() in {"1", "true", "yes"}
    if ci_skip_refresh and stale_n > 0 and not force_refresh:
        print(
            f"{Y}Note: {stale_n} issue binaries look older than shared SOs; "
            f"CI skips bulk refresh (rebuild on rc=127/crash only; "
            f"AURA_ISSUES_REFRESH_STALE=1 to force).{N}"
        )
    elif 0 < stale_n <= 40:
        refresh_stale_issue_binaries(present)
    elif stale_n > 40:
        print(
            f"{Y}Note: {stale_n} issue binaries look older than shared SOs; "
            f"will rebuild on-demand after rc=127/crash (set "
            f"AURA_ISSUES_REFRESH_STALE=1 to bulk-relink first).{N}"
        )
        if force_refresh:
            refresh_stale_issue_binaries(present)

    workers = max(1, min(jobs, len(runnable)))
    phase1: dict[str, tuple[int, int, int, str]] = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(run_one, b, timeout): b for b in runnable}
        for fut in as_completed(futures):
            b, passed, failed, rc, err = fut.result()
            phase1[b] = (passed, failed, rc, err)

    # Phase 2: serial recovery for any failure from phase 1.
    # Crash/timeout/symbol get a rebuild first; pure AC (rc=1) get a clean
    # solo re-run (rules out /tmp collisions and parallel resource pressure).
    recovery: list[str] = []
    for b, (_passed, failed, rc, _err) in phase1.items():
        if rc != 0 or failed > 0:
            recovery.append(b)

    if recovery:
        print(
            f"\n{Y}Serial recovery: re-running {len(recovery)} failures under jobs=1 "
            f"(load isolation + optional rebuild)...{N}"
        )
        crash_like = [
            b
            for b in recovery
            if phase1[b][2] in _CRASH_RCS
            or phase1[b][2] in (127, 124)
            or "undefined symbol" in (phase1[b][3] or "")
            or "symbol lookup error" in (phase1[b][3] or "")
        ]
        if crash_like:
            build_targets(crash_like, timeout_s=min(1200, 30 * max(1, len(crash_like))))
        for b in recovery:
            name, passed, failed, rc, err = _run_one_attempt(b, timeout)
            # Extra rebuild+retry if still crash/symbol after first solo attempt.
            if rc in _CRASH_RCS or rc == 127 or (err and "undefined symbol" in (err or "")):
                build_targets([b], timeout_s=300)
                name, passed, failed, rc, err = _run_one_attempt(b, timeout)
            phase1[b] = (passed, failed, rc, err)
    # Classify final results.
    for b in runnable:
        if b not in phase1:
            continue
        passed, failed, rc, err = phase1[b]
        total_passed += passed
        total_failed += failed
        _classify_result(
            b,
            passed,
            failed,
            rc,
            err,
            failures=failures,
        )

    return total_passed, total_failed, failures, skipped


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="run_issue_tests.py")
    ap.add_argument("--build", action="store_true", help="build targets first")
    ap.add_argument("--tier", default=None, choices=["fast", "full"], help="issue test tier")
    ap.add_argument("--filter", default=None, help="only run tests matching substring")
    ap.add_argument("--jobs", type=int, default=None, help="parallel workers (default: min(8, cpu))")
    ap.add_argument("--timeout", type=int, default=60, help="per-test timeout (seconds)")
    ap.add_argument("--list", action="store_true", help="list available tests")
    ap.add_argument(
        "--changed",
        action="store_true",
        help="only run git-changed issue tests (Issue #871 diff-aware mode; force tier=fast)",
    )
    # Issue #884: topic/profile-based selection (substring match on binary name).
    ap.add_argument(
        "--profile",
        default=None,
        help="topic profile filter (substring match on binary name, e.g. 'edsl', 'jit', '809')",
    )
    # Issue #886: machine-readable CI report.
    ap.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON summary to stdout after the human report",
    )
    args = ap.parse_args(argv)
    tier = args.tier or issues_tier()
    # Full tier: default jobs=4 (not 8) — high parallelism was the main
    # driver of SIGSEGV/timeout under load. Override with AURA_ISSUES_JOBS.
    _cpu = os.cpu_count() or 4
    _default_jobs = min(4, _cpu) if tier == "full" else min(8, _cpu)
    jobs = args.jobs or int(os.environ.get("AURA_ISSUES_JOBS", str(_default_jobs)))
    changed_only = args.changed
    if changed_only and tier == "full":
        tier = "fast"

    bins = discover_test_issue_binaries()
    bins = filter_bins_for_tier(bins, tier)
    if args.filter:
        bins = [b for b in bins if args.filter in b]
    if args.profile:
        # Issue #884: profile is an additional name filter (topic-based).
        bins = [b for b in bins if args.profile.lower() in b.lower()]

    if changed_only:
        # Issue #871: 减法 close diff-aware filter. Restrict
        # the discovered bins to only the ones whose source
        # touched the git working tree (so PR simulation runs
        # ONLY the issue tests that the PR actually affects,
        # not the whole fast bundle).
        changed_set = set(git_changed_issue_targets())
        if changed_set:
            bins = [b for b in bins if b in changed_set]
        else:
            # No git-changed sources — fall back to the fast
            # subset so --changed has SOME useful output even
            # when nothing in the working tree is touched.
            bins = filter_bins_for_tier(discover_test_issue_binaries(), "fast")

    if args.list:
        print(f"Available test_issue_* binaries ({len(bins)}, tier={tier}):")
        for b in bins:
            print(f"  {b}")
        return 0

    if not bins and tier == "fast":
        bins = resolve_issue_targets("fast")

    if not bins:
        print(f"{Y}No test_issue_* binaries found in {BUILD}{N}")
        if tier == "full":
            build_targets(discover_test_issue_targets())
        else:
            build_targets(resolve_issue_targets("fast"))
        bins = discover_test_issue_binaries()
        bins = filter_bins_for_tier(bins, tier)
        if args.filter:
            bins = [b for b in bins if args.filter in b]
        if not bins:
            print(f"{R}No test_issue_* binaries available after build.{N}")
            return 1

    if args.build:
        if tier == "full":
            build_targets(discover_test_issue_targets())
        else:
            build_targets(resolve_issue_targets("fast"))
        bins = discover_test_issue_binaries()
        bins = filter_bins_for_tier(bins, tier)
        if args.filter:
            bins = [b for b in bins if args.filter in b]

    print(f"{B}═══ Running {len(bins)} test_issue_* binaries (tier={tier}, jobs={jobs}) ═══{N}\n")
    t0 = time.time()
    total_passed, total_failed, failures, skipped = run_bins_parallel(bins, jobs, args.timeout)
    elapsed = time.time() - t0

    print(f"\n{B}════════════════════════════════════════{N}")
    print(
        f"Tests: {G}{len(bins) - len(failures) - len(skipped)}{N} ran, "
        f"{G}{total_passed} passed{N}, "
        f"{R}{total_failed} failed{N}, "
        f"{Y}{len(skipped)} skipped{N}"
    )
    print(f"Time: {elapsed:.1f}s (tier={tier}, jobs={jobs})")
    if failures:
        print(f"\n{R}Failures (will fail CI):{N}")
        for b, p, f, rc, err in failures:
            print(f"  - {b}: rc={rc}, {p} passed, {f} failed")
            if err:
                print(f"      {err[:200]}")
    if args.json:
        # Issue #886: machine-readable summary for CI dashboards.
        import json

        report = {
            "tier": tier,
            "jobs": jobs,
            "profile": args.profile,
            "elapsed_s": round(elapsed, 3),
            "bins": len(bins),
            "passed": total_passed,
            "failed": total_failed,
            "skipped": len(skipped),
            "failures": [{"binary": b, "passed": p, "failed": f, "rc": rc} for b, p, f, rc, _ in failures],
        }
        print(json.dumps(report, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
