#!/usr/bin/env python3
"""Primitive surface inventory + freeze gate + SlimSurface governance.

Issues:
  - #1432 / P0b: freeze blocked-pattern names vs baseline
  - #1448 / SlimSurface: --strict budget + facade-only stats governance

Scans C++ `add("name", …)` registrations in evaluator_primitives*.cpp and:
  1. Writes docs/generated/primitive-inventory.json (all names)
  2. Enforces freeze: no NEW blocked-pattern primitive names vs baseline
  3. With --strict: budget check toward ≤ TARGET_BUDGET public names,
     and zero *new* public *-stats via add() (facade-only observability)

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
  python3 scripts/check_primitive_surface.py              # check (CI / gate)
  python3 scripts/check_primitive_surface.py --strict     # freeze + budget + facade-only
  python3 scripts/check_primitive_surface.py --write      # refresh inventory
  python3 scripts/check_primitive_surface.py --update-baseline  # intentional growth

Exit 0 = OK, 1 = freeze / strict violation or I/O error.
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

# Issue #1448: SlimSurface target for public engine primitives (add() names).
TARGET_BUDGET = 420
# Interim hard ceiling ratchets down as demotion batches land (#1449).
# After dirty/render demotion (~573 total). Hard-fail only on growth past this.
# Raised 520→521 after #1907 (reflect/EDSL bridge public surface +1).
# Raised 521→523 after #1914 (query:node-provenance + query:last-mutation-provenance
# diagnostic hashes for AI hygiene root-cause; arg-taking, not engine:metrics-only).
INTERIM_HARD_CEILING = 523

# Domain / vertical packs — counted in total inventory; *core* budget
# (→ ≤420) excludes them. See docs/design/epic-1449-surface-slim-v2.md.
DOMAIN_PREFIXES: tuple[str, ...] = (
    "eda:",
    "seva:",
    "verify:",
    "tui:",
    "terminal:",
    "git-",
    "tcp-",
    "auto-evolve-",
    "channel:",
    "m4-",
    "strategy:",
    "synthesize:",
)

# Issue #1965 cycle 2 — formalize which domain prefixes are CORE vs DEFERRED.
# - "core"     = essential for production compiler / runtime, keep in codebase.
# - "deferred" = commercial vertical / UI / integration / AI feature — mark for
#                follow-up issues (filed in cycle 3 governance doc).
# Rationale:
#   - verify:    formal verification integration (safety-critical, keep).
#   - channel:   cross-fiber messaging (agent framework, keep).
#   - tui:       terminal UI (UI vertical) — KEEP deferred, #1967: gated by
#                AURA_ENABLE_TUI + COMMERCIAL_DOMAIN_BUDGETS["tui:"].
#   - eda:       electronic design automation — KEEP deferred, #1968: gated by
#                AURA_ENABLE_EDA + COMMERCIAL_DOMAIN_BUDGETS["eda:"].
#   - auto-evolve-: self-evolution AI — KEEP deferred, #1969: gated by
#                AURA_ENABLE_AUTO_EVOLVE + COMMERCIAL_DOMAIN_BUDGETS["auto-evolve-"].
#   - git-:      git integration (integration, defer).
#   - terminal:  terminal ops (UI/integration, defer).
#   - seva:      service evaluation (commercial vertical, defer).
#   - strategy:  strategy DSL (commercial vertical, defer).
#   - synthesize: synthesis (commercial vertical, defer).
#   - tcp-:      TCP networking (integration, defer).
#   - m4-:       m4 macro processor (integration, defer).
DOMAIN_STATUS: dict[str, str] = {
    "verify:": "core",
    "channel:": "core",
    "tui:": "deferred",
    "eda:": "deferred",
    "auto-evolve-": "deferred",
    "git-": "deferred",
    "terminal:": "deferred",
    "seva:": "deferred",
    "strategy:": "deferred",
    "synthesize:": "deferred",
    "tcp-": "deferred",
    "m4-": "deferred",
}

# Issue #1967: per-prefix commercial / UI domain budgets.
# Deferred DOMAIN_STATUS prefixes that are KEPT (not deleted) must not grow
# without an intentional budget raise in this map + PR justification.
# Count is source-scanned add("prefix…") names (same as freeze inventory).
COMMERCIAL_DOMAIN_BUDGETS: dict[str, int] = {
    "tui:": 21,  # #1967 — terminal UI vertical; AURA_ENABLE_TUI build flag
    "eda:": 13,  # #1968 — EDA vertical; AURA_ENABLE_EDA build flag
    "auto-evolve-": 7,  # #1969 — self-evo AI vertical; AURA_ENABLE_AUTO_EVOLVE
}

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


def is_domain_name(name: str) -> bool:
    return any(name.startswith(p) for p in DOMAIN_PREFIXES)


def domain_status(name: str) -> str:
    """Issue #1965 cycle 2: 'core' or 'deferred' for a domain prefix.
    Returns 'unknown' for non-domain names."""
    if not is_domain_name(name):
        return "unknown"
    for p in DOMAIN_PREFIXES:
        if name.startswith(p):
            return DOMAIN_STATUS.get(p, "deferred")
    return "unknown"


def core_public_names(all_names: list[str]) -> list[str]:
    return [n for n in all_names if not is_domain_name(n)]


def domain_breakdown(all_names: list[str]) -> dict[str, dict[str, int]]:
    """Per-prefix count bucketed by domain_status. Issue #1965 cycle 2."""
    out: dict[str, dict[str, int]] = {}
    for n in all_names:
        if not is_domain_name(n):
            continue
        for p in DOMAIN_PREFIXES:
            if n.startswith(p):
                bucket = out.setdefault(p, {"core": 0, "deferred": 0})
                bucket[DOMAIN_STATUS.get(p, "deferred")] += 1
                break
    return out


