#!/usr/bin/env python3
"""Rewrite query:/compile:*-stats call sites to (engine:metrics "…") — #1434/#1439.

Safe transforms (tests/, demos/, lib/ only — never registration lines):
  (query:foo-stats)  →  (engine:metrics "query:foo-stats")
  Escapes quotes inside C++ "…" string literals; raw R"(…)" keeps plain quotes.

Skips:
  - lines containing add( / register_stats_impl
  - lines that already contain engine:metrics
  - src/compiler registration TUs

Usage:
  python3 scripts/migrate_top_stats_to_facade.py --dry-run
  python3 scripts/migrate_top_stats_to_facade.py            # top-20 only
  python3 scripts/migrate_top_stats_to_facade.py --all      # every name in tree
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from find_top_stats import NAME_RE, PINNED_TOP20, iter_files  # noqa: E402

SCAN_ROOTS = ("tests", "demos", "lib")
EXTS = {".cpp", ".aura", ".h", ".hpp"}


def discover_all_names() -> list[str]:
    names: set[str] = set()
    for path in iter_files():
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in NAME_RE.finditer(text):
            names.add(m.group(1))
    return sorted(names, key=len, reverse=True)


def migrate_text(text: str, names: list[str], *, cpp: bool) -> tuple[str, int]:
    """Rewrite call sites. For C++ sources, escape inner quotes in "…" literals."""
    changes = 0
    out_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        if "engine:metrics" in line:
            out_lines.append(line)
            continue
        if re.search(r'\badd\s*\(\s*["\']', line) or "register_stats_impl" in line:
            out_lines.append(line)
            continue
        new = line
        for name in names:
            pat = re.compile(rf"\({re.escape(name)}\)")
            if cpp:
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
    ap.add_argument(
        "--all",
        action="store_true",
        help="Migrate every query:/compile:*-stats name (not only top-20).",
    )
    args = ap.parse_args()

    names = discover_all_names() if args.all else list(PINNED_TOP20)
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
