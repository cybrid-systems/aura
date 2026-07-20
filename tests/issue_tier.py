"""Resolve which issue test targets to build/run for fast vs full CI.

Issue #1484 sub-task 4 audit (2026-07-16): AURA_ISSUES_TIER=fast
covers 23 targets (10 bundles + 5 test_issue_* + 8 other) —
sufficient for catching broken-include regressions in PR CI.
The 9 broken files fixed at commit 313c530d were in low-tier
tests (per issue body), and the new test_check_test_includes.sh
shell test runs upstream of all tiers (pre-commit hook +
./build.py lint pass), so the linter catches broken-include
regressions regardless of which tier the affected test is in.
Net: no fast-tier expansion needed for #1484.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = _SCRIPT_DIR.parent
FAST_FIXTURE = _SCRIPT_DIR / "fixtures" / "issues_fast.json"
PROFILES_FIXTURE = _SCRIPT_DIR / "fixtures" / "issue_link_profiles.json"
# Match tests/test_*.cpp, tests/domain/test_*.cpp, tests/domain/<theme>/test_*.cpp.
ISSUE_CPP_RE = re.compile(r"^tests/(?:domain/(?:[\w]+/)?)?(test_[\w]+)\.cpp$")

BUNDLE_PROFILES = (
    "light",
    "jit",
    "jit_minimal",
    "jit_contract",
    "jit_tests",
    "fiber",
    "jit_late1",
    "jit_late2",
    "jit_late3",
    "jit_late4",
    "jit_late5",
    "light_late",
)


def issues_tier() -> str:
    """Return 'fast' or 'full' (default full for local dev)."""
    tier = os.environ.get("AURA_ISSUES_TIER", "full").strip().lower()
    return tier if tier in {"fast", "full"} else "full"


def _member_to_bundle() -> dict[str, str]:
    data = json.loads(PROFILES_FIXTURE.read_text(encoding="utf-8"))
    out: dict[str, str] = {}
    for profile in BUNDLE_PROFILES:
        for member in data.get(profile, []):
            out[member] = f"test_issues_{profile}"
    return out


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
    """Map touched issue test sources to ninja targets (bundles or standalone)."""
    root = root or ROOT
    member_bundle = _member_to_bundle()
    names = _git_diff_names(root)
    targets: set[str] = set()
    for name in names:
        m = ISSUE_CPP_RE.match(name.replace("\\", "/"))
        if not m:
            continue
        stem = m.group(1)
        if stem in member_bundle:
            targets.add(member_bundle[stem])
        else:
            targets.add(stem)
    return sorted(targets)


def resolve_issue_targets(tier: str | None = None) -> list[str]:
    """Targets to build/run. Full tier returns empty list (= use aggregate)."""
    tier = tier or issues_tier()
    if tier == "full":
        return []
    targets = set(load_fast_targets())
    targets.update(git_changed_issue_targets())
    return sorted(targets)
