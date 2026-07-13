#!/usr/bin/env python3
"""Primitive surface inventory + freeze gate (P0b).

Scans C++ `primitives_.add("name", …)` registrations and:
  1. Writes docs/generated/primitive-inventory.json (all names)
  2. Enforces freeze: no NEW stats-like primitive names vs baseline

Stats-like names (frozen surface — must not grow without explicit baseline bump):
  - endswith "-stats" or "-stats-hash"
  - contains ":*-stats" pattern (e.g. query:foo-stats)
  - listed in ObservabilityPrims::stats_primitives source of truth

Usage:
  python3 scripts/check_primitive_surface.py           # check (CI)
  python3 scripts/check_primitive_surface.py --write   # refresh inventory + baseline
  python3 scripts/check_primitive_surface.py --update-baseline  # allow new stats (explicit)

Exit 0 = OK, 1 = freeze violation or I/O error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIM_GLOB = "src/compiler/evaluator_primitives*.cpp"
OBS_CPP = ROOT / "src/compiler/evaluator_primitives_observability.cpp"
INVENTORY_PATH = ROOT / "docs" / "generated" / "primitive-inventory.json"
BASELINE_PATH = ROOT / "docs" / "generated" / "stats-primitives-baseline.json"

ADD_RE = re.compile(r'add\(\s*"([^"]+)"')
STATS_LIST_RE = re.compile(
    r"kObservabilityStatsPrimitives\s*=\s*\{(.*?)\n\};",
    re.DOTALL,
)


def is_stats_like(name: str) -> bool:
    # Public observability surface freeze: *-stats and *-stats-hash names.
    # bare mutation-history etc. are NOT stats primitives.
    return name.endswith("-stats") or name.endswith("-stats-hash") or "-stats-" in name


def scan_registered_names() -> list[str]:
    names: set[str] = set()
    for path in sorted((ROOT / "src" / "compiler").glob("evaluator_primitives*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in ADD_RE.finditer(text):
            names.add(m.group(1))
    return sorted(names)


def scan_observability_registry() -> list[str]:
    if not OBS_CPP.exists():
        return []
    text = OBS_CPP.read_text(encoding="utf-8", errors="replace")
    m = STATS_LIST_RE.search(text)
    if not m:
        return []
    return sorted(set(re.findall(r'"([^"]+)"', m.group(1))))


def collect_stats_names(all_names: list[str]) -> list[str]:
    from_add = {n for n in all_names if is_stats_like(n)}
    from_reg = set(scan_observability_registry())
    return sorted(from_add | from_reg)


def write_json(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--write",
        action="store_true",
        help="Write inventory JSON (always). Baseline only if missing.",
    )
    ap.add_argument(
        "--update-baseline",
        action="store_true",
        help="Rewrite stats baseline to match current tree (explicit growth).",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        default=True,
        help="Enforce freeze against baseline (default).",
    )
    args = ap.parse_args()

    all_names = scan_registered_names()
    stats_names = collect_stats_names(all_names)
    inventory = {
        "schema": 1,
        "count": len(all_names),
        "stats_count": len(stats_names),
        "names": all_names,
        "stats_names": stats_names,
    }

    if args.write or args.update_baseline or not INVENTORY_PATH.exists():
        write_json(INVENTORY_PATH, inventory)
        print(f"wrote {INVENTORY_PATH.relative_to(ROOT)} ({len(all_names)} names)")

    if args.update_baseline or not BASELINE_PATH.exists():
        write_json(
            BASELINE_PATH,
            {
                "schema": 1,
                "note": (
                    "Frozen set of stats-like primitive names. "
                    "Do not add new *-stats public primitives; use engine:metrics. "
                    "To intentionally expand, run with --update-baseline and justify in PR."
                ),
                "count": len(stats_names),
                "names": stats_names,
            },
        )
        print(f"wrote {BASELINE_PATH.relative_to(ROOT)} ({len(stats_names)} stats names)")
        if args.update_baseline:
            return 0

    # Freeze check
    if not BASELINE_PATH.exists():
        print("ERROR: baseline missing; run with --write first", file=sys.stderr)
        return 1

    baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    base_set = set(baseline.get("names", []))
    cur_set = set(stats_names)
    new = sorted(cur_set - base_set)
    removed = sorted(base_set - cur_set)

    if new:
        print("FAIL: new stats-like primitive names (freeze violation):", file=sys.stderr)
        for n in new:
            print(f"  + {n}", file=sys.stderr)
        print(
            "\nDo not add public *-stats primitives. Expose counters via (engine:metrics) "
            "or CompilerMetrics fields.\n"
            "If this growth is intentional, run:\n"
            "  python3 scripts/check_primitive_surface.py --update-baseline\n"
            "and justify in the PR.",
            file=sys.stderr,
        )
        return 1

    if removed:
        # Allow removals (good); refresh inventory only note
        print(f"OK: {len(removed)} stats name(s) removed since baseline (shrink is good)")
        for n in removed[:10]:
            print(f"  - {n}")
        if len(removed) > 10:
            print(f"  … +{len(removed) - 10} more")

    print(f"OK: primitive surface freeze ({len(all_names)} names, {len(stats_names)} stats, 0 new vs baseline)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
