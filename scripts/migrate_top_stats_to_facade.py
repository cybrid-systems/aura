#!/usr/bin/env python3
"""Rewrite top-20 query:*-stats call sites to (engine:metrics "…") — Issue #1434.

Safe transforms (tests/, demos/, lib/ only — never registration add("…")):
  (query:foo-stats)  →  (engine:metrics "query:foo-stats")
  "(query:foo-stats)" in C++ string literals when the whole expr is that form

Skips:
  - lines containing add(" / add('
  - lines that already contain engine:metrics
  - src/compiler registration TUs

Usage:
  python3 scripts/migrate_top_stats_to_facade.py --dry-run
  python3 scripts/migrate_top_stats_to_facade.py
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from find_top_stats import PINNED_TOP20  # noqa: E402

SCAN_ROOTS = ("tests", "demos", "lib")
EXTS = {".cpp", ".aura", ".h", ".hpp"}


def migrate_text(text: str, names: list[str], *, cpp: bool) -> tuple[str, int]:
    """Rewrite call sites. For C++ sources, escape inner quotes in "…" literals."""
    changes = 0
    out_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        if "engine:metrics" in line:
            out_lines.append(line)
            continue
        if re.search(r'\badd\s*\(\s*["\']', line):
            out_lines.append(line)
            continue
        new = line
        for name in names:
            # Bare form (query:foo-stats) — Aura source or raw string body.
            pat = re.compile(rf"\({re.escape(name)}\)")
            if cpp:
                # Prefer escaped form safe inside C++ "…":
                #   "(engine:metrics \"query:foo-stats\")"
                # When already inside R"(…)" raw strings, Aura quotes are fine
                # but escaped form still parses as \" which is wrong in raw.
                # Detect raw string: R"( or R"X(
                if re.search(r'R"[A-Za-z]*\(', new):
                    repl = f'(engine:metrics "{name}")'
                else:
                    repl = f'(engine:metrics \\"{name}\\")'
            else:
                repl = f'(engine:metrics "{name}")'
            new2, n = pat.subn(repl, new)
            if n:
                new = new2
                changes += n
        out_lines.append(new)
    return "".join(out_lines), changes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    names = list(PINNED_TOP20)
    # Longest first so longer stats names win over prefixes
    names.sort(key=len, reverse=True)

    total_files = 0
    total_changes = 0
    for root_name in SCAN_ROOTS:
        root = ROOT / root_name
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix not in EXTS:
                continue
            if "build" in path.parts:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            new, n = migrate_text(text, names, cpp=path.suffix in {".cpp", ".h", ".hpp", ".ixx"})
            if n == 0:
                continue
            total_files += 1
            total_changes += n
            rel = path.relative_to(ROOT)
            print(f"{'would rewrite' if args.dry_run else 'rewrote'} {rel} ({n} subs)")
            if not args.dry_run:
                path.write_text(new, encoding="utf-8")

    print(f"\n{total_files} files, {total_changes} substitutions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
