#!/usr/bin/env python3
"""Parallel / changed-scoped coverage check runner (gate hot path).

Replaces the historical serial `or cmd_*_coverage()` chain inside
`./build.py gate` (~400+ subprocesses, plus nested cascade re-runs).

Usage:
  python3 scripts/coverage/run_checks.py --all
  python3 scripts/coverage/run_checks.py --changed
  python3 scripts/coverage/run_checks.py --changed --base origin/main
  python3 scripts/coverage/run_checks.py --all --jobs 12
  python3 scripts/coverage/run_checks.py --list

Env:
  AURA_GATE_JOBS          default parallel workers (else min(16, nproc))
  AURA_COVERAGE_NO_CASCADE  default 1 here; nested check_* subprocesses skip
  AURA_GATE_SERIAL=1      force --jobs 1
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKS_DIR = Path(__file__).resolve().parent / "checks"
SCRIPTS_DIR = ROOT / "scripts"
WRAPPER = Path(__file__).resolve().parent / "check_wrapper.py"

# Touching these paths forces a full check run under --changed
# (same policy as scripts/coverage/runner.py manifests).
ALWAYS_ALL_PREFIXES = (
    "scripts/coverage/",
    "build.py",
    "CMakeLists.txt",
    ".githooks/",
)

# Paths referenced in check bodies that are not source-of-truth for selection.
_SKIP_PATH_PREFIXES = (
    "docs/design/",  # forbid-only presence checks
)

_READ_PATH_RE = re.compile(r"""(?:_read|read_text|open)\(\s*["']([^"']+)["']""")
_STR_PATH_RE = re.compile(r"""["']((?:src|tests|scripts|lib|cmake|contracts|docs)/[^"']+)["']""")


def _truthy(name: str, default: str = "0") -> bool:
    return os.environ.get(name, default).strip().lower() in ("1", "true", "yes", "on")


def _is_thin_wrapper(path: Path) -> bool:
    """Manifest-backed `runner.py --issue N` shims — run_checks drives runner.py."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return (
        "runner.py" in text
        and "--issue" in text
        and text.count("def ") <= 3
        and "must(" not in text.replace("_must(", "_X(")
    )


def discover_checks() -> list[Path]:
    """All static coverage check scripts gate should run."""
    out: list[Path] = []
    if CHECKS_DIR.is_dir():
        out.extend(sorted(CHECKS_DIR.glob("check_*.py")))
    # Legacy / special scripts living under scripts/ (not checks/).
    if SCRIPTS_DIR.is_dir():
        out.extend(sorted(SCRIPTS_DIR.glob("check_*.py")))
    # De-dupe by resolve, preserve order
    seen: set[Path] = set()
    uniq: list[Path] = []
    for p in out:
        rp = p.resolve()
        if rp in seen:
            continue
        seen.add(rp)
        if _is_thin_wrapper(p):
            continue
        uniq.append(p)
    return uniq


