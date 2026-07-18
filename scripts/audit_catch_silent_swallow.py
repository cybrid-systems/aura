#!/usr/bin/env python3
"""Issue #1669 / #615: audit catch(...) for silent-swallow hygiene.

Every ``catch (...)`` in the EDSL / compiler surface must carry a nearby
``[SILENCE-PRIM-#…]`` marker documenting why the catch is intentional
(class A return-value / class B state-change). Unmarked catches are
regression candidates for silent error swallowing.

Usage:
  python3 scripts/audit_catch_silent_swallow.py          # report, exit 0
  python3 scripts/audit_catch_silent_swallow.py --strict # exit 1 if unmarked
  python3 scripts/audit_catch_silent_swallow.py --json

Exit codes:
  0 — clean (or report-only with findings)
  1 — --strict and at least one unmarked catch(...)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "compiler"

CATCH_RE = re.compile(r"catch\s*\(\s*\.\.\.\s*\)")
SILENCE_RE = re.compile(r"SILENCE-PRIM")
# Marker must appear in the catch body window (same line or following lines).
MARKER_WINDOW = 12


def _code_line(line: str) -> str:
    return re.sub(r"//.*", "", line).strip()


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return (line_no, status, snippet) for each catch(...).

    status is 'marked' or 'unmarked'.
    """
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    hits: list[tuple[int, str, str]] = []
    for i, line in enumerate(lines):
        code = _code_line(line)
        if not code or not CATCH_RE.search(code):
            continue
        window = "\n".join(lines[i : min(len(lines), i + MARKER_WINDOW)])
        # Also allow marker on the catch line itself after the brace.
        marked = bool(SILENCE_RE.search(window))
        status = "marked" if marked else "unmarked"
        hits.append((i + 1, status, line.strip()[:100]))
    return hits


def collect_hits(path: Path = SRC, *, unmarked_only: bool = False) -> list[tuple[Path, int, str, str]]:
    root = path if path.is_absolute() else ROOT / path
    files = [root] if root.is_file() else sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.ixx"))
    out: list[tuple[Path, int, str, str]] = []
    for f in files:
        try:
            rel = f.relative_to(ROOT)
        except ValueError:
            rel = f
        for ln, status, snip in scan_file(f):
            if unmarked_only and status != "unmarked":
                continue
            out.append((rel, ln, status, snip))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--strict", action="store_true", help="exit 1 if any unmarked catch(...)")
    ap.add_argument("--json", action="store_true", help="JSON report")
    ap.add_argument(
        "--path",
        type=Path,
        default=SRC,
        help="directory or file (default: src/compiler)",
    )
    ap.add_argument(
        "--all",
        action="store_true",
        help="include marked sites in the report (default: unmarked only for text)",
    )
    args = ap.parse_args()

    all_hits = collect_hits(args.path, unmarked_only=False)
    unmarked = [h for h in all_hits if h[2] == "unmarked"]
    marked = [h for h in all_hits if h[2] == "marked"]

    if args.json:
        payload = {
            "issue": 1669,
            "total": len(all_hits),
            "marked": len(marked),
            "unmarked": len(unmarked),
            "sites": [
                {"path": str(p), "line": ln, "status": st, "snippet": sn}
                for p, ln, st, sn in (all_hits if args.all else unmarked)
            ],
        }
        print(json.dumps(payload, indent=2))
        if args.strict and unmarked:
            return 1
        return 0

    print(f"audit_catch_silent_swallow: {len(all_hits)} catch(...) ({len(marked)} marked, {len(unmarked)} unmarked)")
    report = all_hits if args.all else unmarked
    for path, ln, st, snip in report:
        print(f"  {path}:{ln}: [{st}] {snip}")

    if not unmarked:
        print("audit_catch_silent_swallow: clean (0 unmarked)")
        return 0

    if args.strict:
        print("audit_catch_silent_swallow: FAIL (--strict)", file=sys.stderr)
        return 1
    print("audit_catch_silent_swallow: report-only (use --strict to gate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
