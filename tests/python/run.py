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
  python3 tests/run.py home "<query>"
  python3 tests/run.py cover --issue N
  python3 tests/run.py cover --name <symbol>
  python3 tests/run.py process [--tier fast|l2]
  python3 tests/run.py affected [paths…]   # L1 home + std/test + smoke; no issue-farm

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
import ast
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

# Issue #1932: this file lives under tests/python/ (thin entry: tests/run.py).
_PYTHON = Path(__file__).resolve().parent
_TESTS = _PYTHON.parent
_BENCH = _TESTS / "bench"
if str(_PYTHON) not in sys.path:
    sys.path.insert(0, str(_PYTHON))

from _aura_harness import (  # noqa: E402
    AFFECTED_DOC,
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
from homes import render_cover, render_home  # noqa: E402


def _cmd_list(_: argparse.Namespace) -> int:
    print(f"{B}Aura test categories (python3 tests/run.py <cmd>){N}\n")
    width = max(len(k) for k in CATEGORIES)
    for name, desc in CATEGORIES.items():
        if name == "list":
            continue
        print(f"  {name:<{width}}  {desc}")
    print("\nAlso available via ./build.py test [tier|suite]  (default ci; see ./build.py list)")
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


def _cmd_script(args: argparse.Namespace, script: Path, category: str) -> int:
    path = script if script.is_absolute() else _PYTHON / script
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
        extra={"script": str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path)},
    )
    if args.json_report:
        print_json_report(report)
    else:
        report.print_human()
    return report.exit_code


def _cmd_bash(args: argparse.Namespace) -> int:
    script = _PYTHON / "run-tests.sh"
    env = {**os.environ, "AURA": os.environ.get("AURA", str(AURA_BIN))}
    # Issue #2213 / #2053: production defaults break bash harness unless Soft.
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
    # Issue #3627: AURA_SANDBOX=off no longer selects the walker on the
    # pack — harness cases exercise walker-side features by design, so
    # default the diagnostics face too (caller override wins).
    if not str(env.get("AURA_PIPELINE_STRICT", "")).strip():
        env["AURA_PIPELINE_STRICT"] = "0"
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


def _cmd_home(args: argparse.Namespace) -> int:
    code, text = render_home(" ".join(args.query))
    print(text, end="")
    return code


def _cmd_cover(args: argparse.Namespace) -> int:
    code, text = render_cover(issue=args.issue, name=args.name)
    print(text, end="")
    return code


def _cmd_affected(args: argparse.Namespace) -> int:
    from affected import main_affected

    return main_affected(list(args.paths), plan_only=args.plan)


_PROCESS_FIELDS = ("PROCESS_THEME", "PROCESS_TIMEOUT", "PROCESS_TIER", "CASES", "PARTS")


def _assigned_literals(path: Path) -> dict[str, object]:
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found: dict[str, object] = {}
    for node in tree.body:
        targets: list[ast.expr] = []
        value: ast.expr | None = None
        if isinstance(node, ast.Assign):
            targets = list(node.targets)
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
            value = node.value
        if value is None:
            continue
        for target in targets:
            if isinstance(target, ast.Name) and target.id in _PROCESS_FIELDS:
                found[target.id] = ast.literal_eval(value)
    return found


def _is_testcase(node: ast.ClassDef) -> bool:
    for base in node.bases:
        if isinstance(base, ast.Name) and base.id == "TestCase":
            return True
        if isinstance(base, ast.Attribute) and base.attr == "TestCase":
            return True
    return False


def _test_methods(path: Path) -> list[tuple[str, str]]:
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found: list[tuple[str, str]] = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name.startswith("test_"):
            raise ValueError(f"module-level {node.name}")
        if not isinstance(node, ast.ClassDef) or not _is_testcase(node):
            continue
        for item in node.body:
            if isinstance(item, ast.FunctionDef) and item.name.startswith("test_"):
                found.append((node.name, item.name))
    return found


