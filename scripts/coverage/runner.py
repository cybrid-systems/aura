#!/usr/bin/env python3
"""Declarative issue-coverage runner (architecture Phase 1 + Phase 2).

Phase 1: JSON manifests under scripts/coverage/manifests/<issue>.json
Phase 2: --changed selects only manifests whose declared paths intersect
         the git diff against --base (default: origin/main...HEAD, with
         staged/unstaged worktree files included).

Usage:
  python3 scripts/coverage/runner.py --issue 2622
  python3 scripts/coverage/runner.py --all
  python3 scripts/coverage/runner.py --changed
  python3 scripts/coverage/runner.py --changed --base origin/main
  python3 scripts/coverage/runner.py --list
  python3 scripts/coverage/runner.py --index   # path → issues reverse map

Always-run safety net: if the diff touches the runner, manifests dir,
build.py gate wiring, or CMakeLists.txt, --changed expands to --all.

Policy: new issues add a manifest; reserve check_*.py for custom logic only.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_DIR = Path(__file__).resolve().parent / "manifests"

# Diffing these paths forces a full manifest run under --changed.
ALWAYS_ALL_PREFIXES = (
    "scripts/coverage/",
    "build.py",
    "CMakeLists.txt",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _haystack(check: dict) -> tuple[str, str]:
    """Return (label, text) for a check row."""
    if "path" in check:
        rel = check["path"]
        return rel, _read(rel)
    if "paths" in check:
        paths = check["paths"]
        label = "+".join(paths)
        return label, "".join(_read(p) for p in paths)
    return "", ""


def run_manifest(data: dict) -> list[str]:
    fails: list[str] = []
    issue = data.get("issue", "?")
    for i, check in enumerate(data.get("checks", [])):
        ac = check.get("ac", f"row{i}")
        prefix = f"#{issue} {ac}"

        if "forbid" in check:
            for rel in check["forbid"]:
                if (ROOT / rel).is_file():
                    fails.append(f"{prefix}: unexpected design/doc file {rel!r}")
            continue

        label, text = _haystack(check)
        if ("path" in check or "paths" in check) and not text and ("contains" in check or "contains_any" in check):
            fails.append(f"{prefix}: empty or missing source {label!r}")
            continue

        for needle in check.get("contains", []):
            if needle not in text:
                fails.append(f"{prefix}: missing {needle!r} in {label}")

        any_needles = check.get("contains_any", [])
        if any_needles and not any(n in text for n in any_needles):
            fails.append(f"{prefix}: none of {any_needles!r} in {label}")

    return fails


def load_manifest(issue: int | str) -> dict:
    path = MANIFEST_DIR / f"{issue}.json"
    if not path.is_file():
        raise FileNotFoundError(f"manifest not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if int(data.get("issue", -1)) != int(issue):
        raise ValueError(f"manifest issue field mismatch: {path}")
    return data


def run_issue(issue: int | str) -> int:
    try:
        data = load_manifest(issue)
    except (FileNotFoundError, ValueError, json.JSONDecodeError) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    fails = run_manifest(data)
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    ok = data.get("ok") or (f"OK: Issue #{data['issue']} {data.get('title', '')} — all AC rows satisfied")
    print(ok)
    return 0


def list_manifests() -> list[int]:
    out: list[int] = []
    if not MANIFEST_DIR.is_dir():
        return out
    for p in sorted(MANIFEST_DIR.glob("*.json")):
        try:
            out.append(int(p.stem))
        except ValueError:
            continue
    return out


def manifest_paths(data: dict) -> set[str]:
    """All repo-relative paths declared by a manifest (contains + forbid)."""
    paths: set[str] = set()
    for check in data.get("checks", []):
        if "path" in check:
            paths.add(check["path"])
        for p in check.get("paths", []) or []:
            paths.add(p)
        for p in check.get("forbid", []) or []:
            paths.add(p)
    return paths


def build_path_index() -> dict[str, set[int]]:
    """Reverse map: path → set of issue ids that declare it."""
    index: dict[str, set[int]] = defaultdict(set)
    for issue in list_manifests():
        try:
            data = load_manifest(issue)
        except (FileNotFoundError, ValueError, json.JSONDecodeError):
            continue
        for p in manifest_paths(data):
            index[p].add(int(issue))
    return index


def _git_lines(args: list[str]) -> list[str]:
    try:
        r = subprocess.run(
            ["git", *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return []
    if r.returncode != 0:
        return []
    return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]


def changed_files(base: str) -> tuple[list[str], str | None]:
    """Return (changed paths, error_or_note).

    Collects:
      1) merge-base(base, HEAD)...HEAD
      2) staged (cached)
      3) unstaged worktree
    """
    # Resolve base; fall back to HEAD~1 / empty
    rev_list = _git_lines(["rev-parse", "--verify", base])
    if not rev_list:
        # No origin/main etc. — use empty tree vs HEAD for "all tracked at tip"
        note = f"base {base!r} unavailable; using staged+unstaged only"
        files: set[str] = set()
    else:
        note = None
        mb = _git_lines(["merge-base", base, "HEAD"])
        range_a = mb[0] if mb else base
        files = set(_git_lines(["diff", "--name-only", f"{range_a}...HEAD"]))

    files.update(_git_lines(["diff", "--name-only", "--cached"]))
    files.update(_git_lines(["diff", "--name-only"]))
    # Untracked files under src/scripts/tests can also affect contracts
    untracked = _git_lines(["ls-files", "--others", "--exclude-standard"])
    files.update(untracked)
    return sorted(files), note


def forces_all(changed: list[str]) -> bool:
    for f in changed:
        for pref in ALWAYS_ALL_PREFIXES:
            if f == pref.rstrip("/") or f.startswith(pref):
                return True
    return False


def select_issues_for_changed(changed: list[str]) -> tuple[list[int], str]:
    """Return (issue ids sorted, reason string)."""
    if forces_all(changed):
        return list_manifests(), "diff touches runner/manifests/build.py/CMakeLists → --all"

    index = build_path_index()
    selected: set[int] = set()
    for f in changed:
        # Exact path match
        if f in index:
            selected |= index[f]
            continue
        # Prefix: changed path under a declared directory (rare)
        for declared, issues in index.items():
            if f.startswith(declared.rstrip("/") + "/") or declared.startswith(f.rstrip("/") + "/"):
                selected |= issues

    return sorted(selected), f"{len(changed)} changed file(s) → {len(selected)} related issue(s)"


def run_many(ids: list[int], label: str) -> int:
    if not ids:
        print(f"OK: {label} — no related coverage manifests (skip)")
        return 0
    rc = 0
    for i in ids:
        print(f"── coverage manifest #{i} ──")
        r = run_issue(i)
        if r != 0:
            rc = r
    if rc == 0:
        print(f"OK: {label} — {len(ids)} coverage manifest(s) clean")
    return rc


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--issue", type=int, help="Run one issue manifest")
    g.add_argument("--all", action="store_true", help="Run all manifests")
    g.add_argument(
        "--changed",
        action="store_true",
        help="Run only manifests whose paths intersect the git diff (Phase 2)",
    )
    g.add_argument("--list", action="store_true", help="List available issue ids")
    g.add_argument(
        "--index",
        action="store_true",
        help="Print path → issues reverse index and exit",
    )
    ap.add_argument(
        "--base",
        default="origin/main",
        help="Git ref for --changed merge-base (default: origin/main)",
    )
    args = ap.parse_args(argv)

    if args.list:
        ids = list_manifests()
        print("coverage manifests:", ", ".join(str(i) for i in ids) if ids else "(none)")
        return 0

    if args.index:
        index = build_path_index()
        for path in sorted(index):
            issues = ",".join(str(i) for i in sorted(index[path]))
            print(f"{path}\t{issues}")
        print(f"# {len(index)} path(s), {len(list_manifests())} manifest(s)")
        return 0

    if args.issue is not None:
        return run_issue(args.issue)

    if args.changed:
        changed, note = changed_files(args.base)
        if note:
            print(f"note: {note}", file=sys.stderr)
        if not changed:
            print("OK: --changed — empty diff (skip all manifests)")
            return 0
        print(f"--changed base={args.base!r} files={len(changed)}")
        ids, reason = select_issues_for_changed(changed)
        print(f"  select: {reason}")
        if ids:
            print(f"  issues: {', '.join(f'#{i}' for i in ids)}")
        return run_many(ids, "--changed")

    # --all
    ids = list_manifests()
    if not ids:
        print("FAIL: no manifests in", MANIFEST_DIR, file=sys.stderr)
        return 1
    return run_many(ids, "--all")


if __name__ == "__main__":
    sys.exit(main())
