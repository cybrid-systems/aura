#!/usr/bin/env python3
"""Classify root tests/test_*.cpp vs domain/ subdirs — probe, dedup, streamline plan.

Complements scripts/inventory_legacy_tests.py (#1957, issues/ + theme inventory).
This tool focuses on **root** binaries: theme placement, near-dup clusters,
overlap with existing domain suites, and recommended actions.

Usage:
  python3 scripts/classify_root_tests.py              # write report
  python3 scripts/classify_root_tests.py --check      # fail if report stale
  python3 scripts/classify_root_tests.py --json out.json

Output (default): tests/root_test_classification.md
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass, field
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
DEFAULT_OUT = TESTS / "root_test_classification.md"

# ── Themes (aligned with inventory_legacy_tests.py + domain suites) ─────────
THEME_ORDER = [
    "arena_compaction",
    "mutation_dirty",
    "fiber_orch",
    "linear_ownership",
    "edsl_hygiene",
    "jit_incremental",
    "shape_soa",
    "observability",
    "stdlib",
    "compiler_core",
    "uncategorized",
]

THEME_DEST: dict[str, str] = {
    "arena_compaction": "tests/domain/arena/ (batch pilots already live)",
    "mutation_dirty": "tests/domain/test_domain_typed_mutate.cpp + mutation_boundary_batch",
    "fiber_orch": "tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume_batch",
    "linear_ownership": "tests/test_linear_ownership_batch.cpp → domain/",
    "edsl_hygiene": "tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect_batch",
    "jit_incremental": "future domain/jit/ (heavy JIT stays EXCLUDE or root)",
    "shape_soa": "tests/test_soa_batch.cpp → domain/",
    "observability": "tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp",
    "stdlib": "tests/suite/ + focused root integration (datetime, hot-update)",
    "compiler_core": "keep root or future domain/compiler/",
    "uncategorized": "manual triage",
}

THEME_PATTERNS: dict[str, list[str]] = {
    "arena_compaction": [
        r"arena",
        r"compact",
        r"defrag",
        r"\bgc\b",
        r"auto.?compact",
        r"zero.?copy.?arena",
    ],
    "mutation_dirty": [
        r"mutat",
        r"dirty",
        r"atomic.?batch",
        r"stable.?ref",
        r"rollback",
        r"\bguard\b",
        r"invalidate",
        r"provenance",
        r"typed.?mut",
        r"occurrence",
        r"solve.?delta",
        r"defuse",
        r"blame",
        r"steal",
    ],
    "fiber_orch": [
        r"fiber",
        r"orch",
        r"quota",
        r"agent",
        r"intend",
        r"spawn",
        r"\bjoin\b",
        r"mailbox",
        r"parallel.?orch",
    ],
    "linear_ownership": [r"linear", r"ownership", r"borrow", r"walk.?active.?clos"],
    "edsl_hygiene": [
        r"hygiene",
        r"macro",
        r"marker",
        r"edsl",
        r"syntax.?marker",
        r"pattern.?hygiene",
    ],
    "jit_incremental": [
        r"\bjit\b",
        r"\baot\b",
        r"relower",
        r"incremental.?re",
        r"hot.?update",
        r"inline.?pass",
        r"reemit",
    ],
    "shape_soa": [r"\bsoa\b", r"shape", r"value.?encod", r"list.?vector", r"column"],
    "observability": [
        r"stats",
        r"metrics",
        r"schema",
        r"\bobs\b",
        r"closed.?loop",
        r"readiness",
        r"engine.?metrics",
        r"production.?sweep",
        r"production.?safety",
        r"production.?hardening",
    ],
    "stdlib": [
        r"stdlib",
        r"datetime",
        r"csv",
        r"module.?export",
        r"hot.?update.?stdlib",
        r"atomic.?swap.?stdlib",
    ],
    "compiler_core": [
        r"typecheck",
        r"bidirectional",
        r"constraint",
        r"compile",
        r"\bir_",
        r"envframe",
        r"closure",
        r"workspace",
        r"module.?boundary",
        r"capability",
        r"contract",
        r"\benv\b",
    ],
}

# Existing domain suites (coverage anchors for dedup)
DOMAIN_SUITES: dict[str, str] = {
    "test_domain_fiber_orchestration.cpp": "fiber_orch",
    "test_domain_hygiene_dirty.cpp": "edsl_hygiene",
    "test_domain_typed_mutate.cpp": "mutation_dirty",
    "test_obs_schema_matrix.cpp": "observability",
    "arena/test_arena_batch.cpp": "arena_compaction",
    "arena/test_arena_defrag_concurrent.cpp": "arena_compaction",
    "arena/test_compact_batch.cpp": "arena_compaction",
    "arena/test_compact_sweep_batch.cpp": "arena_compaction",
    "arena/test_gc_batch.cpp": "arena_compaction",
}

# Known supersession / alias pairs (later covers earlier or is preferred home).
# Empty Phase-2 stubs (test_fiber/mutation/observability/persist) were deleted
# in Wave 0 — do not re-add at tests/ root.
SUPERSEDED: dict[str, str] = {
    "test_open_issues_phase1_batch.cpp": "domain/test_obs_schema_matrix.cpp (alias / jit_late3 bundle; EXCLUDE_FROM_ALL)",
}


@dataclass
class RootTest:
    name: str
    lines: int
    theme: str
    action: str
    reason: str
    checks: int = 0
    schemas: list[int] = field(default_factory=list)
    queries: list[str] = field(default_factory=list)
    is_batch: bool = False
    is_stub: bool = False


def classify_theme(name: str, text: str) -> str:
    blob = name.lower() + " " + text[:4000].lower()
    scores: dict[str, int] = {}
    for theme, pats in THEME_PATTERNS.items():
        scores[theme] = sum(1 for p in pats if re.search(p, blob))
    best = max(scores, key=lambda t: (scores[t], -THEME_ORDER.index(t)))
    return best if scores[best] > 0 else "uncategorized"


def normalize_stem(name: str) -> str:
    n = name.removesuffix(".cpp")
    n = re.sub(r"_\d{3,4}$", "", n)
    n = re.sub(r"_task\d+$", "", n)
    n = re.sub(r"_closed_?loop.*$", "", n)
    n = re.sub(r"_reliability$", "", n)
    n = re.sub(r"_hotpath.*$", "", n)
    return n


def decide_action(name: str, text: str, lines: int, theme: str) -> tuple[str, str]:
    if name in SUPERSEDED:
        return "superseded_exclude", SUPERSEDED[name]
    if name.startswith("test_issue_"):
        return "relocate_or_domain", "misplaced test_issue_* at root — prefer domain suite, not issues/"
    if lines <= 22 and text.count("CHECK") < 2 and "placeholder" in text.lower():
        return "delete_stub", f"empty Phase-2 placeholder ({lines}L)"
    if lines <= 22 and "stub" in text.lower() and "main" not in text:
        return "keep_link_stub", f"link-only stub ({lines}L)"
    if name.endswith("_batch.cpp"):
        return "keep_batch_exclude", "family batch driver (EXCLUDE_FROM_ALL convention)"
    if re.search(r"production_sweep|production_safety|production_hardening", name):
        beh = bool(
            re.search(
                r"define-strategy|mutate:|set-code|resource:quota|terminal-|tui:",
                text,
            )
        )
        if not beh and lines < 120:
            return "fold_obs_fieldlist", "pure schema/flag production sweep → FieldListCase"
        return "keep_behavioral", "production family with behavioral extras (batch later)"
    if (
        lines < 150
        and re.search(r"query:[\w:-]+-stats", text)
        and text.count("CHECK") <= 20
        and not re.search(r"mutate:|set-code|spawn|fiber:", text)
    ):
        return "candidate_obs_fold", f"thin schema probe ({lines}L) — consider FieldListCase"
    return "keep", "no automatic streamline"


def scan_root() -> list[RootTest]:
    out: list[RootTest] = []
    for p in sorted((TESTS).glob("test_*.cpp")):
        text = p.read_text(errors="ignore")
        lines = len(text.splitlines())
        theme = classify_theme(p.name, text)
        action, reason = decide_action(p.name, text, lines, theme)
        schemas = [int(x) for x in re.findall(r"schema[^\n]{0,40}==\s*(\d{3,4})", text)]
        schemas += [int(x) for x in re.findall(r"schema\s+(\d{3,4})", text[:500])]
        queries = sorted(set(re.findall(r"query:[\w:-]+-stats", text)))
        out.append(
            RootTest(
                name=p.name,
                lines=lines,
                theme=theme,
                action=action,
                reason=reason,
                checks=text.count("CHECK"),
                schemas=sorted(set(schemas))[:8],
                queries=queries[:6],
                is_batch=p.name.endswith("_batch.cpp"),
                is_stub=action in ("delete_stub", "superseded_exclude") and lines <= 30,
            )
        )
    return out


def near_dup_clusters(tests: list[RootTest]) -> dict[str, list[str]]:
    clusters: dict[str, list[str]] = defaultdict(list)
    for t in tests:
        clusters[normalize_stem(t.name)].append(t.name)
    return {k: v for k, v in clusters.items() if len(v) >= 2}


def domain_inventory() -> list[str]:
    paths = sorted((TESTS / "domain").rglob("test_*.cpp"))
    return [str(p.relative_to(TESTS)) for p in paths]


def content_hash(rows: list[RootTest]) -> str:
    blob = json.dumps([asdict(r) for r in rows], sort_keys=True, default=str)
    return hashlib.sha256(blob.encode()).hexdigest()[:16]


def render_md(rows: list[RootTest]) -> str:
    by_theme = Counter(r.theme for r in rows)
    by_action = Counter(r.action for r in rows)
    clusters = near_dup_clusters(rows)
    domain = domain_inventory()
    h = content_hash(rows)

    lines: list[str] = []
    lines.append("# Root test classification")
    lines.append("")
    lines.append(f"**Generated:** {date.today().isoformat()} by `scripts/classify_root_tests.py`")
    lines.append(f"**Content hash:** `{h}`")
    lines.append("**Companion:** [`legacy_test_inventory.md`](legacy_test_inventory.md) (#1957 issues/)")
    lines.append("")
    lines.append("## Purpose")
    lines.append("")
    lines.append(
        "Probe `tests/test_*.cpp` (root), classify into theme buckets that match "
        "`tests/domain/` and family batches, flag near-dups / supersessions, and "
        "drive streamline waves **without losing coverage**."
    )
    lines.append("")
    lines.append("## Snapshot")
    lines.append("")
    lines.append("| Location | Count |")
    lines.append("|----------|------:|")
    lines.append(f"| `tests/test_*.cpp` (root) | {len(rows)} |")
    lines.append(f"| `tests/domain/**/test_*.cpp` | {len(domain)} |")
    lines.append(f"| Near-dup name clusters (≥2) | {len(clusters)} |")
    lines.append("")
    lines.append("### Domain suite anchors (coverage homes)")
    lines.append("")
    for rel, theme in DOMAIN_SUITES.items():
        lines.append(f"- `{rel}` → **{theme}** — dest: `{THEME_DEST[theme]}`")
    lines.append("")
    lines.append("## Theme distribution (root)")
    lines.append("")
    lines.append("| Theme | Count | Preferred destination |")
    lines.append("|-------|------:|------------------------|")
    for theme in THEME_ORDER:
        c = by_theme.get(theme, 0)
        if c:
            lines.append(f"| `{theme}` | {c} | {THEME_DEST[theme]} |")
    lines.append("")
    lines.append("## Action summary (streamline plan)")
    lines.append("")
    lines.append("| Action | Count | Meaning |")
    lines.append("|--------|------:|---------|")
    action_help = {
        "keep": "Retain root binary for now",
        "keep_batch_exclude": "Family batch; already EXCLUDE_FROM_ALL convention",
        "keep_behavioral": "Has behavioral ACs beyond schema flags",
        "candidate_obs_fold": "Thin schema probe → fold into obs matrix cases",
        "fold_obs_fieldlist": "Pure production flag gate → FieldListCase",
        "superseded_exclude": "Covered by domain suite / later issue — exclude or delete",
        "delete_stub": "Empty placeholder — remove CMake + source",
        "keep_link_stub": "Link-only helper (not a test suite)",
        "relocate_or_domain": "Root `test_issue_*` — promote ACs into domain, do not grow issues/",
    }
    for action, count in sorted(by_action.items(), key=lambda x: -x[1]):
        lines.append(f"| `{action}` | {count} | {action_help.get(action, '')} |")
    lines.append("")
    lines.append("## Wave status (streamline implementation)")
    lines.append("")
    lines.append("| Wave | Status | What shipped |")
    lines.append("|------|--------|--------------|")
    lines.append("| 0 | **done** | Empty Phase-2 stubs deleted; `open_issues_phase1_batch` EXCLUDE |")
    lines.append("| 1 | **done** | Thin probes → `obs_schema_cases.hpp` FieldList; selfevo/stdlib EXCLUDE |")
    lines.append("| 2 | **done** | `test_domain_production_sweep` + `production_sweep_cases.hpp`; ~27 prod EXCLUDE |")
    lines.append("| 3 | **done** | Near-dup supersession EXCLUDE (1636, fine_dirty, 1622, …) |")
    lines.append("| 4 | **done** | Root `test_issue_1943…1956` → `tests/domain/` |")
    lines.append("")
    lines.append("Prefer **extend domain/** over new root binaries (see tests/README.md).")
    lines.append("")
    lines.append("## Near-dup name clusters")
    lines.append("")
    lines.append(
        "Name-normalized groups (strip issue suffix / task / closed_loop). "
        "Not always redundant — inspect AC headers before merging."
    )
    lines.append("")
    for key, members in sorted(clusters.items(), key=lambda x: -len(x[1])):
        lines.append(f"### `{key}` ({len(members)})")
        for m in members:
            row = next(r for r in rows if r.name == m)
            lines.append(f"- `{m}` — theme=`{row.theme}` action=`{row.action}` ({row.lines}L)")
        lines.append("")
    lines.append("## Superseded / alias map")
    lines.append("")
    lines.append("| Root file | Prefer |")
    lines.append("|-----------|--------|")
    for src, dst in sorted(SUPERSEDED.items()):
        lines.append(f"| `{src}` | {dst} |")
    lines.append("")
    lines.append("## Per-theme file list (root)")
    lines.append("")
    by_t: dict[str, list[RootTest]] = defaultdict(list)
    for r in rows:
        by_t[r.theme].append(r)
    for theme in THEME_ORDER:
        files = by_t.get(theme, [])
        if not files:
            continue
        lines.append(f"### {theme} ({len(files)})")
        lines.append("")
        lines.append("| File | Lines | Action | Notes |")
        lines.append("|------|------:|--------|-------|")
        for r in sorted(files, key=lambda x: x.name):
            note = r.reason.replace("|", "/")[:80]
            lines.append(f"| `{r.name}` | {r.lines} | `{r.action}` | {note} |")
        lines.append("")
    lines.append("## Streamline roadmap (historical)")
    lines.append("")
    lines.append("Waves 0–4 applied — see **Wave status** above. Further reductions:")
    lines.append("fold remaining `candidate_obs_fold` keepers; rename `domain/test_issue_*`")
    lines.append("to `test_domain_<theme>_*.cpp`; promote more root keeps into theme suites.")
    lines.append("")
    lines.append("## Related")
    lines.append("")
    lines.append("- Policy: [`tests/README.md`](README.md)")
    lines.append("- Domain rules: [`domain/README.md`](domain/README.md)")
    lines.append("- Issues inventory: [`legacy_test_inventory.md`](legacy_test_inventory.md)")
    lines.append("- Coarse 5-bucket map: [`domain_classification.md`](domain_classification.md)")
    lines.append("")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="fail if report is stale")
    ap.add_argument("--json", type=Path, default=None, help="also write JSON dump")
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    rows = scan_root()
    md = render_md(rows)
    h = content_hash(rows)

    if args.json:
        args.json.write_text(
            json.dumps(
                {
                    "hash": h,
                    "count": len(rows),
                    "tests": [asdict(r) for r in rows],
                    "clusters": near_dup_clusters(rows),
                    "domain": domain_inventory(),
                },
                indent=2,
            )
            + "\n"
        )

    if args.check:
        if not args.output.exists():
            print(f"FAIL: missing {args.output}", file=sys.stderr)
            return 1
        old = args.output.read_text()
        m = re.search(r"\*\*Content hash:\*\* `([0-9a-f]+)`", old)
        if not m or m.group(1) != h:
            print(
                f"FAIL: {args.output} stale (hash {m.group(1) if m else '?'} → {h}). "
                f"Re-run: python3 scripts/classify_root_tests.py",
                file=sys.stderr,
            )
            return 1
        print(f"OK: {args.output} up to date (hash {h}, {len(rows)} root tests)")
        return 0

    args.output.write_text(md)
    by_action = Counter(r.action for r in rows)
    print(f"wrote {args.output} ({len(rows)} root tests, hash {h})")
    print("actions:", dict(by_action))
    return 0


if __name__ == "__main__":
    sys.exit(main())
