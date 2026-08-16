#!/usr/bin/env python3
"""Shared runner for file-based .aura leaves (suite + regression).

Two on-disk conventions, one judge:

  tests/suite/*.aura        --load file, default expect = exit 0
  tests/regression/*.aura   stdin (header ;; comments stripped), ``;; expect:``

Optional first ``;; expect:`` line:
  no-crash    fail only on fatal signal
  no-error    fail if stderr/stdout contains "internal error"
  no-timeout  success if the process returned (timeout is still a fail)
  <text>      exit 0 and substring present in stdout
  (absent)    exit 0
"""

from __future__ import annotations

import subprocess
from collections.abc import Mapping
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

from _aura_harness import fail, ok, warn

SIG_MAP = {-6: "SIGABRT", -8: "SIGFPE", -11: "SIGSEGV"}


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
                proc = subprocess.run(
                    [aura_bin, "--load", str(case.path)],
                    capture_output=True,
                    text=True,
                    timeout=timeout_s,
                    env=env,
                )
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
