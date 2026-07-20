"""Shared helpers for Aura Python test/build scripts (#1961).

Used by tests/run.py (primary CLI), run_issue_tests.py, fixture_check,
build.py-adjacent runners, and gate scripts.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

# Issue #1932: this file lives under tests/python/ (was tests/).
TESTS = Path(__file__).resolve().parent.parent  # .../tests
ROOT = TESTS.parent
PYTHON = TESTS / "python"
BENCH = TESTS / "bench"
FUZZ = TESTS / "fuzz"
MEMORY = TESTS / "memory"
BUILD = ROOT / "build"
AURA_BIN = BUILD / "aura"

G = "\033[32m"
Y = "\033[33m"
R = "\033[31m"
B = "\033[34m"
C = "\033[36m"
N = "\033[0m"


def ok(msg: str) -> None:
    print(f"  {G}✓{N} {msg}")


def fail(msg: str) -> None:
    print(f"  {R}✗{N} {msg}")


def warn(msg: str) -> None:
    print(f"  {Y}!{N} {msg}")


def info(msg: str) -> None:
    print(f"  {B}→{N} {msg}")


def run(cmd: Sequence[str] | str, **kwargs: Any) -> int:
    return subprocess.run(cmd, **kwargs).returncode


def ensure_tests_on_path() -> None:
    """Allow sibling imports from tests/python/ (issue_tier, fixture_store, …)."""
    py = str(PYTHON)
    if py not in sys.path:
        sys.path.insert(0, py)
    # Also keep tests/ on path for any residual top-level helpers.
    tests = str(TESTS)
    if tests not in sys.path:
        sys.path.insert(0, tests)


@dataclass
class RunReport:
    """Uniform CI-friendly summary for a runner category."""

    category: str
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    pre_existing: int = 0
    elapsed_s: float = 0.0
    exit_code: int = 0
    extra: dict[str, Any] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.exit_code == 0 and self.failed == 0

    def print_human(self) -> None:
        status = f"{G}PASS{N}" if self.ok else f"{R}FAIL{N}"
        print(f"\n{B}── {self.category} ──{N} {status}")
        print(
            f"  passed={self.passed} failed={self.failed} "
            f"skipped={self.skipped} pre_existing={self.pre_existing} "
            f"time={self.elapsed_s:.1f}s"
        )
        if self.extra:
            for k, v in self.extra.items():
                print(f"  {k}={v}")

    def to_json(self) -> dict[str, Any]:
        d = asdict(self)
        d["ok"] = self.ok
        return d


def print_json_report(report: RunReport) -> None:
    print(json.dumps(report.to_json(), indent=2, sort_keys=True))


def timed_call(fn: Callable[[], int], *, category: str) -> RunReport:
    """Run fn() → exit code, wrap as RunReport with elapsed time."""
    t0 = time.time()
    code = int(fn())
    elapsed = time.time() - t0
    return RunReport(
        category=category,
        passed=1 if code == 0 else 0,
        failed=0 if code == 0 else 1,
        elapsed_s=round(elapsed, 3),
        exit_code=code,
    )


# Categories exposed by tests/run.py (and build.py suite names where shared).
CATEGORIES: dict[str, str] = {
    "issues": "C++ issue/domain/bundle binaries (tests/python/run_issue_tests.py)",
    "issues-fast": "Same as issues with AURA_ISSUES_TIER=fast",
    "fixtures": "Validate tests/fixtures/*.json",
    "gradual": "Gradual typing guarantee scenarios",
    "bench": "Compiler pipeline benchmark SLO gate (tests/bench/)",
    "mutation": "EDSL mutation_loop self-evolution",
    "bash": "Legacy tests/python/run-tests.sh smoke",
    "e2e": "commercial_readiness .aura E2E + golden PASS labels (#1934)",
    "list": "List categories (this help)",
}
