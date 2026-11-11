#!/usr/bin/env python3
"""Primitive tier list + registration-count ratchet (Issue #5008).

Two contracts, both fed by the ONE scan in scripts/tools/prim_surface.py:

1. Tier coverage.  Every name the engine registers (register_prim / add() in
   src/**/*.cpp *and* the evaluator ctor hot table in
   src/compiler/evaluator_primitives_builtins.cpp) must have a tier in
   docs/stdlib/primitive-tiers.json:

     kernel            Aura cannot implement it without losing atomicity or a
                       host effect
     stdlib-candidate  pure value->value function Aura can host
     delete            redundant: another registered name already provides it

   docs/generated/primitives.md is rendered from that same file, so the
   catalog the docs show and the catalog this gate checks cannot drift.

2. Growth ratchet.  Relative to the landing baseline
   (docs/generated/primitives-baseline.json) the registration count may only
   shrink.  A new name must be paid for by removing at least one registration
   in the same change, or be listed in "kernel_exceptions" (the authoring
   contract's kernel exception table) with a justification.

3. Retirement channel.  A name still registered from the deprecated set must
   be flagged "deprecated": true in the tier list; a name flagged deprecated
   must not be called from lib/std.

Usage:
  python3 scripts/coverage/checks/check_primitive_tiers_5008.py
  python3 scripts/coverage/checks/check_primitive_tiers_5008.py --update-baseline
  python3 scripts/coverage/checks/check_primitive_tiers_5008.py --report
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "scripts" / "tools"
sys.path.insert(0, str(TOOLS))

import gen_docs  # noqa: E402 — DEPRECATED_PRIMITIVES (Issue #1438), one source
import prim_surface  # noqa: E402 — the shared registration scan

TIERS_PATH = ROOT / "docs" / "stdlib" / "primitive-tiers.json"
BASELINE_PATH = ROOT / "docs" / "generated" / "primitives-baseline.json"
HOT_TABLE_TU = "src/compiler/evaluator_primitives_builtins.cpp"
STDLIB_DIR = "lib/std/"

VALID_TIERS = ("kernel", "stdlib-candidate", "delete")


def load_tiers() -> dict:
    return json.loads(TIERS_PATH.read_text(encoding="utf-8"))


def write_baseline(names: list[str]) -> None:
    doc = {
        "schema": 1,
        "issue": 5008,
        "note": (
            "Registration-count ratchet baseline (Issue #5008). Written by "
            "check_primitive_tiers_5008.py --update-baseline. The count may only "
            "shrink; growth needs a matching removal or an entry in "
            "kernel_exceptions."
        ),
        "channels": ["register_prim / add() in src/**/*.cpp", HOT_TABLE_TU],
        "count": len(names),
        "names": names,
    }
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_PATH.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {BASELINE_PATH.relative_to(ROOT)} ({len(names)} names)")


def check_tiers(tiers: dict, catalog: list[str]) -> list[str]:
    errors: list[str] = []
    entries: dict[str, dict] = tiers.get("primitives", {})
    rule_notes: dict[str, str] = tiers.get("rule_notes", {})
    cur = set(catalog)

    missing = sorted(cur - set(entries))
    if missing:
        errors.append(
            f"{len(missing)} registered primitive(s) have no tier in "
            f"{TIERS_PATH.relative_to(ROOT)}:\n"
            + "\n".join(f"  + {n}" for n in missing)
            + "\n  Assign each one kernel / stdlib-candidate / delete and record the rule."
        )

    stale = sorted(set(entries) - cur)
    if stale:
        errors.append(
            f"{len(stale)} tier entr(ies) no longer match a registration "
            f"(renamed or removed without updating the list):\n" + "\n".join(f"  - {n}" for n in stale)
        )

    bad_tier = sorted(n for n, e in entries.items() if e.get("tier") not in VALID_TIERS)
    if bad_tier:
        errors.append(
            "invalid tier value (expected "
            + " / ".join(VALID_TIERS)
            + "):\n"
            + "\n".join(f"  ? {n}: {entries[n].get('tier')!r}" for n in bad_tier)
        )

    bad_rule = sorted(n for n, e in entries.items() if not e.get("rule") or e.get("rule") not in rule_notes)
    if bad_rule:
        errors.append(
            "tier entry without a known rule id (rule_notes is the audit trail):\n"
            + "\n".join(f"  ? {n}: {entries[n].get('rule')!r}" for n in bad_rule)
        )

    counts = tiers.get("counts")
    if isinstance(counts, dict):
        live: dict[str, int] = {t: 0 for t in VALID_TIERS}
        for e in entries.values():
            if e.get("tier") in live:
                live[e["tier"]] += 1
        if {k: counts.get(k) for k in live} != live:
            errors.append(f"stale counts block: file says {counts}, entries say {live}")

    exc = tiers.get("kernel_exceptions", [])
    if not isinstance(exc, list):
        errors.append("kernel_exceptions must be a list of primitive names")
        exc = []
    orphan = sorted(set(exc) - cur)
    if orphan:
        errors.append(
            "kernel_exceptions names that are not registered (remove them):\n" + "\n".join(f"  - {n}" for n in orphan)
        )

    return errors


def check_growth(tiers: dict, catalog: list[str], *, quiet: bool = False) -> list[str]:
    if not BASELINE_PATH.exists():
        return [
            f"missing {BASELINE_PATH.relative_to(ROOT)}; run with --update-baseline once to record the landing count"
        ]
    base_doc = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    base = set(base_doc.get("names", []))
    base_count = int(base_doc.get("count", len(base)))
    cur = set(catalog)
    added = sorted(cur - base)
    removed = sorted(base - cur)
    exc = set(tiers.get("kernel_exceptions", []))
    unpaid = [n for n in added if n not in exc]

    errors: list[str] = []
    if len(unpaid) > len(removed):
        src = prim_surface.scan_sources()
        detail = "\n".join(f"  + {n}  ({src.get(n, ['?'])[0]})" for n in unpaid)
        errors.append(
            f"primitive catalog grew without paying for it (Issue #5008).\n"
            f"  baseline {base_count} ({BASELINE_PATH.relative_to(ROOT)}) -> now {len(cur)}\n"
            f"  {len(added)} added, {len(removed)} removed, "
            f"{len(unpaid)} addition(s) not paid for:\n{detail}\n"
            "  Fix one of:\n"
            "    - remove a registration in the same change (one per added name), or\n"
            "    - list the name in kernel_exceptions of docs/stdlib/primitive-tiers.json\n"
            "      (authoring contract: kernel exception table), with a justification."
        )
    elif not quiet:
        covered = len(added) - len(unpaid)
        print(
            f"OK: catalog {len(cur)} vs baseline {base_count} "
            f"({len(added)} added, {len(removed)} removed, "
            f"{covered} addition(s) covered by kernel_exceptions)"
        )
    return errors


def check_retirement(tiers: dict, catalog: list[str]) -> list[str]:
    errors: list[str] = []
    entries: dict[str, dict] = tiers.get("primitives", {})
    cur = set(catalog)
    refs = prim_surface.stdlib_call_references(sorted(entries))

    flagged = sorted(n for n, e in entries.items() if e.get("deprecated"))
    for name in flagged:
        if name not in cur:
            errors.append(f"{name!r} is flagged deprecated but no longer registered; drop the flag")
        if name in refs:
            errors.append(f"{name!r} is deprecated but lib/std still calls it: " + ", ".join(refs[name][:3]))

    # Issue #5008 acceptance: either the deprecated list is non-empty and
    # visible, or the retired names are already gone from the catalog.
    retired_in_catalog = sorted(set(gen_docs.DEPRECATED_PRIMITIVES) & cur)
    unmarked = [n for n in retired_in_catalog if not entries.get(n, {}).get("deprecated")]
    if unmarked:
        errors.append(
            "deprecated names still registered without a deprecated flag "
            "(mark them, or finish removing them):\n" + "\n".join(f"  ? {n}" for n in unmarked)
        )

    deletes = sorted(n for n, e in entries.items() if e.get("tier") == "delete")
    called = [n for n in deletes if n in refs]
    if called:
        errors.append(
            "delete-tier primitives are still called from lib/std "
            "(retire the call site first, then delete):\n"
            + "\n".join(f"  x {n}: " + ", ".join(refs[n][:3]) for n in called)
        )

    if not errors:
        print(
            f"OK: retirement channel — {len(flagged)} deprecated name(s) flagged, "
            f"{len(gen_docs.DEPRECATED_PRIMITIVES) - len(retired_in_catalog)} retired "
            f"name(s) absent from the {len(cur)}-name catalog, "
            f"{len(deletes)} delete-tier name(s) unused by lib/std"
        )
    return errors


def report(tiers: dict, catalog: list[str]) -> int:
    """Print the evidence the follow-up deletion issues need."""
    entries: dict[str, dict] = tiers.get("primitives", {})
    refs = prim_surface.stdlib_references(catalog)
    unused = [n for n in catalog if n not in refs]
    by_tier: dict[str, list[str]] = {t: [] for t in VALID_TIERS}
    for n in catalog:
        t = entries.get(n, {}).get("tier")
        if t in by_tier:
            by_tier[t].append(n)
    print(f"catalog: {len(catalog)} names")
    for t in VALID_TIERS:
        print(f"  {t:18s} {len(by_tier[t])}")
    print(f"  referenced by lib/std: {len(catalog) - len(unused)}")
    print(f"  no lib/std caller    : {len(unused)}")
    for n in unused:
        print(f"    {n:42s} {entries.get(n, {}).get('tier', '?'):18s} {prim_surface.scan_sources()[n][0]}")
    return 0


# Issue #5136: names this cycle moved into the stdlib or deleted. A later
# registration of the same name would silently reopen the surface that #5131
# and #5137 closed, so the gate refuses it.
RETIRED_022 = frozenset(
    {
        # moved to std/char (#5137)
        "char-alphabetic?",
        "char-numeric?",
        "char-whitespace?",
        "char-upcase",
        "char-downcase",
        # moved to std/orch / deleted alias (#5131)
        "orch:metrics",
        "orch:parallel-intend",
    }
)

# The one-cycle K-EXC-5118 bucket is settled: its names are kernel-resident
# under K-ORCH-STATE, so the rule id must not come back.
FORBIDDEN_RULES_022 = frozenset({"K-EXC-5118"})

# Issue #5143: the qr: read aliases are dispatch-only too. (query :op) still
# reaches the body through the query verb, but the name has no public
# registration and no tier row. qr:ref-mutation-id / qr:ref-workspace-id are
# re-tier names and stay public until Batch 5 (#5144).
RETIRED_QR_ALIASES_022 = frozenset(
    {
        "qr:allow-macro-mutate?",
        "qr:api-reference",
        "qr:can-write?",
        "qr:concurrent-mutation-policy",
        "qr:conflicts-with",
        "qr:deopt-fn?",
        "qr:diff",
        "qr:fiber-backend",
        "qr:find-define",
        "qr:generation",
        "qr:get-provenance",
        "qr:ir-dependents",
        "qr:ir-dirty?",
        "qr:memory-limit",
        "qr:memory-used",
        "qr:mutation-log-diff",
        "qr:mutation-log-summary",
        "qr:ppa-reasons",
        "qr:protected?",
        "qr:reasons",
        "qr:ref-deserialize",
        "qr:ref-get",
        "qr:ref-serialize",
        "qr:ref-valid-id-gen",
        "qr:resolve-stable-ref",
        "qr:snapshot-fail-reason",
        "qr:stable-ref-pair",
        "qr:validate-nodes",
        "qr:validate-ownership",
        "qr:validate-post-restore",
    }
)

# Issue #5142: the mw: prefix aliases are dispatch-only. Their bodies stay in
# the primitive table under the same name, so (mutate :op) and (workspace :op)
# still resolve them, but the name is no longer a public registration: no slot,
# no PrimMeta, no api-reference entry, no tier row. Registering one with add()
# again would silently reopen the surface this batch closed.
RETIRED_MW_ALIASES_022 = frozenset(
    {
        "mw:arena-compact",
        "mw:arena-compact-with-policy",
        "mw:arena-request-defrag",
        "mw:auto-rollback-on-panic",
        "mw:closure-free",
        "mw:compact-env-frames",
        "mw:compact-nodes",
        "mw:define-strategy",
        "mw:define-struct",
        "mw:discard",
        "mw:exception-fibers-clear",
        "mw:hot-swap-fn",
        "mw:merge-3way",
        "mw:overlay-begin",
        "mw:overlay-commit",
        "mw:overlay-conflict",
        "mw:overlay-define",
        "mw:overlay-get",
        "mw:overlay-has",
        "mw:overlay-pending",
        "mw:overlay-report",
        "mw:overlay-rollback",
        "mw:overlay-set!",
        "mw:propagate-marker",
        "mw:recycle-nodes",
        "mw:restore",
        "mw:rollback-latest",
        "mw:rollback-to",
        "mw:scheduler-pin",
        "mw:self-heal-on-drift",
        "mw:session-create",
        "mw:set-allow-macro-mutate",
        "mw:set-auto-compact-threshold",
        "mw:set-concurrent-mutation-policy",
        "mw:set-marker",
        "mw:set-memory-limit",
        "mw:set-provenance",
        "mw:snapshot",
        "mw:string-pool-compact",
        "mw:sync-from",
        "mw:try-mutation",
        "mw:tune-gc-frequency",
        "mw:workspace-delete",
    }
)

# Issue #5144: same treatment for the mutate: write surface. Every body now
# registers with ev.primitives().add_hidden(), so (mutate :op) still resolves
# it through the primitive table while the name loses its slot, PrimMeta,
# api-reference entry and tier row. Re-registering one as public would silently
# reopen the surface this batch closed.
RETIRED_MUTATE_ALIASES_022 = frozenset(
    {
        "mutate:atomic-batch",
        "mutate:check",
        "mutate:check-stable-ref",
        "mutate:extract-function",
        "mutate:from-verification-feedback",
        "mutate:inline-call",
        "mutate:insert-child",
        "mutate:move-node",
        "mutate:query-and-replace",
        "mutate:query-and-replace-batch",
        "mutate:rebind",
        "mutate:record-patch",
        "mutate:refactor/extract",
        "mutate:remove-node",
        "mutate:rename-symbol",
        "mutate:replace-pattern",
        "mutate:replace-subtree",
        "mutate:replace-type",
        "mutate:replace-value",
        "mutate:request-gc-safepoint",
        "mutate:restore-hygiene-checkpoint",
        "mutate:rollback-macro-introduced",
        "mutate:save-hygiene-checkpoint",
        "mutate:set-agent-fingerprint",
        "mutate:set-body",
        "mutate:set-pattern-index-policy",
        "mutate:set-stale-ref-policy",
        "mutate:splice",
        "mutate:tweak-literal",
        "mutate:validate-reflected",
        "mutate:wrap",
    }
)


def check_retired_022(tiers: dict, catalog: set[str]) -> list[str]:
    errors: list[str] = []
    for name in sorted(RETIRED_022):
        if name in catalog:
            errors.append(
                f"{name} was retired in v0.2.2 (#5131/#5137) but is registered again — "
                "move the caller to the stdlib home instead of re-adding the primitive"
            )
        entry = tiers["primitives"].get(name)
        if entry is not None:
            errors.append(f"{name} was retired in v0.2.2 but still has a tier entry")
    # Issue #5142/#5143: a deregistered prefix alias must not come back as a
    # public registration or a tier row; its dispatch-only body has no surface.
    for retired, issue, verb in (
        (RETIRED_MW_ALIASES_022, "#5142", "(mutate :<op>)"),
        (RETIRED_QR_ALIASES_022, "#5143", "(query :<op>)"),
        (RETIRED_MUTATE_ALIASES_022, "#5144", "(mutate :<op>)"),
    ):
        for name in sorted(retired):
            if name in catalog:
                errors.append(
                    f"{name} was deregistered in v0.2.2 ({issue}); call "
                    f"{verb} instead of registering the prefix alias again"
                )
            if name in tiers["primitives"]:
                errors.append(f"{name} was deregistered in v0.2.2 ({issue}) but still has a tier entry")
    for name, entry in sorted(tiers["primitives"].items()):
        if entry.get("rule") in FORBIDDEN_RULES_022:
            errors.append(f"{name} still uses the settled rule {entry.get('rule')}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--update-baseline",
        action="store_true",
        help="Rewrite docs/generated/primitives-baseline.json from the current scan.",
    )
    ap.add_argument("--report", action="store_true", help="Print tier + lib/std usage evidence.")
    args = ap.parse_args()

    if not TIERS_PATH.exists():
        print(f"FAIL: missing {TIERS_PATH.relative_to(ROOT)}", file=sys.stderr)
        return 1

    tiers = load_tiers()
    catalog = prim_surface.scan_names()

    if args.update_baseline:
        write_baseline(catalog)

    if args.report:
        return report(tiers, catalog)

    errors = check_tiers(tiers, catalog)
    errors += check_growth(tiers, catalog)
    errors += check_retirement(tiers, catalog)
    errors += check_retired_022(tiers, catalog)

    if errors:
        print(f"FAIL: primitive tier contract (Issue #5008) — {len(errors)} problem(s)", file=sys.stderr)
        for i, err in enumerate(errors, 1):
            print(f"\n[{i}] {err}", file=sys.stderr)
        return 1

    print(
        f"OK: primitive tiers ({len(catalog)} names: "
        + ", ".join(f"{t} {sum(1 for e in tiers['primitives'].values() if e['tier'] == t)}" for t in VALID_TIERS)
        + ")"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
