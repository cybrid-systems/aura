"""Resolve which test_issue_* targets to build/run for fast vs full CI."""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = _SCRIPT_DIR.parent
FAST_FIXTURE = _SCRIPT_DIR / "fixtures" / "issues_fast.json"
ISSUE_CPP_RE = re.compile(r"^tests/(test_issue_[\w]+)\.cpp$")


def issues_tier() -> str:
    """Return 'fast' or 'full' (default full for local dev)."""
    tier = os.environ.get("AURA_ISSUES_TIER", "full").strip().lower()
    return tier if tier in {"fast", "full"} else "full"


def load_fast_targets() -> list[str]:
    data = json.loads(FAST_FIXTURE.read_text(encoding="utf-8"))
    targets = data.get("targets", [])
    if not isinstance(targets, list) or not targets:
        raise ValueError(f"{FAST_FIXTURE}: 'targets' must be a non-empty array")
    return sorted({str(t) for t in targets})


def _git_diff_names(root: Path) -> list[str]:
    base = os.environ.get("AURA_GIT_BASE", "").strip()
    commands: list[list[str]] = []
    if base:
        commands.append(["git", "diff", "--name-only", f"{base}...HEAD"])
    commands.append(["git", "diff", "--name-only", "HEAD~1"])
    commands.append(["git", "diff", "--name-only", "--cached"])

    for cmd in commands:
        try:
            r = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                cwd=root,
                timeout=30,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0 and r.stdout.strip():
            return [line.strip() for line in r.stdout.splitlines() if line.strip()]
    return []


def git_changed_issue_targets(root: Path | None = None) -> list[str]:
    """Map touched tests/test_issue_*.cpp files to ninja target names."""
    root = root or ROOT
    names = _git_diff_names(root)
    targets: set[str] = set()
    for name in names:
        m = ISSUE_CPP_RE.match(name.replace("\\", "/"))
        if m:
            targets.add(m.group(1))
    return sorted(targets)


def resolve_issue_targets(tier: str | None = None) -> list[str]:
    """Targets to build/run. Full tier returns empty list (= use aggregate)."""
    tier = tier or issues_tier()
    if tier == "full":
        return []
    targets = set(load_fast_targets())
    targets.update(git_changed_issue_targets())
    return sorted(targets)
