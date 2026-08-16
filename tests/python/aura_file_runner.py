#!/usr/bin/env python3
"""Shared runner for .aura leaves (files + JSON snippets).

File cases (suite / regression):
  tests/suite/*.aura        --load file, default expect = exit 0
  tests/regression/*.aura   stdin (header ;; comments stripped), ``;; expect:``

Snippet cases (integ / p0 / typecheck fixtures):
  stdin + optional extra argv (--ir / --typecheck / --serve)
  judge_snippet: status, substring / >=N / exact (…), type: line, err needle or regex

Command cases (smoke): bash -c, substring in combined output.
e2e --load spawn is invoke_aura_load (golden checks stay in e2e_harness).
"""

from __future__ import annotations

import re
import subprocess
from collections.abc import Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

from _aura_harness import fail, ok, warn

SIG_MAP = {-6: "SIGABRT", -8: "SIGFPE", -11: "SIGSEGV"}


def invoke_aura_load(
    path: Path,
    *,
    aura_bin: str | Path,
    env: dict[str, str],
    timeout_s: float,
    cwd: str | Path | None = None,
) -> subprocess.CompletedProcess[str]:
    """Shared `aura --load path` spawn (suite files + e2e). May raise TimeoutExpired."""
    kwargs: dict = {
        "capture_output": True,
        "text": True,
        "timeout": timeout_s,
        "env": env,
    }
    if cwd is not None:
        kwargs["cwd"] = str(cwd)
    return subprocess.run([str(aura_bin), "--load", str(path)], **kwargs)


@dataclass(frozen=True)
class AuraFileCase:
    path: Path
    expect: str | None
    rel: str


@dataclass(frozen=True)
class DiscoverResult:
    cases: list[AuraFileCase]
    skipped: list[tuple[str, str]]  # (name, reason)


def first_expect(text: str) -> str | None:
    for line in text.splitlines():
        if line.startswith(";; expect:"):
            value = line[len(";; expect:") :].strip()
            return value or None
    return None


def stdin_payload(text: str) -> str:
    """Drop leading ``;;`` comment / blank lines (regression convention)."""
    lines: list[str] = []
    in_code = False
    for line in text.splitlines():
        if not in_code and not line.startswith(";;") and line.strip():
            in_code = True
        if in_code:
            lines.append(line)
    return "\n".join(lines)


def judge(exit_code: int, stdout: str, stderr: str, expect: str | None) -> tuple[bool, str]:
    """Return (ok, detail). detail is empty on success."""
    if expect == "no-crash":
        if exit_code < 0:
            return False, SIG_MAP.get(exit_code, f"signal{-exit_code}")
        return True, ""
    if expect == "no-error":
        if "internal error" in (stderr or stdout or "").lower():
            return False, "internal error"
        return True, ""
    if expect == "no-timeout":
        return True, ""
    if exit_code < 0:
        return False, SIG_MAP.get(exit_code, f"signal{-exit_code}")
    if exit_code != 0:
        return False, f"exit {exit_code}"
    if expect and expect not in (stdout or ""):
        got = (stdout or "").strip()
        if len(got) > 80:
            got = got[:80] + "..."
        return False, f"expected {expect!r}, got {got!r}"
    return True, ""


@dataclass(frozen=True)
class SnippetSpec:
    """In-memory snippet (integ JSON / p0 fixture cases)."""

    name: str
    code: str
    extra_args: tuple[str, ...] = ()
    expect_out: str = ""
    expect_err: str = ""
    expect_status: int | None = 0
    accept_status: tuple[int, ...] = ()
    timeout_s: float = 30
    stdin_suffix: str = "\n"
    stdout_last_line: bool = False
    err_regex: bool = False
    exact_out: bool = False
    type_line: bool = False  # typecheck: a "type:" line contains expect_out