def _git_lines(args: list[str]) -> list[str]:
    try:
        r = subprocess.run(
            ["git", *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return []
    if r.returncode != 0:
        return []
    return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]


def changed_files(base: str) -> tuple[list[str], str | None]:
    rev_list = _git_lines(["rev-parse", "--verify", base])
    if not rev_list:
        note = f"base {base!r} unavailable; using staged+unstaged+untracked only"
        files: set[str] = set()
    else:
        note = None
        mb = _git_lines(["merge-base", base, "HEAD"])
        range_a = mb[0] if mb else base
        files = set(_git_lines(["diff", "--name-only", f"{range_a}...HEAD"]))

    files.update(_git_lines(["diff", "--name-only", "--cached"]))
    files.update(_git_lines(["diff", "--name-only"]))
    files.update(_git_lines(["ls-files", "--others", "--exclude-standard"]))
    return sorted(files), note


def forces_all(changed: list[str]) -> bool:
    for f in changed:
        for pref in ALWAYS_ALL_PREFIXES:
            if f == pref.rstrip("/") or f.startswith(pref):
                return True
    return False


def script_declared_paths(script: Path) -> set[str]:
    """Best-effort path set referenced by a check script body."""
    try:
        text = script.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return set()
    paths: set[str] = set()
    rel_self = str(script.relative_to(ROOT)).replace("\\", "/")
    paths.add(rel_self)
    for m in _READ_PATH_RE.finditer(text):
        paths.add(m.group(1))
    for m in _STR_PATH_RE.finditer(text):
        p = m.group(1)
        if any(p.startswith(pref) for pref in _SKIP_PATH_PREFIXES):
            continue
        paths.add(p)
    return paths


def select_checks_for_changed(checks: list[Path], changed: list[str]) -> tuple[list[Path], str]:
    if forces_all(changed):
        return checks, "diff touches runner/build.py/CMakeLists/.githooks → --all"
    if not changed:
        return [], "empty diff"

    changed_set = set(changed)
    selected: list[Path] = []
    for script in checks:
        paths = script_declared_paths(script)
        hit = False
        for p in paths:
            if p in changed_set:
                hit = True
                break
            # directory / prefix intersection
            for c in changed_set:
                if c.startswith(p.rstrip("/") + "/") or p.startswith(c.rstrip("/") + "/"):
                    hit = True
                    break
            if hit:
                break
        if hit:
            selected.append(script)
    return selected, f"{len(changed)} changed file(s) → {len(selected)} related check(s)"


def default_jobs() -> int:
    if _truthy("AURA_GATE_SERIAL", "0"):
        return 1
    env = os.environ.get("AURA_GATE_JOBS", "").strip()
    if env.isdigit() and int(env) > 0:
        return int(env)
    nproc = os.cpu_count() or 4
    return max(1, min(16, nproc))


def _run_manifests(extra: list[str]) -> int:
    """Run declarative manifests in-process via runner.py (no check_*.py spawn)."""
    runner = ROOT / "scripts" / "coverage" / "runner.py"
    if not runner.is_file():
        print("FAIL: missing scripts/coverage/runner.py", file=sys.stderr)
        return 1
    print(f"coverage manifests: python3 {runner.name} {' '.join(extra)}", flush=True)
    r = subprocess.run([sys.executable, str(runner), *extra], cwd=ROOT)
    return r.returncode


def run_one(script: Path, *, no_cascade: bool) -> tuple[str, int, float, str]:
    env = os.environ.copy()
    if no_cascade:
        env["AURA_COVERAGE_NO_CASCADE"] = "1"
    else:
        env.setdefault("AURA_COVERAGE_NO_CASCADE", "0")
    t0 = time.perf_counter()
    try:
        r = subprocess.run(
            [sys.executable, str(WRAPPER), str(script)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )
        dt = time.perf_counter() - t0
        tail = (r.stderr or r.stdout or "").strip()
        if len(tail) > 400:
            tail = tail[-400:]
        return script.name, r.returncode, dt, tail
    except OSError as e:
        dt = time.perf_counter() - t0
        return script.name, 1, dt, str(e)


def run_checks(
    checks: list[Path],
    *,
    jobs: int,
    no_cascade: bool,
    label: str,
) -> int:
    if not checks:
        print(f"OK: {label} — no checks selected (skip)")
        return 0

    print(f"{label}: {len(checks)} check(s), jobs={jobs}, no_cascade={int(no_cascade)}")
    t0 = time.perf_counter()
    results: list[tuple[str, int, float, str]] = []

    if jobs <= 1:
        for s in checks:
            results.append(run_one(s, no_cascade=no_cascade))
            name, rc, dt, _ = results[-1]
            status = "OK" if rc == 0 else "FAIL"
            print(f"  [{status}] {dt:5.2f}s  {name}", flush=True)
    else:
        # Stream completions for progress without flooding (print failures immediately).
        done = 0
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = {ex.submit(run_one, s, no_cascade=no_cascade): s for s in checks}
            for fut in as_completed(futs):
                row = fut.result()
                results.append(row)
                done += 1
                name, rc, dt, tail = row
                if rc != 0:
                    print(f"  [FAIL] {dt:5.2f}s  {name}", flush=True)
                    if tail:
                        for line in tail.splitlines()[-8:]:
                            print(f"         {line}", flush=True)
                elif done % 50 == 0 or done == len(checks):
                    print(f"  … {done}/{len(checks)} complete", flush=True)

    wall = time.perf_counter() - t0
    fails = [r for r in results if r[1] != 0]
    slow = sorted(results, key=lambda r: -r[2])[:8]
    print(
        f"OK: {label} — {len(results) - len(fails)}/{len(results)} passed "
        f"in {wall:.1f}s (sum {sum(r[2] for r in results):.1f}s)"
        if not fails
        else f"FAIL: {label} — {len(fails)}/{len(results)} failed in {wall:.1f}s"
    )
    if slow:
        print("  slowest:")
        for name, rc, dt, _ in slow:
            mark = " FAIL" if rc else ""
            print(f"    {dt:5.2f}s{mark}  {name}")
    if fails:
        print(f"\n{len(fails)} check(s) failed:", file=sys.stderr)
        for name, rc, dt, tail in fails:
            print(f"  FAIL {name} (rc={rc}, {dt:.2f}s)", file=sys.stderr)
            if tail:
                print(f"    {tail[:300]}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--all", action="store_true", help="Run every discovered check_*.py")
    g.add_argument(
        "--changed",
        action="store_true",
        help="Run only checks whose declared paths intersect the git diff",
    )
    g.add_argument("--list", action="store_true", help="List discovered checks and exit")
    ap.add_argument(
        "--base",
        default="origin/main",
        help="Git ref for --changed merge-base (default: origin/main)",
    )
    ap.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Parallel workers (default: AURA_GATE_JOBS or min(16,nproc); 1=serial)",
    )
    ap.add_argument(
        "--cascade",
        action="store_true",
        help="Allow nested check_* re-runs (default: suppress cascades)",
    )
    args = ap.parse_args(argv)

    checks = discover_checks()
    if args.list:
        for p in checks:
            print(p.relative_to(ROOT))
        print(f"# {len(checks)} check(s)")
        return 0

    jobs = args.jobs if args.jobs > 0 else default_jobs()
    no_cascade = not args.cascade
    # Default ON for this runner (gate path); --cascade opts out.
    if no_cascade:
        os.environ.setdefault("AURA_COVERAGE_NO_CASCADE", "1")

    if args.changed:
        changed, note = changed_files(args.base)
        if note:
            print(f"note: {note}", file=sys.stderr)
        print(f"--changed base={args.base!r} files={len(changed)}")
        selected, reason = select_checks_for_changed(checks, changed)
        print(f"  select: {reason}")
        rc = run_checks(selected, jobs=jobs, no_cascade=no_cascade, label="coverage --changed")
        rc2 = _run_manifests(["--changed", "--base", args.base])
        return rc or rc2

    rc = run_checks(checks, jobs=jobs, no_cascade=no_cascade, label="coverage --all")
    rc2 = _run_manifests(["--all"])
    return rc or rc2


if __name__ == "__main__":
    raise SystemExit(main())
