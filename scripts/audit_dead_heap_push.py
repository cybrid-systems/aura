#!/usr/bin/env python3
"""Issue #1488 / #1072 / #1668: audit dead string_heap_ push patterns.

Flags sites where an index is taken from string_heap_.size(), a push_back
follows, and the index is never *really* used (incomplete-refactor leftover
that pollutes the heap on every call). Also flags bare push_back with no
nearby size capture (index never taken).

Usage:
  python3 scripts/audit_dead_heap_push.py          # report, exit 0
  python3 scripts/audit_dead_heap_push.py --strict # exit 1 if any candidate
  python3 scripts/audit_dead_heap_push.py --json   # machine-readable report

Exit codes:
  0 — clean (or report-only with findings)
  1 — --strict and at least one candidate
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

# size capture: auto/size_t/uint64_t idx = [ev.]string_heap_.size();
SIZE_RE = re.compile(
    r"(?:auto|const\s+auto|std::size_t|size_t|std::uint64_t|uint64_t)\s+(\w+)\s*=\s*"
    r"(?:ev\.)?string_heap_\.size\(\)\s*;"
)
# Any string_heap_.size() in a prior window (covers multi-line string literals
# between size capture and push — e.g. seva:generate-regression).
SIZE_ANY_RE = re.compile(r"(?:ev\.)?string_heap_\.size\s*\(\s*\)")
PUSH_RE = re.compile(r"(?:ev\.)?string_heap_\.(?:push_back|emplace_back)\s*\(")

# Intentional dual-heap mirror (IR executor keeps local string_heap_ aligned
# with primitives heap for coercion; make_string uses prim_idx). #1668.
ALLOWLIST_SUBSTR: tuple[tuple[str, str], ...] = (
    ("src/compiler/ir_executor_impl.cpp", "string_heap_.push_back(module_.string_pool"),
    ("src/compiler/ir_executor_impl.cpp", 'string_heap_.push_back("")'),
)


def _strip_line_comment(line: str) -> str:
    return re.sub(r"//.*", "", line)


def _code_line(line: str) -> str:
    """Code portion only (no // comments). Empty if line is comment-only."""
    return _strip_line_comment(line).strip()


def is_void_only_use(line: str, name: str) -> bool:
    """True when the only mention of name is a (void) cast (warning silence)."""
    wl = _code_line(line)
    if not re.search(rf"\b{re.escape(name)}\b", wl):
        return False
    cleaned = re.sub(rf"\(void\)\s*{re.escape(name)}\b", "", wl)
    return not re.search(rf"\b{re.escape(name)}\b", cleaned)


def has_real_use(line: str, name: str) -> bool:
    wl = _code_line(line)
    if not re.search(rf"\b{re.escape(name)}\b", wl):
        return False
    return not is_void_only_use(line, name)


def is_allowlisted(rel: str, line: str) -> bool:
    rel_n = rel.replace("\\", "/")
    code = _code_line(line)
    for path_suf, needle in ALLOWLIST_SUBSTR:
        if (rel_n.endswith(path_suf) or rel_n == path_suf) and (needle in code or needle in line):
            return True
    return False


def scan_file(path: Path, *, rel: str | None = None) -> list[tuple[int, str, str, str]]:
    """Return (line_no, var_or_bare, kind, snippet) for dead candidates.

    kind is 'unused-index' or 'bare-push'.
    """
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    rel_s = rel if rel is not None else str(path)
    hits: list[tuple[int, str, str, str]] = []
    size_lines: set[int] = set()  # 0-based lines that are size captures

    for i, line in enumerate(lines):
        code = _code_line(line)
        if not code:
            continue
        m = SIZE_RE.search(code)
        if not m:
            continue
        size_lines.add(i)
        name = m.group(1)
        push_at: int | None = None
        for j in range(i + 1, min(len(lines), i + 40)):
            jc = _code_line(lines[j])
            if jc and PUSH_RE.search(jc):
                push_at = j
                break
        if push_at is None:
            continue
        used = False
        for j in range(push_at + 1, min(len(lines), i + 120)):
            wl = _code_line(lines[j])
            if not wl:
                continue
            if re.search(
                rf"(?:auto|const\s+auto|std::size_t|size_t|std::uint64_t|uint64_t)\s+"
                rf"{re.escape(name)}\s*=",
                wl,
            ):
                break
            if has_real_use(lines[j], name):
                used = True
                break
        if not used:
            hits.append((i + 1, name, "unused-index", lines[i].strip()))

    # Bare push: push_back without string_heap_.size() in the previous 30 lines.
    for i, line in enumerate(lines):
        code = _code_line(line)
        if not code or not PUSH_RE.search(code):
            continue
        if is_allowlisted(rel_s, line):
            continue
        # size() anywhere in previous 30 lines of code?
        window = "\n".join(_code_line(lines[j]) for j in range(max(0, i - 30), i))
        if SIZE_ANY_RE.search(window):
            continue
        if any((i - k) in size_lines for k in range(1, 31) if i - k >= 0):
            continue
        hits.append((i + 1, "(bare)", "bare-push", code[:120]))

    return hits


def collect_hits(path: Path = SRC) -> list[tuple[Path, int, str, str, str]]:
    """Scan path (file or directory) → list of (relpath, line, name, kind, snip)."""
    root = path if path.is_absolute() else (ROOT / path if not path.is_absolute() else path)
    if not root.is_absolute():
        root = ROOT / path
    files = [root] if root.is_file() else sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.ixx"))
    all_hits: list[tuple[Path, int, str, str, str]] = []
    for f in files:
        try:
            rel = f.relative_to(ROOT)
        except ValueError:
            rel = f
        for ln, name, kind, snip in scan_file(f, rel=str(rel)):
            all_hits.append((rel, ln, name, kind, snip))
    return all_hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 when any dead candidate is found",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="emit JSON report to stdout",
    )
    ap.add_argument(
        "--path",
        type=Path,
        default=SRC,
        help="directory or file to scan (default: src/)",
    )
    args = ap.parse_args()

    all_hits = collect_hits(args.path)

    if args.json:
        payload = {
            "issue": 1668,
            "candidates": [
                {
                    "path": str(p),
                    "line": ln,
                    "name": name,
                    "kind": kind,
                    "snippet": snip,
                }
                for p, ln, name, kind, snip in all_hits
            ],
            "count": len(all_hits),
        }
        print(json.dumps(payload, indent=2))
        if args.strict and all_hits:
            return 1
        return 0

    if not all_hits:
        print("audit_dead_heap_push: clean (0 candidates)")
        return 0

    print(f"audit_dead_heap_push: {len(all_hits)} candidate(s)")
    for path, ln, name, kind, snip in all_hits:
        print(f"  {path}:{ln}: [{kind}] unused '{name}' after string_heap_ push")
        print(f"    {snip}")

    if args.strict:
        print("audit_dead_heap_push: FAIL (--strict)", file=sys.stderr)
        return 1
    print("audit_dead_heap_push: report-only (use --strict to gate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