def judge_snippet(
    exit_code: int,
    stdout: str,
    stderr: str,
    spec: SnippetSpec,
    *,
    timed_out: bool = False,
) -> tuple[bool, str]:
    """Judge an integ/p0 snippet. Returns (ok, detail)."""
    if timed_out:
        return False, "timeout"
    issues: list[str] = []
    if spec.expect_status is not None:
        allowed = (spec.expect_status,) + spec.accept_status
        if exit_code not in allowed:
            issues.append(f"exit_code={exit_code} (expected {spec.expect_status})")
    view = stdout.split("\n")[-1] if spec.stdout_last_line else stdout
    if spec.type_line:
        hit = any(line.startswith("type:") and spec.expect_out in line for line in stdout.split("\n"))
        if not hit:
            issues.append(f"expected type:{spec.expect_out!r} in stdout")
    elif spec.expect_out.startswith(">="):
        try:
            threshold = int(spec.expect_out[2:].strip())
            tokens = view.strip().split()
            if not tokens:
                issues.append(f"expected value>={threshold}, got empty stdout")
            else:
                val = int(tokens[-1])
                if val < threshold:
                    issues.append(f"expected value>={threshold}, got {view[:80]!r}")
        except ValueError:
            if spec.expect_out not in view:
                issues.append(f"expected {spec.expect_out!r} in stdout")
    elif spec.expect_out:
        if spec.exact_out:
            if view != spec.expect_out:
                issues.append(f"expected exact {spec.expect_out!r}, got {view[:80]!r}")
        elif spec.expect_out not in view:
            issues.append(f"expected {spec.expect_out!r} in stdout")
    if spec.expect_err:
        if spec.err_regex:
            if not re.search(spec.expect_err, stderr):
                issues.append(f"expected err~/{spec.expect_err}/")
        else:
            combined = stdout + "\n" + stderr
            if spec.expect_err not in combined:
                issues.append(f"expected error {spec.expect_err!r}")
    return (not issues, "; ".join(issues))


def run_snippet_suite(
    label: str,
    specs: Sequence[SnippetSpec],
    *,
    aura_bin: str | Path,
    env: dict[str, str],
    jobs: int = 1,
    skipped: Sequence[tuple[str, str]] = (),
) -> int:
    """Run in-memory snippets (stdin + extra argv). jobs=1 keeps /tmp-sharing cases serial."""
    aura_bin = str(aura_bin)
    for name, reason in skipped:
        warn(f"{name}: SKIPPED — {reason}")

    def run_one(spec: SnippetSpec) -> tuple[str, bool, str]:
        try:
            proc = subprocess.run(
                [aura_bin, *spec.extra_args],
                input=spec.code + spec.stdin_suffix,
                capture_output=True,
                text=True,
                timeout=spec.timeout_s,
                env=env,
            )
        except subprocess.TimeoutExpired:
            return spec.name, False, f"timeout {spec.timeout_s:g}s"
        stdout = (proc.stdout or "").strip()
        stderr = (proc.stderr or "").strip()
        ok_case, detail = judge_snippet(proc.returncode, stdout, stderr, spec)
        return spec.name, ok_case, detail

    work = list(specs)
    results: list[tuple[str, bool, str]]
    if jobs <= 1 or len(work) <= 1:
        results = [run_one(s) for s in work]
    else:
        print(f"  {label} parallel jobs={jobs} cases={len(work)}")
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {pool.submit(run_one, s): s for s in work}
            collected: dict[str, tuple[bool, str]] = {}
            for fut in as_completed(futs):
                name, ok_case, err = fut.result()
                collected[name] = (ok_case, err)
        results = [(n, collected[n][0], collected[n][1]) for n in sorted(collected)]

    passed = failed = 0
    for name, ok_case, err in results:
        if ok_case:
            ok(name)
            passed += 1
        else:
            fail(f"{name}: {err}" if err else name)
            failed += 1
    skipped_n = len(skipped)
    total = passed + failed + skipped_n
    summary = f"  {label}: {passed}/{total} passed"
    if skipped_n:
        summary += f" ({skipped_n} skipped)"
    if jobs > 1:
        summary += f" [jobs={jobs}]"
    print(summary)
    return 1 if failed > 0 else 0


@dataclass(frozen=True)
class CommandSpec:
    """Shell command case (smoke fixtures)."""

    name: str
    command: str
    expect: str
    timeout_s: float = 30
    cwd: str | Path | None = None


def run_command_suite(
    label: str,
    specs: Sequence[CommandSpec],
    *,
    env: dict[str, str],
    jobs: int = 1,
) -> int:
    """Run bash -c cases; pass if ``expect`` is in stdout+stderr."""

    def run_one(spec: CommandSpec) -> tuple[str, bool, str]:
        try:
            proc = subprocess.run(
                ["bash", "-c", spec.command],
                capture_output=True,
                text=True,
                timeout=spec.timeout_s,
                env=env,
                cwd=None if spec.cwd is None else str(spec.cwd),
            )
        except subprocess.TimeoutExpired:
            return spec.name, False, f"timeout {spec.timeout_s:g}s"
        combined = (proc.stdout or "") + (proc.stderr or "")
        if spec.expect in combined:
            return spec.name, True, ""
        got = combined.strip()[:60]
        return spec.name, False, f"expected {spec.expect!r}, got {got!r}"

    work = list(specs)
    results: list[tuple[str, bool, str]]
    if jobs <= 1 or len(work) <= 1:
        results = [run_one(s) for s in work]
    else:
        print(f"  {label} parallel jobs={jobs} cases={len(work)}")
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {pool.submit(run_one, s): s for s in work}
            collected: dict[str, tuple[bool, str]] = {}
            for fut in as_completed(futs):
                name, ok_case, err = fut.result()
                collected[name] = (ok_case, err)
        results = [(n, collected[n][0], collected[n][1]) for n in sorted(collected)]

    passed = failed = 0
    for name, ok_case, err in results:
        if ok_case:
            ok(name)
            passed += 1
        else:
            fail(f"{name}: {err}" if err else name)
            failed += 1
    print(f"  {label}: {passed}/{passed + failed} passed")
    return 1 if failed > 0 else 0