def commercial_domain_counts(all_names: list[str]) -> dict[str, int]:
    """Per-prefix counts for COMMERCIAL_DOMAIN_BUDGETS (#1967)."""
    counts = dict.fromkeys(COMMERCIAL_DOMAIN_BUDGETS, 0)
    for n in all_names:
        for p in COMMERCIAL_DOMAIN_BUDGETS:
            if n.startswith(p):
                counts[p] += 1
                break
    return counts


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


def public_stats_via_add(all_names: list[str]) -> list[str]:
    """Stats-like names still registered via public add() (not facade-only)."""
    return sorted(n for n in all_names if is_stats_like(n))


def write_json(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def classify_public(all_names: list[str]) -> dict[str, list[str]]:
    """Bucket public add() names for stats:list-style reporting."""
    buckets: dict[str, list[str]] = {
        "stats-like": [],
        "query": [],
        "mutate": [],
        "compile": [],
        "workspace": [],
        "engine": [],
        "other": [],
    }
    for n in all_names:
        if is_stats_like(n):
            buckets["stats-like"].append(n)
        elif n.startswith("query:") or n.startswith("query-"):
            buckets["query"].append(n)
        elif n.startswith("mutate:") or n.startswith("mutate-"):
            buckets["mutate"].append(n)
        elif n.startswith("compile:") or n.startswith("compile-"):
            buckets["compile"].append(n)
        elif n.startswith("workspace:") or n.startswith("workspace-"):
            buckets["workspace"].append(n)
        elif n.startswith("engine:") or n.startswith("stats:"):
            buckets["engine"].append(n)
        else:
            buckets["other"].append(n)
    return buckets


def run_strict_checks(all_names: list[str], stats_names: list[str]) -> int:
    """Issue #1448 --strict: budget + facade-only + category report.

    Returns 0 if OK, 1 if violation.
    """
    rc = 0
    public_count = len(all_names)
    core_names = core_public_names(all_names)
    core_count = len(core_names)
    domain_count = public_count - core_count
    public_stats = public_stats_via_add(all_names)
    buckets = classify_public(all_names)

    print("── SlimSurface --strict (Issue #1448 / #1449) ──")
    print(f"  public add() total : {public_count}")
    print(f"  core public count  : {core_count}  (excludes domain verticals)")
    print(f"  domain public count: {domain_count}  ({', '.join(DOMAIN_PREFIXES[:6])}…)")
    print(f"  core target budget : {TARGET_BUDGET}")
    print(f"  interim hard ceiling (total): {INTERIM_HARD_CEILING}")
    print(f"  internal stats catalog (register_stats_impl / list): {len(stats_names)}")
    print(f"  public add() *-stats remaining: {len(public_stats)}")
    print("  categories:")
    for k, v in buckets.items():
        print(f"    {k:12s} {len(v)}")
    # Issue #1965 cycle 2: per-prefix core vs deferred breakdown.
    print("  domain status (Issue #1965 cycle 2):")
    breakdown = domain_breakdown(all_names)
    core_dom = sum(v.get("core", 0) for v in breakdown.values())
    def_dom = sum(v.get("deferred", 0) for v in breakdown.values())
    print(f"    total core domain     : {core_dom}")
    print(f"    total deferred domain : {def_dom}")
    for p in sorted(breakdown.keys()):
        b = breakdown[p]
        print(f"    {p:18s} core={b.get('core', 0):2d}  deferred={b.get('deferred', 0):2d}")

    # Issue #1967 / #1968 / #1969: commercial UI / vertical budgets.
    print("  commercial domain budgets (Issue #1967 / #1968 / #1969):")
    commercial_counts = commercial_domain_counts(all_names)
    for p in sorted(COMMERCIAL_DOMAIN_BUDGETS.keys()):
        budget = COMMERCIAL_DOMAIN_BUDGETS[p]
        n = commercial_counts.get(p, 0)
        status = "OK" if n <= budget else "FAIL"
        print(f"    {p:18s} count={n:2d}  budget={budget:2d}  [{status}]")
        if n > budget:
            print(
                f"FAIL: commercial domain {p!r} has {n} primitives, "
                f"budget is {budget} (Issue #1967). Raise "
                f"COMMERCIAL_DOMAIN_BUDGETS in scripts/check_primitive_surface.py "
                f"only with explicit PR justification.",
                file=sys.stderr,
            )
            rc = 1

    # Hard ceiling — no growth past interim limit (total surface).
    if public_count > INTERIM_HARD_CEILING:
        print(
            f"FAIL: public primitive count {public_count} exceeds interim hard ceiling "
            f"{INTERIM_HARD_CEILING} (Issue #1448).",
            file=sys.stderr,
        )
        rc = 1

    # Progress toward target uses *core* count (domain packs tracked separately).
    if core_count > TARGET_BUDGET:
        print(
            f"NOTE: core public count {core_count} still above SlimSurface target "
            f"{TARGET_BUDGET} (gap {core_count - TARGET_BUDGET}; domain {domain_count} excluded)"
        )
    else:
        print(f"OK: core public count {core_count} ≤ target budget {TARGET_BUDGET}")

    # Facade-only: public add() of new stats is already freeze-blocked.
    # Strict additionally flags any remaining public stats via add() as
    # debt (soft), and fails if count grows beyond grandfathered set.
    if public_stats:
        print(
            f"NOTE: {len(public_stats)} public add() *-stats still registered "
            f"(prefer register_stats_impl + engine:metrics). First 10:"
        )
        for n in public_stats[:10]:
            print(f"  - {n}")
        if len(public_stats) > 10:
            print(f"  … +{len(public_stats) - 10} more")

    if rc == 0:
        print("OK: SlimSurface --strict checks passed")
    return rc


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
    ap.add_argument(
        "--strict",
        action="store_true",
        help=(
            "Issue #1448 SlimSurface governance: freeze + public budget "
            f"(target ≤{TARGET_BUDGET}, hard ceiling {INTERIM_HARD_CEILING}) + "
            "facade-only stats reporting."
        ),
    )
    args = ap.parse_args()

    all_names = scan_registered_names()
    stats_names = collect_stats_names(all_names)
    frozen_names = collect_frozen_names(all_names)
    inventory = {
        "schema": 2,
        "count": len(all_names),
        "stats_count": len(stats_names),
        "frozen_count": len(frozen_names),
        "target_budget": TARGET_BUDGET,
        "interim_hard_ceiling": INTERIM_HARD_CEILING,
        "public_stats_via_add": len(public_stats_via_add(all_names)),
        "names": all_names,
        "stats_names": stats_names,
        "frozen_names": frozen_names,
        "categories": {k: len(v) for k, v in classify_public(all_names).items()},
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
                    "Frozen set of blocked-pattern primitive names (Issue #1432 / P0b / #1448). "
                    "No new *-stats, string-*/json-*/math-*/vector-*/path-*/time-* "
                    "(or : forms), or ast:ref-* without --update-baseline + PR justification. "
                    "Prefer stdlib (lib/std/surface) and (engine:metrics). "
                    f"SlimSurface target public budget: {TARGET_BUDGET}."
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
            "and justify in the PR (Issue #1448 governance).",
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

    if args.strict:
        return run_strict_checks(all_names, stats_names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
