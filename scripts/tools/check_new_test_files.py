#!/usr/bin/env python3
"""Reject newly added issue-numbered test files.

`python3 build.py gate` runs this on every gate, including ``--changed``.
The pre-commit hook runs the same classifier with ``--cached`` (staged adds
only). A missing ``core.hooksPath`` does not skip the gate.

Fail only paths git reports as added (``--diff-filter=A --no-renames``), plus
untracked non-ignored files in gate mode. Modifying or deleting a historical
numbered file passes. A rename that git shows as delete+add fails when the
new name is still banned.

Patterns (3–5 digit issue numbers; ``test_issue_*`` keeps the historical
C++ rule that any leading digit is enough):

  ^tests/(?:.+/)?test_issue_[0-9].*\\.cpp$
  ^tests/(?:.+/)?test_.+_[0-9]{3,5}\\.cpp$
  ^tests/test_[a-z].*\\.cpp$
  ^tests/(?:.+/)?test_issue_.*\\.py$
  ^tests/(?:.+/)?test_.+_[0-9]{3,5}\\.py$
  ^tests/suite/.+_[0-9]{3,5}\\.aura$
  ^tests/suite/issue_[0-9]{3,5}_.+\\.aura$
  ^tests/suite/issue_[0-9]{3,5}\\.aura$
  ^scripts/coverage/checks/check_.+_[0-9]{3,5}(?![0-9]).*\\.py$
  ^scripts/coverage/checks/check_[0-9]{3,5}_.+\\.py$
  ^scripts/coverage/checks/check_[0-9]{3,5}\\.py$

The last three close ``check_<digits>.py`` and the aura ``issue_<digits>.aura``
name when those globs would otherwise require an underscore after the digits.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from homes import CREATE_RULE, render_home  # noqa: E402

# Published set. is_banned() is "any pattern fullmatches".
BANNED_PATTERNS: tuple[str, ...] = (
    r"^tests/(?:.+/)?test_issue_[0-9].*\.cpp$",
    r"^tests/(?:.+/)?test_.+_[0-9]{3,5}\.cpp$",
    r"^tests/test_[a-z].*\.cpp$",
    r"^tests/(?:.+/)?test_issue_.*\.py$",
    r"^tests/(?:.+/)?test_.+_[0-9]{3,5}\.py$",
    r"^tests/suite/.+_[0-9]{3,5}\.aura$",
    r"^tests/suite/issue_[0-9]{3,5}_.+\.aura$",
    r"^tests/suite/issue_[0-9]{3,5}\.aura$",
    r"^scripts/coverage/checks/check_.+_[0-9]{3,5}(?![0-9]).*\.py$",
    r"^scripts/coverage/checks/check_[0-9]{3,5}_.+\.py$",
    r"^scripts/coverage/checks/check_[0-9]{3,5}\.py$",
)
_BANNED = tuple(re.compile(p) for p in BANNED_PATTERNS)
_ISSUE_DIGITS = re.compile(r"(?<![0-9])[0-9]{3,5}(?![0-9])")
_STEM_PREFIXES = ("test_issue_", "issue_", "check_")


def _norm(path: str) -> str:
    return path.replace("\\", "/").strip().lstrip("./")


def is_banned(path: str) -> bool:
    """True when a newly added path must be refused. Existing files may still match."""
    path = _norm(path)
    return any(rx.fullmatch(path) for rx in _BANNED)


def query_from_path(path: str) -> str:
    """Surface words left after dropping test_issue_/issue_/check_ and issue digits.

    An empty result means there is nothing to ask. Callers must not invent a path.
    """
    name = _norm(path).rsplit("/", 1)[-1]
    stem = name.rsplit(".", 1)[0]
    for prefix in _STEM_PREFIXES:
        if stem.startswith(prefix):
            stem = stem[len(prefix) :]
            break
    stem = _ISSUE_DIGITS.sub(" ", stem)
    stem = stem.replace("_", " ")
    return re.sub(r"\s+", " ", stem).strip(" -")


def _without_rule(text: str) -> str:
    rule = CREATE_RULE.strip("\n")
    body = text.strip("\n")
    if body.endswith(rule):
        body = body[: -len(rule)].rstrip("\n")
    return body


def format_refusal(paths: list[str]) -> str:
    """One create-rule, and per file the home hit or the home miss."""
    lines = [
        "new issue-numbered test file(s) blocked.",
        "Policy: tests/HOMES.md. python3 build.py gate enforces this.",
        "The pre-commit hook runs the same check on staged adds.",
        "Append a case to the home below, or create a theme file with no issue digits.",
        "Modifying or deleting a historical numbered file is allowed.",
        "Do not add a check script to satisfy this refusal.",
    ]
    for path in paths:
        query = query_from_path(path)
        asked = query if query else "<feature>"
        _code, text = render_home(asked)
        lines.append("")
        lines.append(path)
        lines.append(_without_rule(text))
        lines.append(f'python3 tests/run.py home "{asked}"')
    lines.append("")
    lines.append(CREATE_RULE.strip("\n"))
    lines.append("Genealogy: tests/HOMES.md")
    return "\n".join(lines) + "\n"


# Container jobs check out as one uid and run git as another; --global
# safe.directory is not set there. The refusals below stay strict, but the
# ownership check must not read as "no refs".
GIT = ["git", "-c", "safe.directory=*"]


def _git(args: list[str]) -> list[str] | None:
    try:
        proc = subprocess.run(
            [*GIT, *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return None
    if proc.returncode != 0:
        return None
    out: list[str] = []
    for line in proc.stdout.splitlines():
        line = _norm(line)
        if line:
            out.append(line)
    return out


def _ref_exists(ref: str) -> bool:
    try:
        proc = subprocess.run(
            [*GIT, "rev-parse", "--verify", "--quiet", ref],
            cwd=ROOT,
            capture_output=True,
            check=False,
        )
    except FileNotFoundError:
        return False
    return proc.returncode == 0


def _extend(found: list[str], lines: list[str] | None) -> None:
    for line in lines or []:
        if line not in found:
            found.append(line)


def collect_added(mode: str = "gate") -> list[str]:
    """Added paths. ``cached`` is staged adds. ``gate`` also sees the branch and untracked files.

    When ``origin/main`` is missing, fall back to ``main`` and then ``HEAD``.
    Never return success by skipping the diff.
    """
    found: list[str] = []
    add_args = ["--name-only", "--diff-filter=A", "--no-renames"]
    if mode == "cached":
        staged = _git(["diff", "--cached", *add_args])
        if staged is None:
            raise RuntimeError("git diff --cached failed; refusing to skip the new-file check")
        return staged

    base = next((ref for ref in ("origin/main", "main", "HEAD") if _ref_exists(ref)), None)
    if base is None:
        raise RuntimeError("git has no origin/main, main, or HEAD; refusing to skip the new-file check")
    committed = _git(["diff", *add_args, f"{base}...HEAD"])
    if committed is None:
        committed = _git(["diff", *add_args, base, "HEAD"])
    if committed is None:
        raise RuntimeError(f"git diff against {base} failed; refusing to skip the new-file check")
    staged = _git(["diff", "--cached", *add_args])
    untracked = _git(["ls-files", "--others", "--exclude-standard"])
    if staged is None or untracked is None:
        raise RuntimeError("git could not list staged or untracked files; refusing to skip the new-file check")
    _extend(found, committed)
    _extend(found, staged)
    _extend(found, untracked)
    return found


def banned_added(mode: str = "gate") -> list[str]:
    return [path for path in collect_added(mode) if is_banned(path)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cached",
        action="store_true",
        help="staged adds only (pre-commit). Gate mode also scans the branch and untracked files.",
    )
    args = parser.parse_args(argv)
    mode = "cached" if args.cached else "gate"
    try:
        banned = banned_added(mode)
    except RuntimeError as exc:
        print(f"new-test-file check failed open: {exc}", file=sys.stderr)
        return 1
    if not banned:
        return 0
    print(format_refusal(banned), file=sys.stderr, end="")
    return 1


if __name__ == "__main__":
    sys.exit(main())
