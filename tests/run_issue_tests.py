#!/usr/bin/env python3
"""
run_issue_tests.py — unified runner for all test_issue_*.cpp binaries.

Builds (or assumes built) all test_issue_* binaries via ninja, then runs
each in parallel, aggregating pass/fail counts. Exits 0 on full pass,
1 on any failure.

Usage:
  python3 tests/run_issue_tests.py                # run all (full tier)
  python3 tests/run_issue_tests.py --tier fast    # PR subset + git-changed
  python3 tests/run_issue_tests.py --build        # build first
  python3 tests/run_issue_tests.py --filter 196   # run only #196
  python3 tests/run_issue_tests.py --jobs 8       # parallel execution
  python3 tests/run_issue_tests.py --timeout 30   # per-test timeout (default 60)
  python3 tests/run_issue_tests.py --list         # list available tests

Wired into:
  - build.py: cmd_test("issues") dispatches here
  - .github/workflows/ci.yml: PR uses AURA_ISSUES_TIER=fast
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock

from _aura_harness import AURA_BIN, BUILD, ROOT, B, G, N, R, Y
from issue_tier import _member_to_bundle, git_changed_issue_targets, issues_tier, resolve_issue_targets

# Pre-existing test failures (NOT caused by recent PRs).
PRE_EXISTING_FAILURES: set[str] = set()

_print_lock = Lock()


def _bundled_standalone_members() -> set[str]:
    """Issue members linked into bundles — skip stale standalone binaries."""
    return set(_member_to_bundle().keys())


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
        if (
            name.startswith("test_issues_")
            or name.startswith("test_obs_")
            or name.startswith("test_domain_")
            or name.startswith("test_aura_result_")
            or (name.startswith("test_issue_") and name not in bundled)
            or name.startswith("test_primitives_hotpath")
        ):
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
        if (
            (
                name.startswith("test_issues_")
                or name.startswith("test_issue_")
                or name.startswith("test_obs_")
                or name.startswith("test_domain_")
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


def build_targets(targets: list[str]) -> int:
    """Build the given test_issue_* targets via ninja (-k 0)."""
    if not targets:
        return 0
    print(f"{B}Building {len(targets)} test_issue_* binaries (ninja -k 0)...{N}")
    cmd = ["ninja", "-k", "0", "-C", str(BUILD)] + targets
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{Y}Some targets failed to build (pre-existing). Continuing with what built.{N}")
    return 0


def run_one(bin_name: str, timeout: int) -> tuple[str, int, int, int, str]:
    """Run one test binary. Returns (name, passed, failed, returncode, error_msg)."""
    bin_path = BUILD / bin_name
    if not bin_path.is_file():
        return bin_name, 0, 0, 127, f"binary not found: {bin_path}"
    # Per-binary timeout scaling. Bench / stress / large late bundles
    # need more than the default 60s (test_issue_159_bench alone can
    # exceed a minute; multi-member late* bundles are longer).
    # late1 alone can exceed 6 min under parallel load on aarch64 CI
    # (was timing out at 60*4=240s with rc=124).
    is_heavy = (
        "bench" in bin_name
        or bin_name == "test_issues_jit"
        or bin_name.startswith("test_jit_")
        or bin_name.startswith("test_issues_jit_late")
        or bin_name.startswith("test_issues_fiber")
    )
    is_very_heavy = bin_name in (
        "test_issues_jit_late1",
        "test_issues_jit_late3",
        "test_issues_jit_late4",
    )
    if is_very_heavy:
        eff_timeout = timeout * 10  # 600s default
    elif is_heavy:
        eff_timeout = timeout * 4
    else:
        eff_timeout = timeout
    # Issue #226 follow-up: pass AURA_BIN + AURA_SRC_ROOT to
    # subprocesses so tests that shell out to the aura binary
    # (test_issue_294, test_issue_295) can resolve relative
    # paths regardless of cwd. The bundle binaries themselves
    # don't read these vars, but the tests they link do.
    # Use ROOT as cwd (consistent with build.py / CI infra)
    # so the test's `cd <repo_root>` works as expected.
    env = {
        **os.environ,
        "AURA_BIN": str(AURA_BIN),
        "AURA_SRC_ROOT": str(ROOT),
    }
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
    passed, failed = parse_pass_fail_count(r.stdout)
    if passed + failed == 0:
        if r.returncode == 0:
            return bin_name, 1, 0, 0, ""
        return bin_name, 0, 1, r.returncode, r.stderr[-500:] if r.stderr else "no output"
    err = ""
    if r.returncode != 0 and r.returncode not in (0, 1):
        member = _last_bundle_member(r.stdout)
        if member:
            err = f"crashed during bundle member {member}"
        elif r.stderr:
            err = r.stderr[-500:]
        else:
            err = "no output"
    return bin_name, passed, failed, r.returncode, err


def _print_result(
    b: str,
    passed: int,
    failed: int,
    rc: int,
    err: str,
    *,
    pre_existing: bool,
) -> None:
    with _print_lock:
        if rc == 0 and failed == 0:
            print(f"  {G}✓{N} {b} ({passed} passed)")
        elif pre_existing:
            print(f"  {Y}⚠{N} {b} ({passed} passed, {failed} failed, rc={rc}) [pre-existing]")
        else:
            print(f"  {R}✗{N} {b} ({passed} passed, {failed} failed, rc={rc})")
            if err:
                print(f"      {err[:200]}")


def run_bins_parallel(bins: list[str], jobs: int, timeout: int) -> tuple[int, int, list, list, list]:
    """Run binaries with a thread pool. Returns aggregate stats."""
    total_passed = 0
    total_failed = 0
    failures: list[tuple] = []
    pre_existing_failures: list[tuple] = []
    skipped: list[str] = []

    runnable = []
    for b in bins:
        if (BUILD / b).is_file():
            runnable.append(b)
        else:
            skipped.append(b)
            print(f"  {Y}⊘{N} {b} (not built)")

    if not runnable:
        return total_passed, total_failed, failures, pre_existing_failures, skipped

    workers = max(1, min(jobs, len(runnable)))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(run_one, b, timeout): b for b in runnable}
        for fut in as_completed(futures):
            b, passed, failed, rc, err = fut.result()
            total_passed += passed
            total_failed += failed
            if b in PRE_EXISTING_FAILURES and (rc != 0 or failed > 0):
                pre_existing_failures.append((b, passed, failed, rc, err))
                _print_result(b, passed, failed, rc, err, pre_existing=True)
            elif rc == 0 and failed == 0:
                _print_result(b, passed, failed, rc, err, pre_existing=False)
            else:
                failures.append((b, passed, failed, rc, err))
                _print_result(b, passed, failed, rc, err, pre_existing=False)

    return total_passed, total_failed, failures, pre_existing_failures, skipped


def main():
    ap = argparse.ArgumentParser()
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
    args = ap.parse_args()
    tier = args.tier or issues_tier()
    jobs = args.jobs or int(os.environ.get("AURA_ISSUES_JOBS", str(min(8, os.cpu_count() or 4))))
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
    total_passed, total_failed, failures, pre_existing_failures, skipped = run_bins_parallel(bins, jobs, args.timeout)
    elapsed = time.time() - t0

    print(f"\n{B}════════════════════════════════════════{N}")
    print(
        f"Tests: {G}{len(bins) - len(failures) - len(skipped)}{N} ran, "
        f"{G}{total_passed} passed{N}, "
        f"{R}{total_failed} failed{N}, "
        f"{Y}{len(skipped)} skipped{N}, "
        f"{Y}{len(pre_existing_failures)} pre-existing{N}"
    )
    print(f"Time: {elapsed:.1f}s (tier={tier}, jobs={jobs})")
    if failures:
        print(f"\n{R}NEW Failures (will fail CI):{N}")
        for b, p, f, rc, err in failures:
            print(f"  - {b}: rc={rc}, {p} passed, {f} failed")
            if err:
                print(f"      {err[:200]}")
    if pre_existing_failures:
        print(f"\n{Y}Pre-existing Failures (NOT failing CI, tracked separately):{N}")
        for b, p, f, rc, _err in pre_existing_failures:
            print(f"  - {b}: rc={rc}, {p} passed, {f} failed")
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
            "pre_existing": len(pre_existing_failures),
            "failures": [{"binary": b, "passed": p, "failed": f, "rc": rc} for b, p, f, rc, _ in failures],
        }
        print(json.dumps(report, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
