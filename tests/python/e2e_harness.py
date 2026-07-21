"""e2e_harness.py — Issue #1934 strengthened .aura E2E assertions.

Reusable check_* helpers for commercial_readiness / suite e2e:

  - run_aura_file(path) → AuraRunResult
  - check_e2e_pass(result) — requires E2E-PASS and zero FAIL: lines
  - check_pass_labels(result, expected_labels)
  - check_golden(result, golden_path=None, suite_name=None) — PASS labels match consolidated golden JSON
    (suite_name auto-derived from result.path.stem if omitted)
    Default golden_path: tests/fixtures/e2e_golden/all.json (single consolidated file)
  - assert helpers raise E2EAssertionError with actual vs expected context
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from _aura_harness import AURA_BIN, ROOT, TESTS

E2E_ROOT = TESTS / "e2e"
COMMERCIAL = E2E_ROOT / "commercial_readiness"
GOLDEN_DIR = TESTS / "fixtures" / "e2e_golden"

PASS_RE = re.compile(r"^PASS:\s*(.+)$", re.M)
FAIL_RE = re.compile(r"^FAIL:\s*(.+)$", re.M)
SUMMARY_RE = re.compile(
    r"""(\d+)\s+suites:\s+(\d+)\s+passed,\s+(\d+)\s+failed""",
    re.I,
)


class E2EAssertionError(AssertionError):
    """Raised when an e2e check fails; message includes actual vs expected."""


@dataclass
class AuraRunResult:
    path: Path
    exit_code: int
    stdout: str
    stderr: str
    elapsed_s: float
    pass_labels: list[str] = field(default_factory=list)
    fail_labels: list[str] = field(default_factory=list)
    suites_passed: int | None = None
    suites_failed: int | None = None
    has_e2e_pass: bool = False
    has_e2e_fail: bool = False
    crashed: bool = False

    @property
    def combined(self) -> str:
        return (self.stdout or "") + "\n" + (self.stderr or "")


def run_aura_file(
    path: Path | str,
    *,
    aura_bin: Path | None = None,
    timeout: float = 60.0,
    env: dict[str, str] | None = None,
) -> AuraRunResult:
    """Load a .aura file via `aura --load` and capture stdout/stderr."""
    path = Path(path)
    bin_path = Path(aura_bin or AURA_BIN)
    if not bin_path.is_file():
        raise FileNotFoundError(f"aura binary missing: {bin_path}")
    if not path.is_file():
        raise FileNotFoundError(f"e2e script missing: {path}")

    run_env = {**os.environ, **(env or {})}
    t0 = time.time()
    try:
        proc = subprocess.run(
            [str(bin_path), "--load", str(path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=str(ROOT),
            env=run_env,
        )
        elapsed = time.time() - t0
        # strip NULs from crash dumps
        out = (proc.stdout or "").replace("\x00", "")
        err = (proc.stderr or "").replace("\x00", "")
        combined = out + "\n" + err
        return AuraRunResult(
            path=path,
            exit_code=int(proc.returncode),
            stdout=out,
            stderr=err,
            elapsed_s=round(elapsed, 3),
            pass_labels=[m.strip() for m in PASS_RE.findall(combined)],
            fail_labels=[m.strip() for m in FAIL_RE.findall(combined)],
            has_e2e_pass="E2E-PASS" in combined,
            has_e2e_fail="E2E-FAIL" in combined,
            crashed="AURA CRASH" in combined or "SIGSEGV" in combined,
            **_parse_suite_summary(combined),
        )
    except subprocess.TimeoutExpired as e:
        elapsed = time.time() - t0
        out = (e.stdout or "") if isinstance(e.stdout, str) else ""
        err = (e.stderr or "") if isinstance(e.stderr, str) else "timeout"
        return AuraRunResult(
            path=path,
            exit_code=124,
            stdout=out,
            stderr=err,
            elapsed_s=round(elapsed, 3),
            crashed=True,
        )


def _parse_suite_summary(text: str) -> dict[str, Any]:
    m = SUMMARY_RE.search(text)
    if not m:
        return {}
    return {
        "suites_passed": int(m.group(2)),
        "suites_failed": int(m.group(3)),
    }


def check_e2e_pass(result: AuraRunResult, *, min_passes: int = 1) -> None:
    """Require E2E-PASS marker, zero FAIL lines, no crash, min PASS count."""
    if result.crashed:
        raise E2EAssertionError(
            f"{result.path.name}: process crashed (SIGSEGV/timeout)\n  stderr tail: {result.stderr[-300:]!r}"
        )
    if result.fail_labels:
        raise E2EAssertionError(
            f"{result.path.name}: {len(result.fail_labels)} FAIL line(s)\n"
            f"  fails: {result.fail_labels[:8]}\n"
            f"  passes: {result.pass_labels[:8]}"
        )
    if result.has_e2e_fail:
        raise E2EAssertionError(f"{result.path.name}: E2E-FAIL marker present\n  stdout tail: {result.stdout[-300:]!r}")
    if not result.has_e2e_pass and result.suites_failed is None:
        # Allow std/test run-tests summary path without E2E-PASS
        if result.suites_failed is not None:
            pass
        else:
            raise E2EAssertionError(
                f"{result.path.name}: missing E2E-PASS marker "
                f"(got {len(result.pass_labels)} PASS lines)\n"
                f"  stdout tail: {result.stdout[-300:]!r}"
            )
    if result.suites_failed is not None and result.suites_failed > 0:
        raise E2EAssertionError(
            f"{result.path.name}: run-tests reported "
            f"{result.suites_failed} failed suite(s) "
            f"(passed={result.suites_passed})"
        )
    if len(result.pass_labels) < min_passes and (result.suites_passed is None or result.suites_passed < min_passes):
        raise E2EAssertionError(f"{result.path.name}: expected ≥{min_passes} PASS lines, got {len(result.pass_labels)}")


def check_pass_labels(result: AuraRunResult, expected: Sequence[str]) -> None:
    """Require exact ordered PASS labels (machine-checkable AC list)."""
    got = result.pass_labels
    exp = list(expected)
    if got != exp:
        raise E2EAssertionError(
            f"{result.path.name}: PASS labels mismatch\n  expected ({len(exp)}): {exp}\n  actual   ({len(got)}): {got}"
        )


def check_golden(
    result: AuraRunResult,
    golden_path: Path | str | None = None,
    *,
    suite_name: str | None = None,
) -> None:
    """Compare PASS labels against the consolidated e2e_golden/all.json.

    Default golden_path is tests/fixtures/e2e_golden/all.json. Default
    suite_name is `result.path.stem`. The file schema is:
        {"schema": <n>, "issue": <n>, "suites": {<stem>: {source, pass_labels, min_passes}}}
    """
    golden_path = Path(golden_path) if golden_path else GOLDEN_DIR / "all.json"
    if not golden_path.is_file():
        raise FileNotFoundError(f"golden missing: {golden_path} (run --update-golden)")
    data = json.loads(golden_path.read_text(encoding="utf-8"))
    suites = data.get("suites") or {}
    name = suite_name or result.path.stem
    if name not in suites:
        raise E2EAssertionError(f"{golden_path}: no entry for suite {name!r}")
    suite = suites[name]
    expected = suite.get("pass_labels") or suite.get("passes") or []
    if not isinstance(expected, list):
        raise E2EAssertionError(f"{golden_path}: suites[{name!r}].pass_labels must be a list")
    check_pass_labels(result, [str(x) for x in expected])
    min_p = int(suite.get("min_passes", 0))
    if min_p and len(result.pass_labels) < min_p:
        raise E2EAssertionError(
            f"{result.path.name}: golden suites[{name}] min_passes={min_p}, got {len(result.pass_labels)}"
        )


def write_golden(
    result: AuraRunResult,
    golden_path: Path | str | None = None,
    *,
    suite_name: str | None = None,
) -> None:
    """Update one suite's entry inside the consolidated e2e_golden/all.json.

    Default golden_path is tests/fixtures/e2e_golden/all.json. Default
    suite_name is `result.path.stem`. Reads existing file (if any), updates
    one suite entry, writes back.
    """
    golden_path = Path(golden_path) if golden_path else GOLDEN_DIR / "all.json"
    golden_path.parent.mkdir(parents=True, exist_ok=True)
    if golden_path.is_file():
        existing = json.loads(golden_path.read_text(encoding="utf-8"))
    else:
        existing = {"schema": 1934, "issue": 1934, "suites": {}}
    suites = existing.setdefault("suites", {})
    name = suite_name or result.path.stem
    suites[name] = {
        "source": str(result.path.relative_to(ROOT)) if result.path.is_relative_to(ROOT) else str(result.path),
        "pass_labels": result.pass_labels,
        "min_passes": len(result.pass_labels),
    }
    golden_path.write_text(json.dumps(existing, indent=2) + "\n", encoding="utf-8")


def discover_commercial_readiness() -> list[Path]:
    if not COMMERCIAL.is_dir():
        return []
    return sorted(
        p for p in COMMERCIAL.glob("commercial_readiness_*.aura") if p.is_file() and not p.name.startswith("_")
    )


def golden_for(script: Path) -> Path:
    """Return the consolidated e2e_golden path (single file holds all suites)."""
    return GOLDEN_DIR / "all.json"