def discover_aura_files(
    root: Path,
    *,
    skip: Mapping[str, str] | None = None,
    exclude: set[str] | None = None,
    allow: set[str] | None = None,
) -> DiscoverResult:
    skip = skip or {}
    exclude = exclude or set()
    cases: list[AuraFileCase] = []
    skipped: list[tuple[str, str]] = []
    if not root.is_dir():
        return DiscoverResult(cases, skipped)
    rel_root = root.name
    for path in sorted(root.glob("*.aura")):
        name = path.name
        if name in exclude:
            continue
        if name in skip:
            skipped.append((f"{rel_root}/{name}", skip[name]))
            continue
        if allow is not None and name not in allow:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        cases.append(AuraFileCase(path=path, expect=first_expect(text), rel=f"{rel_root}/{name}"))
    return DiscoverResult(cases, skipped)


def run_aura_file_suite(
    label: str,
    discovered: DiscoverResult,
    *,
    aura_bin: str | Path,
    env: dict[str, str],
    mode: str,
    timeout_s: float,
    jobs: int = 1,
) -> int:
    """Run discovered cases. mode is ``load`` (--load path) or ``stdin``."""
    if mode not in {"load", "stdin"}:
        raise ValueError(f"mode must be load|stdin, got {mode!r}")
    aura_bin = str(aura_bin)

    for rel, reason in discovered.skipped:
        warn(f"{rel}: SKIPPED — {reason}")

    def run_one(case: AuraFileCase) -> tuple[str, bool, str]:
        try:
            text = case.path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            return case.rel, False, str(exc)[:80]
        if not text.strip():
            return case.rel, False, "empty"
        try:
            if mode == "load":
                proc = invoke_aura_load(case.path, aura_bin=aura_bin, env=env, timeout_s=timeout_s)
            else:
                proc = subprocess.run(
                    [aura_bin],
                    input=stdin_payload(text),
                    capture_output=True,
                    text=True,
                    timeout=timeout_s,
                    env=env,
                )
        except subprocess.TimeoutExpired:
            return case.rel, False, f"timeout {timeout_s:g}s"
        ok_case, detail = judge(proc.returncode, proc.stdout or "", proc.stderr or "", case.expect)
        if ok_case:
            return case.rel, True, ""
        if not detail:
            detail = ((proc.stderr or proc.stdout or "")[:80]) or "failed"
        elif proc.returncode != 0 and "exit" in detail:
            tail = (proc.stderr or proc.stdout or "").strip()[:80]
            if tail:
                detail = f"{detail}: {tail}"
        return case.rel, False, detail

    work = discovered.cases
    results: list[tuple[str, bool, str]]
    if jobs <= 1 or len(work) <= 1:
        results = [run_one(c) for c in work]
    else:
        print(f"  {label} parallel jobs={jobs} cases={len(work)}")
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {pool.submit(run_one, c): c for c in work}
            collected: dict[str, tuple[bool, str]] = {}
            for fut in as_completed(futs):
                rel, ok_case, err = fut.result()
                collected[rel] = (ok_case, err)
        results = [(rel, collected[rel][0], collected[rel][1]) for rel in sorted(collected)]

    passed = failed = 0
    for rel, ok_case, err in results:
        if ok_case:
            ok(rel)
            passed += 1
        else:
            fail(f"{rel}: {err}")
            failed += 1

    skipped_n = len(discovered.skipped)
    total = passed + failed + skipped_n
    summary = f"  {label}: {passed}/{total} passed"
    if skipped_n:
        summary += f" ({skipped_n} skipped)"
    if jobs > 1:
        summary += f" [jobs={jobs}]"
    print(summary)
    return 1 if failed > 0 else 0
