#!/usr/bin/env python3
"""Declarative issue-coverage runner (architecture Phase 1).

Replaces per-issue hand-written check_*.py boilerplate with JSON manifests
under scripts/coverage/manifests/<issue>.json.

Usage:
  python3 scripts/coverage/runner.py --issue 2622
  python3 scripts/coverage/runner.py --all
  python3 scripts/coverage/runner.py --list

Manifest schema (minimal):
  {
    "issue": 2622,
    "title": "short title",
    "ok": "OK: ...",                 # optional success line
    "checks": [
      {
        "ac": "AC1",
        "path": "src/foo.cpp",       # single file
        "contains": ["needle", ...]  # all must appear
      },
      {
        "ac": "AC1",
        "paths": ["a.cpp", "b.h"],   # concatenated haystack
        "contains": ["x"]
      },
      {
        "ac": "AC5",
        "path": "src/foo.cpp",
        "contains_any": ["a", "b"]   # at least one
      },
      {
        "ac": "AC6",
        "forbid": ["docs/design/foo.md"]  # must not exist
      }
    ]
  }

Policy: new issues should add a manifest here; do not add a new check_*.py
unless the contract needs custom Python logic (ordering graphs, subprocess, …).
Legacy check_*.py may thin-wrap this runner during migration.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_DIR = Path(__file__).resolve().parent / "manifests"


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
            # Empty file / missing — report if needles expected
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
    ok = data.get("ok") or f"OK: Issue #{data['issue']} {data.get('title', '')} — all AC rows satisfied"
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


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--issue", type=int, help="Run one issue manifest")
    g.add_argument("--all", action="store_true", help="Run all manifests under manifests/")
    g.add_argument("--list", action="store_true", help="List available issue ids")
    args = ap.parse_args(argv)

    if args.list:
        ids = list_manifests()
        print("coverage manifests:", ", ".join(str(i) for i in ids) if ids else "(none)")
        return 0

    if args.issue is not None:
        return run_issue(args.issue)

    # --all
    ids = list_manifests()
    if not ids:
        print("FAIL: no manifests in", MANIFEST_DIR, file=sys.stderr)
        return 1
    rc = 0
    for i in ids:
        print(f"── coverage manifest #{i} ──")
        r = run_issue(i)
        if r != 0:
            rc = r
    if rc == 0:
        print(f"OK: all {len(ids)} coverage manifest(s) clean")
    return rc


if __name__ == "__main__":
    sys.exit(main())