def _validate_process(proc: Path) -> tuple[list[str], list[tuple[Path, int, str]]]:
    """Return (errors, [(theme path, timeout seconds, tier)]). Does not import."""
    errors: list[str] = []
    themes: list[tuple[Path, int, str]] = []
    referenced: set[str] = set()
    for path in sorted(proc.glob("test_*.py")):
        try:
            meta = _assigned_literals(path)
        except (SyntaxError, ValueError) as exc:
            errors.append(f"{path.name}: {exc}")
            continue
        theme = meta.get("PROCESS_THEME")
        tier = meta.get("PROCESS_TIER")
        timeout = meta.get("PROCESS_TIMEOUT")
        cases = meta.get("CASES")
        parts = meta.get("PARTS", ())
        if not isinstance(theme, str) or not theme:
            errors.append(f"{path.name}: missing PROCESS_THEME")
            continue
        if tier not in ("fast", "l2"):
            errors.append(f"{path.name}: PROCESS_TIER must be fast or l2")
            continue
        if not isinstance(timeout, int) or timeout <= 0:
            errors.append(f"{path.name}: PROCESS_TIMEOUT must be a positive int")
            continue
        if not isinstance(parts, tuple) or not all(isinstance(part, str) for part in parts):
            errors.append(f"{path.name}: PARTS must be a tuple of module names")
            continue
        if not isinstance(cases, tuple):
            errors.append(f"{path.name}: CASES must be a tuple")
            continue
        try:
            own = _test_methods(path)
        except (SyntaxError, ValueError) as exc:
            errors.append(f"{path.name}: {exc}")
            continue
        part_methods: dict[str, set[tuple[str, str]]] = {}
        for part in parts:
            if part.startswith("test_") or not part.isidentifier():
                errors.append(f"{path.name}: bad part name {part}")
                continue
            part_path = proc / f"{part}.py"
            if not part_path.is_file():
                errors.append(f"{path.name}: missing part {part}.py")
                continue
            referenced.add(part)
            try:
                part_methods[part] = set(_test_methods(part_path))
            except (SyntaxError, ValueError) as exc:
                errors.append(f"{part}.py: {exc}")
        specs: set[tuple[str, str, str]] = set()
        listed_own: set[tuple[str, str]] = set()
        listed_part: dict[str, set[tuple[str, str]]] = {part: set() for part in part_methods}
        for row in cases:
            if (
                not isinstance(row, tuple)
                or len(row) != 3
                or not isinstance(row[0], int)
                or not isinstance(row[1], str)
                or not isinstance(row[2], str)
            ):
                errors.append(f"{path.name}: bad CASES row {row!r}")
                continue
            bits = row[2].split(".")
            if len(bits) == 2:
                key = (bits[0], bits[1])
                if key in listed_own:
                    errors.append(f"{path.name}: duplicate {row[2]}")
                listed_own.add(key)
            elif len(bits) == 3:
                spec = (bits[0], bits[1], bits[2])
                if spec in specs:
                    errors.append(f"{path.name}: duplicate {row[2]}")
                specs.add(spec)
                if bits[0] not in part_methods:
                    errors.append(f"{path.name}: {row[2]} is not in PARTS")
                else:
                    listed_part[bits[0]].add((bits[1], bits[2]))
            else:
                errors.append(f"{path.name}: bad spec {row[2]}")
        for cls, method in own:
            if (cls, method) not in listed_own:
                errors.append(f"{path.name}: {cls}.{method} missing from CASES")
        for cls, method in listed_own:
            if (cls, method) not in own:
                errors.append(f"{path.name}: CASES lists missing {cls}.{method}")
        for part, methods in part_methods.items():
            for cls, method in methods:
                if (cls, method) not in listed_part.get(part, set()):
                    errors.append(f"{path.name}: {part}.{cls}.{method} missing from CASES")
            for cls, method in listed_part.get(part, set()):
                if (cls, method) not in methods:
                    errors.append(f"{path.name}: CASES lists missing {part}.{cls}.{method}")
        themes.append((path, timeout, tier))
    for part in sorted(proc.glob("*.py")):
        if part.name.startswith("test_"):
            continue
        if part.stem not in referenced:
            errors.append(f"orphan part {part.name}")
    return errors, themes


