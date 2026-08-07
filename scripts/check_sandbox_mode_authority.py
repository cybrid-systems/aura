#!/usr/bin/env python3
"""Issue #2657: audit direct writes to CapabilityRegistry::sandbox_mode.

The process-wide authority for SandboxMode is
``aura::core::sandbox::set_mode`` (SSOT in sandbox.hh; sandbox.ixx
only re-exports). It atomically updates the atomic source of truth,
the plain enum mirror, the registry atomic, the workspace-isolation
strict link, and the provenance tracker policy. Any code that writes
the registry's ``sandbox_mode`` field directly via
``g_capability_registry().sandbox_mode = X`` (or any local
``reg.sandbox_mode = X``) bypasses the broadcast and silently
re-introduces the triple-state drift the authority was created to
close.

This linter is the second gate (the first is the compile-time
``friend aura::core::sandbox::set_mode`` declaration in
``CapabilityRegistry``). It scans ``src/`` and ``tests/`` for
direct writes outside the authority files.

Exit codes:
  0 — clean (no direct writes outside the authority)
  1 — at least one direct write found
  2 — invocation error

Usage:
  python3 scripts/check_sandbox_mode_authority.py             # report
  python3 scripts/check_sandbox_mode_authority.py --strict   # exit 1 on hit
  python3 scripts/check_sandbox_mode_authority.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Files where the SOLE writer is allowed to touch the registry atomic.
# - sandbox.hh is the SSOT authority (set_mode body).
# - sandbox.ixx only re-exports; listed so accidental second body stays
#   scannable but must not reintroduce direct writes.
# - capability_model.hh declares the friend + the private field, but does
#   NOT write the field directly (the friend is the only writer).
AUTHORITY_FILES = {
    "src/core/sandbox.hh",
    "src/core/sandbox.ixx",
    # The CapabilityRegistry field is private; direct writes inside the
    # class (e.g. clear_for_test) are allowed, but the field is
    # forward-declared so no `reg.sandbox_mode = ` should appear here.
    "src/core/capability_model.hh",
}

# File extensions we care about.
SCAN_EXTENSIONS = {".cpp", ".ixx", ".hh", ".cc"}

# Patterns: direct writes to sandbox_mode. The compiled-out (comment-only)
# lines are filtered by re.sub below; the regex intentionally matches the
# textual pattern so callers can grep / GitHub code-search consistently.
# Negative lookahead `(?!=)` excludes the `==` comparison (read) from the
# write pattern, so legitimate acquires / asserts do not trip the gate.
DIRECT_WRITE_PATTERNS = [
    re.compile(r"\bg_capability_registry\s*\(\s*\)\s*\.sandbox_mode\s*=(?!=)"),
    re.compile(r"\breg\.sandbox_mode\s*=(?!=)"),
    re.compile(r"\bregistry\.sandbox_mode\s*=(?!=)"),
]


def _strip_comments(line: str) -> str:
    # Strip line comments; leave block comments intact (rare for these
    # patterns; if encountered the next line will still be scanned).
    s = re.sub(r"//.*", "", line)
    return s


def _scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return (line_no, snippet, raw) for each direct write hit."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    hits: list[tuple[int, str, str]] = []
    for i, raw in enumerate(text.splitlines(), start=1):
        stripped = _strip_comments(raw)
        if not stripped.strip():
            continue
        for pat in DIRECT_WRITE_PATTERNS:
            if pat.search(stripped):
                hits.append((i, raw.strip(), raw))
                break
    return hits


def _is_authority_path(rel: Path) -> bool:
    return rel.as_posix() in AUTHORITY_FILES


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--strict", action="store_true", help="exit 1 on any direct write outside the authority")
    ap.add_argument("--json", action="store_true", help="emit JSON report")
    args = ap.parse_args()

    findings: list[dict] = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix not in SCAN_EXTENSIONS:
            continue
        if any(seg in path.parts for seg in ("build", ".git", "node_modules")):
            continue
        rel = path.relative_to(ROOT)
        hits = _scan_file(path)
        if not hits:
            continue
        for line_no, snippet, _ in hits:
            findings.append(
                {
                    "file": str(rel),
                    "line": line_no,
                    "in_authority": _is_authority_path(rel),
                    "snippet": snippet,
                }
            )

    if args.json:
        print(
            json.dumps(
                {
                    "issue": 2657,
                    "rule": "no direct registry.sandbox_mode write outside set_mode",
                    "authority_files": sorted(AUTHORITY_FILES),
                    "findings": findings,
                },
                indent=2,
            )
        )
    else:
        if not findings:
            print("[#2657] sandbox-mode-authority: 0 hits (all writers route through set_mode).")
            return 0
        outside = [f for f in findings if not f["in_authority"]]
        print(f"[#2657] sandbox-mode-authority: {len(findings)} total hits ({len(outside)} outside authority).")
        for f in findings:
            tag = "(authority)" if f["in_authority"] else "(VIOLATION)"
            print(f"  {tag} {f['file']}:{f['line']}  {f['snippet']}")
        if args.strict and outside:
            print(f"[#2657] strict mode: {len(outside)} violation(s) outside the authority.", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
