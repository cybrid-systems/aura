#!/usr/bin/env python3
"""Primary Python test entry point (#1961).

Unify discovery of how Aura tests are invoked. Prefer this CLI (or
./build.py test …) over ad-hoc scripts.

Usage:
  python3 tests/run.py list
  python3 tests/run.py issues [--tier fast|full] [run_issue_tests flags…]
  python3 tests/run.py issues-fast
  python3 tests/run.py fixtures
  python3 tests/run.py gradual [--verbose]
  python3 tests/run.py bench [-- …benchmark.py args…]
  python3 tests/run.py mutation [-- …mutation_loop.py args…]
  python3 tests/run.py bash

  python3 tests/run.py issues --json          # machine-readable summary
  python3 tests/run.py --json fixtures        # JSON wrap for simple categories

Legacy scripts remain callable for transition:
  tests/run_issue_tests.py → same as `run.py issues`
  tests/fixture_check.py   → same as `run.py fixtures`
  tests/check_gradual.py   → same as `run.py gradual`

See tests/README.md § Running tests.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

# Allow sibling imports when run as `python3 tests/run.py`
_TESTS = Path(__file__).resolve().parent
if str(_TESTS) not in sys.path:
    sys.path.insert(0, str(_TESTS))

from _aura_harness import (  # noqa: E402
    AURA_BIN,
    BUILD,
    CATEGORIES,
    ROOT,
    B,
    N,
    RunReport,
    print_json_report,
    timed_call,
)


def _cmd_list(_: argparse.Namespace) -> int:
    print(f"{B}Aura test categories (python3 tests/run.py <cmd>){N}\n")
    width = max(len(k) for k in CATEGORIES)
    for name, desc in CATEGORIES.items():
        if name == "list":
            continue
        print(f"  {name:<{width}}  {desc}")
    print(
        "\nAlso available via ./build.py test {unit,integ,smoke,issues,issues-fast,gradual,bench,mutation,bash,suite,…}"
    )
    print(f"Build dir: {BUILD}")
    print(f"Aura bin:  {AURA_BIN}  ({'ok' if AURA_BIN.is_file() else 'missing'})")
    return 0


def _cmd_issues(args: argparse.Namespace) -> int:
    import run_issue_tests as rit

    # Rebuild argv for run_issue_tests.main()
    forwarded: list[str] = []
    tier = args.tier
    if args.command == "issues-fast":
        tier = tier or "fast"
        os.environ["AURA_ISSUES_TIER"] = "fast"
    if tier:
        forwarded.extend(["--tier", tier])
    if args.json_report:
        forwarded.append("--json")
    if getattr(args, "build", False):
        forwarded.append("--build")
    if getattr(args, "list_bins", False):
        forwarded.append("--list")
    if getattr(args, "changed", False):
        forwarded.append("--changed")
    if getattr(args, "filter", None):
        forwarded.extend(["--filter", args.filter])
    if getattr(args, "jobs", None) is not None:
        forwarded.extend(["--jobs", str(args.jobs)])
    if getattr(args, "timeout", None) is not None:
        forwarded.extend(["--timeout", str(args.timeout)])
    if getattr(args, "profile", None):
        forwarded.extend(["--profile", args.profile])
    rest = list(getattr(args, "rest", []) or [])
    if rest and rest[0] == "--":
        rest = rest[1:]
    forwarded.extend(rest)
    return int(rit.main(forwarded))


def _cmd_fixtures(args: argparse.Namespace) -> int:
    import fixture_check as fc

    report = timed_call(fc.run_check, category="fixtures")
    if args.json_report:
        print_json_report(report)
    else:
        # fixture_check already prints human lines; add uniform trailer
        report.print_human()
    return report.exit_code


def _cmd_gradual(args: argparse.Namespace) -> int:
    import check_gradual as cg

    # check_gradual reads sys.argv for --verbose
    saved = sys.argv
    try:
        sys.argv = ["check_gradual.py", *(["--verbose"] if args.verbose else [])]
        report = timed_call(cg.main, category="gradual")
    finally:
        sys.argv = saved
    if args.json_report:
        print_json_report(report)
    else:
        report.print_human()
    return report.exit_code


def _cmd_script(args: argparse.Namespace, script: str, category: str) -> int:
    path = _TESTS / script
    cmd = [sys.executable, str(path), *args.rest]
    t0 = __import__("time").time()
    rc = subprocess.run(cmd, cwd=str(ROOT)).returncode
    elapsed = __import__("time").time() - t0
    report = RunReport(
        category=category,
        passed=1 if rc == 0 else 0,
        failed=0 if rc == 0 else 1,
        elapsed_s=round(elapsed, 3),
        exit_code=rc,
        extra={"script": script},
    )
    if args.json_report:
        print_json_report(report)
    else:
        report.print_human()
    return report.exit_code


def _cmd_bash(args: argparse.Namespace) -> int:
    script = _TESTS / "run-tests.sh"
    env = {**os.environ, "AURA": os.environ.get("AURA", str(AURA_BIN))}
    t0 = __import__("time").time()
    rc = subprocess.run(["bash", str(script), *args.rest], cwd=str(ROOT), env=env).returncode
    elapsed = __import__("time").time() - t0
    report = RunReport(
        category="bash",
        passed=1 if rc == 0 else 0,
        failed=0 if rc == 0 else 1,
        elapsed_s=round(elapsed, 3),
        exit_code=rc,
        extra={"script": "run-tests.sh"},
    )
    if args.json_report:
        print_json_report(report)
    else:
        report.print_human()
    return report.exit_code


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="tests/run.py",
        description="Primary Aura Python test runner (#1961).",
    )
    ap.add_argument(
        "--json",
        dest="json_report",
        action="store_true",
        help="emit a machine-readable RunReport JSON trailer (where supported)",
    )
    sub = ap.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="List categories")
    p_list.set_defaults(func=_cmd_list)

    for name, help_ in (
        ("issues", "Run issue/domain/bundle C++ tests"),
        ("issues-fast", "Run issues with AURA_ISSUES_TIER=fast"),
    ):
        p = sub.add_parser(name, help=help_)
        p.add_argument("--tier", choices=("fast", "full"), default=None)
        p.add_argument("--build", action="store_true", help="build targets first")
        p.add_argument("--list", dest="list_bins", action="store_true", help="list binaries")
        p.add_argument("--changed", action="store_true", help="git-changed only")
        p.add_argument("--filter", default=None, help="substring filter")
        p.add_argument("--jobs", type=int, default=None)
        p.add_argument("--timeout", type=int, default=None)
        p.add_argument("--profile", default=None, help="name substring profile")
        p.add_argument(
            "rest",
            nargs=argparse.REMAINDER,
            help="extra flags for run_issue_tests.py (optional; prefix with --)",
        )
        p.set_defaults(func=_cmd_issues)

    p_fx = sub.add_parser("fixtures", help="Validate tests/fixtures JSON")
    p_fx.set_defaults(func=_cmd_fixtures)

    p_gr = sub.add_parser("gradual", help="Gradual typing guarantee")
    p_gr.add_argument("--verbose", action="store_true")
    p_gr.set_defaults(func=_cmd_gradual)

    p_be = sub.add_parser("bench", help="Compiler benchmark gate (benchmark.py)")
    p_be.add_argument("rest", nargs=argparse.REMAINDER)
    p_be.set_defaults(func=lambda a: _cmd_script(a, "benchmark.py", "bench"))

    p_mu = sub.add_parser("mutation", help="mutation_loop.py")
    p_mu.add_argument("rest", nargs=argparse.REMAINDER)
    p_mu.set_defaults(func=lambda a: _cmd_script(a, "mutation_loop.py", "mutation"))

    p_ba = sub.add_parser("bash", help="Legacy run-tests.sh")
    p_ba.add_argument("rest", nargs=argparse.REMAINDER)
    p_ba.set_defaults(func=_cmd_bash)

    return ap


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    # Allow `tests/run.py --json fixtures` as well as `tests/run.py fixtures --json`
    json_flag = False
    if "--json" in argv:
        json_flag = True
        argv = [a for a in argv if a != "--json"]
    ap = build_parser()
    args = ap.parse_args(argv)
    if json_flag:
        args.json_report = True
    # REMAINDER includes leading '--' sometimes; strip one.
    if hasattr(args, "rest") and args.rest and args.rest[0] == "--":
        args.rest = args.rest[1:]
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
