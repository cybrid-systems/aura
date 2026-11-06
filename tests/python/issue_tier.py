"""Resolve which issue test targets to build/run for fast vs full CI.

Issue #1484 sub-task 4 audit (2026-07-16): AURA_ISSUES_TIER=fast
covers the domain batch suites plus the enabled link-profile bundles —
sufficient for catching broken-include regressions in PR CI. The list is
checked against build/build.ninja at resolve time, so a target that CMake
disabled no longer turns the whole shard build into "unknown target".
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
import sys
from pathlib import Path

# Issue #1932: lives under tests/python/
# parents[0]=tests/python's parent=tests, parents[1]=repo root
_SCRIPT_DIR = Path(__file__).resolve().parent  # tests/python
ROOT = _SCRIPT_DIR.parents[1]
_TESTS = _SCRIPT_DIR.parent  # tests/
FAST_FIXTURE = _TESTS / "fixtures" / "issues_fast.json"
PROFILES_FIXTURE = _TESTS / "fixtures" / "issue_link_profiles.json"
# Match tests/<any-subdir>/test_*.cpp (R1 src/-aligned).
ISSUE_CPP_RE = re.compile(r"^tests/(?:[\w]+/)?(test_[\w]+)\.cpp$")

# Profiles CMake actually calls aura_add_issue_bundle() for. jit_late2/3
# are disabled there (bundle mains reference undefined entry functions);
# listing them here made a git-changed member resolve to a dead target.
BUNDLE_PROFILES = (
    "light",
    "jit_late1",
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


_NINJA_TARGETS: set[str] | None | bool = False


def _ninja_target_names() -> set[str] | None:
    """Targets defined by build/build.ninja, or None when there is no build."""
    global _NINJA_TARGETS
    if _NINJA_TARGETS is not False:
        return _NINJA_TARGETS  # type: ignore[return-value]
    text: str | None = None
    try:
        text = (ROOT / "build" / "build.ninja").read_text(errors="replace")
    except OSError:
        text = None
    _NINJA_TARGETS = set(re.findall(r"^build ([^:\s]+):", text, re.M)) if text else None
    return _NINJA_TARGETS  # type: ignore[return-value]


def resolve_issue_targets(tier: str | None = None) -> list[str]:
    """Targets to build/run. Full tier returns empty list (= use aggregate)."""
    tier = tier or issues_tier()
    if tier == "full":
        return []
    targets = set(load_fast_targets())
    targets.update(git_changed_issue_targets())
    live = _ninja_target_names()
    if live is not None:
        unknown = sorted(t for t in targets if t not in live)
        if unknown:
            print(
                f"warning: fast issue tier names target(s) not in build/build.ninja; ignoring: {', '.join(unknown)}",
                file=sys.stderr,
            )
            targets.difference_update(unknown)
    return sorted(targets)