def _cmd_process(args: argparse.Namespace) -> int:
    proc = _PYTHON / "process"
    if not proc.is_dir():
        print("missing tests/python/process", file=sys.stderr)
        return 1
    errors, themes = _validate_process(proc)
    if errors:
        for line in errors:
            print(line, file=sys.stderr)
        return 1
    selected = [(path, timeout) for path, timeout, tier in themes if tier == args.tier]
    failed: list[tuple[str, int]] = []
    for path, timeout in selected:
        rel = path.relative_to(ROOT).as_posix()
        print(f"process {rel} timeout={timeout}s", flush=True)
        started = time.monotonic()
        child = subprocess.Popen(
            [sys.executable, str(path)],
            cwd=str(ROOT),
            start_new_session=True,
        )
        try:
            rc = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
            rc = 124
            print(f"TIMEOUT {rel} after {timeout}s", file=sys.stderr)
        elapsed = time.monotonic() - started
        print(f"process {rel} rc={rc} {elapsed:.1f}s", flush=True)
        if rc != 0:
            failed.append((rel, rc))
    if failed:
        for rel, rc in failed:
            print(f"FAIL {rel} rc={rc}", file=sys.stderr)
        return 1
    print(f"process: {len(selected)} theme(s) passed ({args.tier})")
    return 0


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

    p_be = sub.add_parser("bench", help="Compiler benchmark gate (tests/bench/benchmark.py)")
    p_be.add_argument("rest", nargs=argparse.REMAINDER)
    p_be.set_defaults(func=lambda a: _cmd_script(a, _BENCH / "benchmark.py", "bench"))

    p_mu = sub.add_parser("mutation", help="mutation_loop.py")
    p_mu.add_argument("rest", nargs=argparse.REMAINDER)
    p_mu.set_defaults(func=lambda a: _cmd_script(a, "mutation_loop.py", "mutation"))

    p_ba = sub.add_parser("bash", help="Legacy run-tests.sh")
    p_ba.add_argument("rest", nargs=argparse.REMAINDER)
    p_ba.set_defaults(func=_cmd_bash)

    p_e2e = sub.add_parser("e2e", help="commercial_readiness E2E + golden (#1934)")
    p_e2e.add_argument("rest", nargs=argparse.REMAINDER)
    p_e2e.set_defaults(func=lambda a: _cmd_script(a, "run_e2e.py", "e2e"))

    p_home = sub.add_parser("home", help="Print the theme file and an append snippet (#5225)")
    p_home.add_argument("query", nargs="+", help="natural language or symbol")
    p_home.set_defaults(func=_cmd_home)

    p_cov = sub.add_parser("cover", help="List cases that cover an issue or symbol (#5225)")
    group = p_cov.add_mutually_exclusive_group(required=True)
    group.add_argument("--issue", type=int, help="issue number")
    group.add_argument("--name", help="symbol or phrase")
    p_cov.set_defaults(func=_cmd_cover)

    p_proc = sub.add_parser(
        "process",
        help="Run tests/python/process themes (default tier: fast; l2 is the soak tier)",
    )
    p_proc.add_argument("--tier", choices=("fast", "l2"), default="fast")
    p_proc.set_defaults(func=_cmd_process)

    p_aff = sub.add_parser("affected", help=AFFECTED_DOC)
    p_aff.add_argument(
        "--plan",
        action="store_true",
        help="print the genealogy selection and do not run it",
    )
    p_aff.add_argument(
        "paths",
        nargs="*",
        help="repo-relative paths to map; omit to use the workspace git diff",
    )
    p_aff.set_defaults(func=_cmd_affected)

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
