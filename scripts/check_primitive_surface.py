#!/usr/bin/env python3
"""Primitive surface inventory + freeze gate (P0b / Issue #1432).

Scans C++ `add("name", …)` registrations in evaluator_primitives*.cpp and:
  1. Writes docs/generated/primitive-inventory.json (all names)
  2. Enforces freeze: no NEW blocked-pattern primitive names vs baseline

Blocked patterns (Issue #1432) — no new names matching these:
  - *-stats / *-stats-hash / *-stats-* (observability)
  - string-*, string:*
  - json-*, json:*
  - math-*, math:*
  - vector-*, vector:*
  - path-*, path:*
  - time-*, time:*
  - ast:ref-*  (Issue #393 deprecation; block new names)

Existing matches are grandfathered in stats-primitives-baseline.json.
Empty ALLOWLIST by default (zero exceptions).

Usage:
  python3 scripts/check_primitive_surface.py           # check (CI / gate)
  python3 scripts/check_primitive_surface.py --write   # refresh inventory (+ baseline if missing)
  python3 scripts/check_primitive_surface.py --update-baseline  # allow intentional growth

Exit 0 = OK, 1 = freeze violation or I/O error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OBS_CPP = ROOT / "src/compiler/evaluator_primitives_observability.cpp"
INVENTORY_PATH = ROOT / "docs" / "generated" / "primitive-inventory.json"
BASELINE_PATH = ROOT / "docs" / "generated" / "stats-primitives-baseline.json"

ADD_RE = re.compile(r'add\(\s*"([^"]+)"')
STATS_LIST_RE = re.compile(
    r"kObservabilityStatsPrimitives\s*=\s*\{(.*?)\n\};",
    re.DOTALL,
)

# Issue #1432: empty allowlist — zero exceptions without --update-baseline.
ALLOWLIST: frozenset[str] = frozenset()

# Convenience + ref namespaces (prefix match). Stats handled separately.
BLOCKED_PREFIXES: tuple[str, ...] = (
    "string-",
    "string:",
    "json-",
    "json:",
    "math-",
    "math:",
    "vector-",
    "vector:",
    "path-",
    "path:",
    "time-",
    "time:",
    "ast:ref-",
)


def is_stats_like(name: str) -> bool:
    """Public observability surface: *-stats and *-stats-hash names."""
    return name.endswith("-stats") or name.endswith("-stats-hash") or "-stats-" in name


def blocked_category(name: str) -> str | None:
    """Return freeze category if name is blocked from *new* registration, else None."""
    if name in ALLOWLIST:
        return None
    if is_stats_like(name):
        return "stats"
    for pfx in BLOCKED_PREFIXES:
        if name.startswith(pfx):
            # Normalize to short category label (strip trailing - or :)
            return pfx.rstrip("-:")
    return None


def is_blocked(name: str) -> bool:
    return blocked_category(name) is not None


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


def collect_frozen_names(all_names: list[str]) -> list[str]:
    """All names that match any blocked pattern (grandfathered set source).

    Includes the full stats catalog (add() scan + ObservabilityPrims list)
    so baseline parity with pre-#1432 stats freeze is preserved, plus any
    convenience / ast:ref-* registrations.
    """
    blocked = {n for n in all_names if is_blocked(n)}
    blocked.update(collect_stats_names(all_names))
    return sorted(blocked)


def freeze_violations(current_frozen: list[str], baseline_names: list[str]) -> list[str]:
    """New frozen-pattern names not in baseline (sorted)."""
    return sorted(set(current_frozen) - set(baseline_names))


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
        help="Rewrite frozen-surface baseline to match current tree (explicit growth).",
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
    frozen_names = collect_frozen_names(all_names)
    inventory = {
        "schema": 1,
        "count": len(all_names),
        "stats_count": len(stats_names),
        "frozen_count": len(frozen_names),
        "names": all_names,
        "stats_names": stats_names,
        "frozen_names": frozen_names,
    }

    if args.write or args.update_baseline or not INVENTORY_PATH.exists():
        write_json(INVENTORY_PATH, inventory)
        print(f"wrote {INVENTORY_PATH.relative_to(ROOT)} ({len(all_names)} names)")

    if args.update_baseline or not BASELINE_PATH.exists():
        write_json(
            BASELINE_PATH,
            {
                "schema": 2,
                "note": (
                    "Frozen set of blocked-pattern primitive names (Issue #1432 / P0b). "
                    "No new *-stats, string-*/json-*/math-*/vector-*/path-*/time-* "
                    "(or : forms), or ast:ref-* without --update-baseline + PR justification. "
                    "Prefer stdlib (lib/std/surface) and (engine:metrics)."
                ),
                "count": len(frozen_names),
                "stats_count": len(stats_names),
                "names": frozen_names,
            },
        )
        print(f"wrote {BASELINE_PATH.relative_to(ROOT)} ({len(frozen_names)} frozen names)")
        if args.update_baseline:
            return 0

    # Freeze check
    if not BASELINE_PATH.exists():
        print("ERROR: baseline missing; run with --write first", file=sys.stderr)
        return 1

    baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    base_set = set(baseline.get("names", []))
    cur_set = set(frozen_names)
    new = freeze_violations(frozen_names, sorted(base_set))
    removed = sorted(base_set - cur_set)

    if new:
        print("FAIL: new blocked-pattern primitive names (freeze violation):", file=sys.stderr)
        for n in new:
            cat = blocked_category(n) or "?"
            print(f"  + {n}  [{cat}]", file=sys.stderr)
        print(
            "\nDo not add public convenience/stats/ref primitives.\n"
            "  - counters → CompilerMetrics + (engine:metrics)\n"
            "  - string/json/math/… → lib/std (require std/surface)\n"
            "  - ast:ref-* → StableRef API (Issue #393); no new names\n"
            "If growth is intentional, run:\n"
            "  python3 scripts/check_primitive_surface.py --update-baseline\n"
            "and justify in the PR.",
            file=sys.stderr,
        )
        return 1

    if removed:
        print(f"OK: {len(removed)} frozen name(s) removed since baseline (shrink is good)")
        for n in removed[:10]:
            print(f"  - {n}")
        if len(removed) > 10:
            print(f"  … +{len(removed) - 10} more")

    print(
        f"OK: primitive surface freeze ({len(all_names)} names, "
        f"{len(stats_names)} stats, {len(frozen_names)} frozen-pattern, "
        f"0 new vs baseline)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
