#!/usr/bin/env python3
"""Gate: no new fixed source windows in the tests / checker corpus.

Issue #3480 follow-up. A `text.substr(anchor, N)` (or `text[start:start+N]`)
window rots as soon as the anchored body grows: #3975-#3988, #4332, #4385,
#4910 each pushed a body past its budget and the assertion silently started
reading an empty slice. The migrations replaced the documented cases with
brace-matched bodies (aura::test::aura_source_block_after /
scripts/coverage/checks/_source_scan.py); this gate freezes what is left so
new windows cannot accumulate.

Policy: a file may keep or shrink its existing window count; exceeding the
baseline fails and names the new lines. Prefer the brace helpers instead.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASELINE = Path(__file__).resolve().parents[1] / "source_window_baseline.json"

# Scanned corpora. Also the paths run_checks.py uses to select this gate on a
# --changed run.
SCAN_DIRS = ("tests/core", "tests/compiler", "tests/serve", "tests/reflect", "tests/orch")
CHECKER_DIR = "scripts/coverage/checks"

WINDOW_MIN = 1500  # a body window; shorter budgets are local near-anchor probes

SUFFIXES = (".cpp", ".hpp", ".hh")
CXX_WINDOW_RE = re.compile(r"\.substr\(\s*[^,]+,\s*(\d+)\s*\)")
PY_WINDOW_RE = re.compile(r"\[\s*\w+\s*:\s*\w+\s*\+\s*(\d+)\s*\]")


def _scan() -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    for rel_dir in SCAN_DIRS:
        base = ROOT / rel_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES:
                continue
            rel = str(path.relative_to(ROOT))
            for lineno, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
                for match in CXX_WINDOW_RE.finditer(line):
                    if int(match.group(1)) >= WINDOW_MIN:
                        found.setdefault(rel, []).append(f"{rel}:{lineno}: {line.strip()}")
    checks = ROOT / CHECKER_DIR
    if checks.is_dir():
        for path in sorted(checks.glob("check_*.py")):
            rel = str(path.relative_to(ROOT))
            for lineno, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
                for match in PY_WINDOW_RE.finditer(line):
                    if int(match.group(1)) >= WINDOW_MIN:
                        found.setdefault(rel, []).append(f"{rel}:{lineno}: {line.strip()}")
    return found


def _load() -> dict[str, int]:
    if not BASELINE.is_file():
        return {}
    return json.loads(BASELINE.read_text(encoding="utf-8")).get("counts", {})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--update-baseline", action="store_true")
    args = parser.parse_args()

    found = _scan()
    current = {k: len(v) for k, v in sorted(found.items())}

    if args.update_baseline:
        BASELINE.write_text(
            json.dumps(
                {
                    "issue": "#3480 follow-up",
                    "policy": f"fixed source windows >= {WINDOW_MIN} chars: count may only shrink",
                    "counts": current,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"source-window baseline written: {len(current)} file(s), {sum(current.values())} window(s)")
        return 0

    baseline = _load()
    if not baseline:
        print("FAIL: source_window_baseline.json missing; run --update-baseline once", file=sys.stderr)
        return 1

    grown = [(rel, n - baseline.get(rel, 0)) for rel, n in current.items() if n > baseline.get(rel, 0)]
    if grown:
        for rel, extra in grown:
            print(
                f"FAIL: {rel}: {baseline.get(rel, 0)} -> {current[rel]} fixed source window(s) (+{extra})",
                file=sys.stderr,
            )
            for line in found[rel][-extra:]:
                print(f"      {line}", file=sys.stderr)
        print(
            "      hint: use aura::test::aura_source_block_after / source_block_after, "
            f"or keep the budget below {WINDOW_MIN} chars",
            file=sys.stderr,
        )
        return 1

    shrunk = sum(1 for rel, n in current.items() if n < baseline.get(rel, 0))
    removed = sum(1 for rel in baseline if rel not in current)
    total = sum(current.values())
    print(
        f"OK: source windows — {total} fixed budget(s) across {len(current)} file(s); "
        f"no file grew ({shrunk} shrank, {removed} cleared; run --update-baseline to ratchet)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
